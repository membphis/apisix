/*
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements. See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "engine.h"
#include <math.h>

struct ac_radix_node_s {
    ngx_str_t edge;
    ac_radix_node_t *child;
    ac_radix_node_t *sibling;
    ngx_array_t entries;
};

static void ac_build_index(lua_State *L, ac_main_conf_t *main);

void
ac_variable(lua_State *L, ac_request_t *ctx, ngx_str_t name)
{
    ngx_http_variable_value_t *value;
    int table;
    ac_push(L, ctx->table_ref);
    table = lua_gettop(L);
    lua_getfield(L, table, "var");
    if (lua_istable(L, -1)) {
        lua_pushlstring(L, (char *) name.data, name.len);
        lua_gettable(L, -2);
        lua_replace(L, table);
        lua_settop(L, table);
        return;
    }
    lua_pop(L, 2);
    value = ngx_http_get_variable(ctx->request, &name,
                                 ngx_hash_key(name.data, name.len));
    if (value == NULL || value->not_found) {
        lua_pushnil(L);
    } else {
        lua_pushlstring(L, (char *) value->data, value->len);
    }
}

static ngx_uint_t
ac_method(lua_State *L, int index)
{
    static struct { const char *name; ngx_uint_t mask; } methods[] = {
        {"GET", NGX_HTTP_GET}, {"HEAD", NGX_HTTP_HEAD}, {"POST", NGX_HTTP_POST},
        {"PUT", NGX_HTTP_PUT}, {"DELETE", NGX_HTTP_DELETE}, {"OPTIONS", NGX_HTTP_OPTIONS},
        {"PATCH", NGX_HTTP_PATCH}, {"TRACE", NGX_HTTP_TRACE}, {"CONNECT", NGX_HTTP_CONNECT}
    };
    ngx_uint_t i;
    const char *value = luaL_checkstring(L, index);
    for (i = 0; i < sizeof(methods) / sizeof(methods[0]); i++) {
        if (ngx_strcmp(methods[i].name, value) == 0) return methods[i].mask;
    }
    luaL_error(L, "invalid route method '%s'", value);
    return 0;
}

static void
ac_strings(lua_State *L, ngx_pool_t *pool, ngx_array_t *array, int value)
{
    ngx_str_t *item;
    ngx_uint_t i, count;
    value = ac_abs(L, value);
    count = lua_istable(L, value) ? lua_objlen(L, value) : (lua_isnil(L, value) ? 0 : 1);
    if (ngx_array_init(array, pool, count ? count : 1, sizeof(ngx_str_t)) != NGX_OK) {
        luaL_error(L, "route string array allocation failed");
    }
    for (i = 1; i <= count; i++) {
        if (lua_istable(L, value)) lua_rawgeti(L, value, (int) i);
        else lua_pushvalue(L, value);
        item = ngx_array_push(array);
        if (item == NULL) luaL_error(L, "route string allocation failed");
        *item = ac_string(L, pool, -1);
        lua_pop(L, 1);
    }
}

static void
ac_compile_path(lua_State *L, ac_main_conf_t *main, ac_path_t *path, ngx_str_t source)
{
    size_t i, start;
    ngx_flag_t needs_regex = 0;
    u_char *pattern, *write;
    ngx_str_t *name;
    ngx_regex_compile_t rc;
    u_char error[NGX_MAX_CONF_ERRSTR];
    ngx_memzero(path, sizeof(*path));
    path->source = source;
    path->prefix = source;
    path->exact = 1;
    if (source.len == 0 || source.data[0] != '/') {
        luaL_error(L, "route URI must start with '/' ");
    }
    if (ngx_array_init(&path->names, main->pool, 2, sizeof(ngx_str_t)) != NGX_OK) {
        luaL_error(L, "route captures allocation failed");
    }
    for (i = 0; i < source.len; i++) {
        if (source.data[i] == '*' || (source.data[i] == ':' && main->parameter_router)) {
            path->prefix.len = i;
            path->exact = 0;
            needs_regex = source.data[i] == ':' || i + 1 < source.len;
            break;
        }
    }
    if (!needs_regex) return;
    if (source.len > (NGX_MAX_SIZE_T_VALUE - 1) / 10) {
        luaL_error(L, "native route pattern is too long");
    }
    pattern = ngx_pnalloc(main->pool, source.len * 10 + 1);
    if (pattern == NULL) luaL_error(L, "route regex allocation failed");
    write = pattern;
    for (i = 0; i < source.len;) {
        if ((i == 0 || source.data[i - 1] == '/')
            && (source.data[i] == ':' || source.data[i] == '*')) {
            start = i++;
            while (i < source.len && source.data[i] != '/') i++;
            name = ngx_array_push(&path->names);
            if (name == NULL) luaL_error(L, "route capture name allocation failed");
            name->data = source.data + start + 1;
            name->len = i - start - 1;
            if (source.data[start] == '*') {
                if (name->len == 0) {
                    ngx_str_set(name, ":ext");
                }
                write = ngx_cpymem(write, "((.|\n)*)", sizeof("((.|\n)*)") - 1);
            } else {
                write = ngx_cpymem(write, "([^/]+)", sizeof("([^/]+)") - 1);
            }
        } else {
            *write++ = source.data[i++];
        }
    }
    *write = '\0';
    ngx_memzero(&rc, sizeof(rc));
    rc.pool = main->pool;
    rc.pattern.data = pattern;
    rc.pattern.len = write - pattern;
    rc.err.data = error;
    rc.err.len = sizeof(error);
    if (ngx_regex_compile(&rc) != NGX_OK) luaL_error(L, "invalid route regex: %s", error);
    path->regex = rc.regex;
    path->captures = rc.captures;
}

void
ac_compile_routes(lua_State *L, ac_main_conf_t *main, int config)
{
    ngx_uint_t i, j;
    int routes, value, paths, methods;
    ac_route_t *route;
    ac_path_t *path;
    ngx_array_t strings;
    ngx_str_t *items;
    ngx_cidr_t *cidr;
    const char *mode;
    config = ac_abs(L, config);
    lua_getfield(L, config, "router");
    mode = lua_isstring(L, -1) ? lua_tostring(L, -1) : "radixtree_host_uri";
    main->host_router = ngx_strcmp(mode, "radixtree_host_uri") == 0;
    main->parameter_router = ngx_strcmp(mode, "radixtree_uri_with_parameter") == 0;
    if (!main->host_router && !main->parameter_router && ngx_strcmp(mode, "radixtree_uri") != 0) {
        luaL_error(L, "unknown native HTTP router '%s'", mode);
    }
    lua_pop(L, 1);
    if (ngx_array_init(&main->routes, main->pool, 16, sizeof(ac_route_t)) != NGX_OK) {
        luaL_error(L, "native routes allocation failed");
    }
    ac_resource(L, main, "routes"); lua_getfield(L, -1, "values"); lua_remove(L, -2);
    routes = lua_gettop(L);
    luaL_checktype(L, routes, LUA_TTABLE);
    for (i = 1; i <= lua_objlen(L, routes); i++) {
        lua_rawgeti(L, routes, (int) i);
        lua_getfield(L, -1, "value"); lua_remove(L, -2);
        value = lua_gettop(L);
        luaL_checktype(L, value, LUA_TTABLE);
        lua_getfield(L, value, "status");
        if (lua_isnumber(L, -1) && lua_tonumber(L, -1) == 0) {
            lua_pop(L, 2);
            continue;
        }
        lua_pop(L, 1);
        route = ngx_array_push(&main->routes);
        if (route == NULL) luaL_error(L, "route allocation failed");
        ngx_memzero(route, sizeof(*route));
        route->value_ref = ac_ref(L, value);
        route->vars_ref = LUA_NOREF;
        route->filter_ref = LUA_NOREF;
        route->service_ref = LUA_NOREF;
        lua_getfield(L, value, "id");
        route->id = ac_string(L, main->pool, -1);
        lua_pop(L, 1);
        lua_getfield(L, value, "priority");
        route->priority = lua_isnil(L, -1) ? 0 : luaL_checkinteger(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, value, "uris");
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            lua_getfield(L, value, "uri");
        }
        paths = lua_gettop(L);
        ac_strings(L, main->pool, &strings, paths);
        items = strings.elts;
        if (strings.nelts == 0 || ngx_array_init(&route->paths, main->pool,
                strings.nelts, sizeof(ac_path_t)) != NGX_OK) {
            luaL_error(L, "native route requires at least one URI");
        }
        for (j = 0; j < strings.nelts; j++) {
            path = ngx_array_push(&route->paths);
            if (path == NULL) luaL_error(L, "native route path allocation failed");
            ac_compile_path(L, main, path, items[j]);
        }
        lua_pop(L, 1);
        lua_getfield(L, value, "hosts");
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            lua_getfield(L, value, "host");
        }
        ac_strings(L, main->pool, &route->hosts, -1);
        items = route->hosts.elts;
        for (j = 0; j < route->hosts.nelts; j++) {
            ngx_strlow(items[j].data, items[j].data, items[j].len);
        }
        lua_pop(L, 1);
        lua_getfield(L, value, "methods");
        methods = lua_gettop(L);
        if (lua_istable(L, methods)) {
            for (j = 1; j <= lua_objlen(L, methods); j++) {
                lua_rawgeti(L, methods, (int) j);
                route->methods |= ac_method(L, -1);
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
        lua_getfield(L, value, "remote_addrs");
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            lua_getfield(L, value, "remote_addr");
        }
        ac_strings(L, main->pool, &strings, -1);
        if (ngx_array_init(&route->cidrs, main->pool,
                           strings.nelts ? strings.nelts : 1, sizeof(ngx_cidr_t)) != NGX_OK) {
            luaL_error(L, "native route CIDR allocation failed");
        }
        items = strings.elts;
        for (j = 0; j < strings.nelts; j++) {
            cidr = ngx_array_push(&route->cidrs);
            if (cidr == NULL || ngx_ptocidr(&items[j], cidr) == NGX_ERROR) {
                luaL_error(L, "invalid native route remote address");
            }
        }
        lua_pop(L, 1);
        lua_getfield(L, value, "vars");
        route->vars_ref = ac_ref(L, -1);
        if (!lua_isnil(L, -1)) route->expression = ac_expr_compile(L, main->pool, -1);
        lua_pop(L, 1);
        lua_getfield(L, value, "filter_func");
        if (!lua_isnil(L, -1)) {
            lua_pushliteral(L, "return ");
            lua_pushvalue(L, -2);
            lua_concat(L, 2);
            size_t length;
            const char *source = lua_tolstring(L, -1, &length);
            if (luaL_loadbuffer(L, source, length, "native_route_filter") != 0
                || lua_pcall(L, 0, 1, 0) != 0) {
                luaL_error(L, "invalid native route filter: %s", lua_tostring(L, -1));
            }
            luaL_checktype(L, -1, LUA_TFUNCTION);
            route->filter_ref = ac_ref(L, -1);
            lua_pop(L, 2);
        }
        lua_pop(L, 1);
        lua_getfield(L, value, "id");
        ac_resource_get(L, main, "routes", -1);
        lua_remove(L, -2);
        ac_resolve_route(L, main, route, -1);
        ac_prepare_route_metadata(L, main, route);
        ac_upstream_compile(L, main, route);
        ac_push(L, route->value_ref);
        lua_getfield(L, -1, "plugins");
        ac_compile_plan(L, main, &route->plan, main->pool, -1, 1);
        lua_pop(L, 2);
        lua_pop(L, 2);
    }
    lua_pop(L, 1);
    ac_build_index(L, main);
}

static ngx_int_t
ac_host_rank(ac_main_conf_t *main, ac_route_t *route, ngx_http_request_t *request,
    ngx_str_t *matched)
{
    ngx_str_t *hosts = route->hosts.elts, host = request->headers_in.server;
    ngx_str_t candidate;
    ngx_uint_t i;
    ngx_int_t best = -1, rank;
    if (route->hosts.nelts == 0) return 0;
    for (i = 0; i < route->hosts.nelts; i++) {
        candidate = hosts[i];
        host = request->headers_in.server;
        if (!main->host_router && ngx_strlchr(candidate.data,
                candidate.data + candidate.len, ':') && request->headers_in.host) {
            host = request->headers_in.host->value;
        }
        if (candidate.len && candidate.data[0] == '*') {
            if (host.len < candidate.len - 1 || ngx_strncasecmp(
                    host.data + host.len - (candidate.len - 1), candidate.data + 1,
                    candidate.len - 1) != 0) continue;
            rank = (ngx_int_t) candidate.len;
        } else {
            if (candidate.len != host.len
                || ngx_strncasecmp(host.data, candidate.data, host.len) != 0) continue;
            rank = (ngx_int_t) candidate.len + 65536;
        }
        if (rank > best) {
            best = rank;
            *matched = candidate;
        }
    }
    return best;
}

static ngx_flag_t
ac_path_matches(ac_path_t *path, ac_request_t *ctx, int **captures)
{
    ngx_int_t count;
    if (path->exact) {
        return path->source.len == ctx->routing_uri.len
            && ngx_memcmp(path->source.data, ctx->routing_uri.data, ctx->routing_uri.len) == 0;
    }
    if (ctx->routing_uri.len < path->prefix.len
        || ngx_memcmp(ctx->routing_uri.data, path->prefix.data, path->prefix.len) != 0) return 0;
    if (path->regex == NULL) return 1;
    *captures = ngx_palloc(ctx->request->pool, (path->captures + 1) * 3 * sizeof(int));
    if (*captures == NULL) return 0;
    count = ngx_regex_exec(path->regex, &ctx->routing_uri, *captures,
                           (path->captures + 1) * 3);
    return count >= 0 && (*captures)[0] == 0
           && (size_t) (*captures)[1] == ctx->routing_uri.len;
}

static ac_radix_node_t *
ac_index_node(lua_State *L, ac_main_conf_t *main, ngx_str_t edge)
{
    ac_radix_node_t *node = ngx_pcalloc(main->pool, sizeof(*node));
    if (node == NULL || ngx_array_init(&node->entries, main->pool, 1,
                                        sizeof(ac_candidate_t)) != NGX_OK) {
        luaL_error(L, "native route index allocation failed");
    }
    node->edge = edge;
    return node;
}

static ac_radix_node_t *
ac_index_insert(lua_State *L, ac_main_conf_t *main, ac_radix_node_t *parent,
    ngx_str_t key)
{
    ac_radix_node_t *node, *split;
    ngx_str_t suffix;
    size_t common;
    if (key.len == 0) return parent;
    for (node = parent->child; node; node = node->sibling) {
        if (node->edge.data[0] != key.data[0]) continue;
        for (common = 1; common < node->edge.len && common < key.len; common++) {
            if (node->edge.data[common] != key.data[common]) break;
        }
        if (common < node->edge.len) {
            suffix.data = node->edge.data + common;
            suffix.len = node->edge.len - common;
            split = ac_index_node(L, main, suffix);
            split->child = node->child;
            split->entries = node->entries;
            node->edge.len = common;
            node->child = split;
            if (ngx_array_init(&node->entries, main->pool, 1,
                               sizeof(ac_candidate_t)) != NGX_OK) {
                luaL_error(L, "native route branch allocation failed");
            }
        }
        key.data += common;
        key.len -= common;
        return ac_index_insert(L, main, node, key);
    }
    node = ac_index_node(L, main, key);
    node->sibling = parent->child;
    parent->child = node;
    return node;
}

static void
ac_build_index(lua_State *L, ac_main_conf_t *main)
{
    ac_route_t *routes = main->routes.elts;
    ac_path_t *paths;
    ac_radix_node_t *node;
    ac_candidate_t *entry;
    ngx_str_t empty = ngx_null_string;
    ngx_uint_t i, j, sequence = 0;
    main->route_index = ac_index_node(L, main, empty);
    for (i = 0; i < main->routes.nelts; i++) {
        paths = routes[i].paths.elts;
        for (j = 0; j < routes[i].paths.nelts; j++) {
            node = ac_index_insert(L, main, main->route_index, paths[j].prefix);
            entry = ngx_array_push(&node->entries);
            if (entry == NULL) luaL_error(L, "native route entry allocation failed");
            ngx_memzero(entry, sizeof(*entry));
            entry->route = &routes[i];
            entry->path = &paths[j];
            entry->sequence = sequence++;
        }
    }
}

static int
ac_candidate_order(const void *a, const void *b)
{
    const ac_candidate_t *left = a, *right = b;
    if (left->host_rank != right->host_rank) return left->host_rank > right->host_rank ? -1 : 1;
    if (left->path->exact != right->path->exact) return left->path->exact ? -1 : 1;
    if (left->path->prefix.len != right->path->prefix.len) {
        return left->path->prefix.len > right->path->prefix.len ? -1 : 1;
    }
    if (left->route->priority != right->route->priority) return left->route->priority > right->route->priority ? -1 : 1;
    if (left->path->source.len != right->path->source.len) return left->path->source.len > right->path->source.len ? -1 : 1;
    if (left->sequence == right->sequence) return 0;
    return left->sequence < right->sequence ? -1 : 1;
}

static void
ac_collect_candidates(lua_State *L, ac_request_t *ctx, ac_radix_node_t *node)
{
    ac_candidate_t *entries = node->entries.elts, *entry;
    ngx_uint_t i;
    ngx_int_t rank;
    ngx_str_t host;
    ngx_http_request_t *r = ctx->request;
    for (i = 0; i < node->entries.nelts; i++) {
        if (entries[i].path->exact && entries[i].path->source.len != ctx->routing_uri.len) continue;
        host.len = 0;
        host.data = NULL;
        rank = ac_host_rank(ctx->main, entries[i].route, r, &host);
        if (rank < 0) continue;
        entry = ngx_array_push(&ctx->candidates);
        if (entry == NULL) luaL_error(L, "request route candidate allocation failed");
        *entry = entries[i];
        entry->host_rank = ctx->main->host_router ? rank : 0;
        entry->host = host;
    }
}

void
ac_match_start(lua_State *L, ac_request_t *ctx)
{
    ac_radix_node_t *node = ctx->main->route_index, *child;
    ngx_str_t key = ctx->routing_uri;
    ngx_uint_t offset = 0;
    int opts, table;
    ctx->route = NULL;
    ctx->candidate_cursor = 0;
    ctx->routing = 1;
    if (ngx_array_init(&ctx->candidates, ctx->request->pool, 4,
                       sizeof(ac_candidate_t)) != NGX_OK) {
        luaL_error(L, "request route candidates allocation failed");
    }
    ac_collect_candidates(L, ctx, node);
    while (offset < key.len) {
        for (child = node->child; child; child = child->sibling) {
            if (child->edge.len <= key.len - offset && ngx_memcmp(child->edge.data,
                        key.data + offset, child->edge.len) == 0) break;
        }
        if (child == NULL) break;
        offset += child->edge.len;
        node = child;
        ac_collect_candidates(L, ctx, node);
    }
    ngx_qsort(ctx->candidates.elts, ctx->candidates.nelts, sizeof(ac_candidate_t), ac_candidate_order);
    ac_push(L, ctx->table_ref);
    table = lua_gettop(L);
    lua_newtable(L);
    opts = lua_gettop(L);
    ac_set_string(L, opts, "method", ctx->request->method_name);
    ac_set_string(L, opts, "host", ctx->request->headers_in.server);
    lua_getfield(L, table, "var");
    lua_pushvalue(L, -1);
    lua_setfield(L, opts, "vars");
    lua_getfield(L, -1, "remote_addr");
    lua_setfield(L, opts, "remote_addr");
    lua_pop(L, 1);
    lua_newtable(L);
    lua_setfield(L, opts, "matched");
    ctx->match_opts_ref = ac_ref(L, opts);
    lua_pop(L, 2);
}

static void
ac_accept_route(lua_State *L, ac_request_t *ctx, ac_candidate_t *candidate, int *captures)
{
    ac_route_t *route = candidate->route;
    ac_path_t *path = candidate->path;
    ngx_str_t *names = path->names.elts;
    ngx_uint_t k;
    int table, opts, matched;
    ctx->path = path;
    ctx->route = route;
    ctx->metadata = route->metadata;
    ctx->routing = 0;
    ctx->early_status = route->resolve_status;
    ac_push(L, ctx->table_ref);
    table = lua_gettop(L);
    ac_push(L, ctx->match_opts_ref);
    opts = lua_gettop(L);
    lua_getfield(L, opts, "matched");
    matched = lua_gettop(L);
    ac_set_string(L, matched, "_path", path->source);
    if (route->hosts.nelts) ac_set_string(L, matched, "_host", candidate->host);
    ac_set_string(L, matched, "_method", ctx->request->method_name);
    if (captures != NULL) {
        for (k = 0; k < path->names.nelts; k++) {
            int start = captures[(k + 1) * 2], end = captures[(k + 1) * 2 + 1];
            if (start < 0 || end < start) continue;
            lua_pushlstring(L, (char *) names[k].data, names[k].len);
            lua_pushlstring(L, (char *) ctx->routing_uri.data + start, end - start);
            lua_rawset(L, matched);
        }
    }
    lua_pushvalue(L, matched);
    lua_setfield(L, table, "curr_req_matched");
    ac_push(L, route->wrapper_ref);
    lua_setfield(L, table, "matched_route");
    ac_push(L, route->value_ref);
    lua_getfield(L, -1, "id");
    lua_setfield(L, table, "route_id");
    lua_pop(L, 4);
    ac_route_context(L, ctx, 0);
    ac_http_match_finish(L, ctx);
}

void
ac_match_complete(lua_State *L, ac_request_t *ctx, ngx_flag_t accepted)
{
    int *captures = NULL;
    ac_candidate_t *candidates = ctx->candidates.elts;
    ac_candidate_t *candidate = &candidates[ctx->candidate_cursor - 1];
    if (accepted && ac_path_matches(candidate->path, ctx, &captures)) {
        ac_accept_route(L, ctx, candidate, captures);
    }
}

int
ac_match_next(lua_State *L, ac_request_t *ctx)
{
    ac_candidate_t *candidates = ctx->candidates.elts, *candidate;
    ac_route_t *route;
    int opts;
    while (ctx->candidate_cursor < ctx->candidates.nelts) {
        candidate = &candidates[ctx->candidate_cursor++];
        route = candidate->route;
        if (route->methods && !(route->methods & ctx->request->method)) continue;
        if (route->cidrs.nelts && ngx_cidr_match(ctx->request->connection->sockaddr, &route->cidrs) != NGX_OK) continue;
        if (route->vars_ref != LUA_REFNIL && route->vars_ref != LUA_NOREF) {
            ngx_flag_t matches;
            matches = ac_expr_eval(L, route->expression, ctx);
            if (!matches) continue;
        }
        if (route->filter_ref != LUA_NOREF) {
            ac_push(L, ctx->match_opts_ref);
            opts = lua_gettop(L);
            lua_getfield(L, opts, "matched");
            ac_set_string(L, -1, "_method", ctx->request->method_name);
            if (route->hosts.nelts) ac_set_string(L, -1, "_host", candidate->host);
            lua_pop(L, 1);
            ac_push(L, route->filter_ref);
            lua_getfield(L, opts, "vars");
            lua_pushvalue(L, opts);
            ac_push(L, ctx->table_ref);
            lua_pushvalue(L, opts);
            ctx->callback_pending = 1;
            ctx->filter_pending = 1;
            return 5;
        }
        ac_match_complete(L, ctx, 1);
        if (!ctx->routing) return 0;
    }
    ctx->routing = 0;
    ac_http_match_finish(L, ctx);
    return 0;
}
