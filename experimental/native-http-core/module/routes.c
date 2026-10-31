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

/* Diagnostic build only. The normal binary has no timers, counters or branch
 * checks from this block. These are thread-CPU intervals, not profiler self
 * times: Lua filter callbacks run outside the measured regions. */
#ifdef AC_ROUTE_PROFILE
#include <stdio.h>
#include <time.h>

enum {
    AC_RP_COLLECT_SORT, AC_RP_OPTIONS, AC_RP_ELIGIBILITY_EXPR,
    AC_RP_PATH_MATCH, AC_RP_ACCEPT_CONTEXT, AC_RP_SCOPES
};
static const char *ac_route_profile_names[] = {
    "collect_sort", "options", "eligibility_expr", "path_match", "accept_context"
};
static struct { uint64_t calls, raw_ns; } ac_route_profile_scopes[AC_RP_SCOPES];
static uint64_t ac_route_profile_started, ac_route_profile_empty_ns,
    ac_route_profile_pair_ns;
static ngx_flag_t ac_route_profile_active;

static uint64_t
ac_route_profile_clock(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now) != 0) return 0;
    return (uint64_t) now.tv_sec * 1000000000ULL + (uint64_t) now.tv_nsec;
}

static void
ac_route_profile_record(ngx_uint_t scope, uint64_t started)
{
    uint64_t ended;
    if (!started || !ac_route_profile_active) return;
    ended = ac_route_profile_clock();
    if (ended < started) return;
    ac_route_profile_scopes[scope].calls++;
    ac_route_profile_scopes[scope].raw_ns += ended - started;
}

int
ac_route_profile_start(void)
{
    uint64_t began, before, after, empty = 0;
    ngx_uint_t i;
    if (ac_route_profile_active) return -1;
    began = ac_route_profile_clock();
    if (!began) return -1;
    for (i = 0; i < 10000; i++) {
        before = ac_route_profile_clock();
        after = ac_route_profile_clock();
        if (!before || after < before) return -1;
        empty += after - before;
    }
    ac_route_profile_pair_ns = (ac_route_profile_clock() - began) / 10000;
    ac_route_profile_empty_ns = empty / 10000;
    ngx_memzero(ac_route_profile_scopes, sizeof(ac_route_profile_scopes));
    ac_route_profile_started = ac_route_profile_clock();
    ac_route_profile_active = 1;
    return 0;
}

int
ac_route_profile_stop(const char *path)
{
    uint64_t ended = ac_route_profile_clock(), correction;
    ngx_uint_t i;
    FILE *file;
    if (!ac_route_profile_active || ended < ac_route_profile_started) return -1;
    ac_route_profile_active = 0;
    file = fopen(path, "w");
    if (!file) return -1;
    fprintf(file, "{\"thread_cpu_ns\":%llu,\"calibration_iterations\":10000,"
            "\"empty_interval_mean_ns\":%llu,\"clock_pair_loop_mean_ns\":%llu,\"scopes\":{",
            (unsigned long long) (ended - ac_route_profile_started),
            (unsigned long long) ac_route_profile_empty_ns,
            (unsigned long long) ac_route_profile_pair_ns);
    for (i = 0; i < AC_RP_SCOPES; i++) {
        correction = ac_route_profile_scopes[i].calls * ac_route_profile_empty_ns;
        fprintf(file, "%s\"%s\":{\"calls\":%llu,\"raw_ns\":%llu,\"empty_corrected_ns\":%llu}",
                i ? "," : "", ac_route_profile_names[i],
                (unsigned long long) ac_route_profile_scopes[i].calls,
                (unsigned long long) ac_route_profile_scopes[i].raw_ns,
                (unsigned long long) (ac_route_profile_scopes[i].raw_ns > correction
                    ? ac_route_profile_scopes[i].raw_ns - correction : 0));
    }
    fprintf(file, "},\"note\":\"Diagnostic instrumentation changes CPU cost. "
            "Calibration subtraction is approximate; compare a normal build. "
            "collect_sort includes pool allocation; eligibility_expr may call custom Lua getters; "
            "options and accept_context include Lua ABI work. Filter callbacks are excluded.\"}\n");
    return fclose(file) == 0 ? 0 : -1;
}

#define AC_ROUTE_BEGIN(name) uint64_t name = ac_route_profile_active ? ac_route_profile_clock() : 0
#define AC_ROUTE_END(name, scope) ac_route_profile_record(scope, name)
#else
#define AC_ROUTE_BEGIN(name) do { } while (0)
#define AC_ROUTE_END(name, scope) do { } while (0)
#endif

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
        if (main->native_filters) {
            ac_push(L, route->value_ref); lua_getfield(L, -1, "script");
            if (!lua_isnil(L, -1)) luaL_error(L, "native filter mode cannot dispatch Lua route scripts");
            lua_pop(L, 2);
        }
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

/* Same compressed tree, predicates and precedence as the Lua-ABI path. Native
 * mode forbids expressions/filter callbacks at compile time; no options table,
 * captures table or registry publication is needed by a C-only plugin. */
ngx_int_t
ac_match_native(ac_request_t *ctx)
{
    ac_radix_node_t *node = ctx->main->route_index, *child;
    ac_candidate_t *entries, candidate, best;
    ngx_flag_t found = 0;
    ngx_uint_t offset = 0, i;
    ngx_str_t key = ctx->routing_uri;
    int *captures;
    for (;;) {
        entries = node->entries.elts;
        for (i = 0; i < node->entries.nelts; i++) {
            candidate = entries[i];
            candidate.host_rank = ac_host_rank(ctx->main, candidate.route, ctx->request, &candidate.host);
            if (candidate.host_rank < 0) continue;
            if (!ctx->main->host_router) candidate.host_rank = 0;
            if (candidate.route->methods && !(candidate.route->methods & ctx->request->method)) continue;
            if (candidate.route->cidrs.nelts && ngx_cidr_match(ctx->request->connection->sockaddr,
                    &candidate.route->cidrs) != NGX_OK) continue;
            captures = NULL;
            if (!ac_path_matches(candidate.path, ctx, &captures)) continue;
            if (!found || ac_candidate_order(&candidate, &best) < 0) { best = candidate; found = 1; }
        }
        if (offset == key.len) break;
        for (child = node->child; child; child = child->sibling) {
            if (child->edge.len <= key.len - offset && ngx_memcmp(child->edge.data,
                    key.data + offset, child->edge.len) == 0) break;
        }
        if (!child) break;
        offset += child->edge.len; node = child;
    }
    if (!found) return NGX_HTTP_NOT_FOUND;
    ctx->route = best.route; ctx->path = best.path; ctx->metadata = best.route->metadata;
    return best.route->resolve_status ? (ngx_int_t) best.route->resolve_status : NGX_OK;
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
        if (ctx->main->route_candidates == 1 && ctx->candidates.elts == NULL
            && ngx_array_init(&ctx->candidates, r->pool, 1,
                              sizeof(ac_candidate_t)) != NGX_OK) {
            luaL_error(L, "request route candidate allocation failed");
        }
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
    ac_candidate_t *candidates;
    ngx_uint_t offset = 0, i;
    ngx_flag_t need_opts = ctx->main->lazy_route_opts != 1;
    int opts, table;
    ctx->route = NULL;
    ctx->candidate_cursor = 0;
    ctx->routing = 1;
    AC_ROUTE_BEGIN(collect_started);
    if (ctx->main->route_candidates == 1) {
        /* No allocation for misses; the common single candidate needs one
         * slot. ngx_array_push retains its normal growth path after that. */
        ngx_memzero(&ctx->candidates, sizeof(ctx->candidates));
    } else if (ngx_array_init(&ctx->candidates, ctx->request->pool, 4,
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
    if (ctx->main->route_candidates != 1 || ctx->candidates.nelts > 1)
        ngx_qsort(ctx->candidates.elts, ctx->candidates.nelts, sizeof(ac_candidate_t), ac_candidate_order);
    AC_ROUTE_END(collect_started, AC_RP_COLLECT_SORT);
    AC_ROUTE_BEGIN(options_started);
    if (!need_opts) {
        candidates = ctx->candidates.elts;
        for (i = 0; i < ctx->candidates.nelts; i++) {
            if (candidates[i].route->filter_ref != LUA_NOREF) {
                need_opts = 1;
                break;
            }
        }
    }
    ac_push(L, ctx->table_ref);
    table = lua_gettop(L);
    if (!need_opts) {
        /* This read is observable even without a filter: custom getters may
         * have side effects and default lookup publishes a cached value. Keep
         * its original point before route-expression evaluation. */
        lua_getfield(L, table, "var");
        lua_getfield(L, -1, "remote_addr");
        lua_pop(L, 3);
        AC_ROUTE_END(options_started, AC_RP_OPTIONS);
        return;
    }
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
    AC_ROUTE_END(options_started, AC_RP_OPTIONS);
}

static void
ac_accept_route(lua_State *L, ac_request_t *ctx, ac_candidate_t *candidate, int *captures)
{
    ac_route_t *route = candidate->route;
    ac_path_t *path = candidate->path;
    ngx_str_t *names = path->names.elts;
    ngx_uint_t k;
    int table, matched, top = lua_gettop(L);
    AC_ROUTE_BEGIN(accept_started);
    ctx->path = path;
    ctx->route = route;
    ctx->metadata = route->metadata;
    ctx->routing = 0;
    ctx->early_status = route->resolve_status;
    ac_push(L, ctx->table_ref);
    table = lua_gettop(L);
    /* A previous accepted route may still have plugin-held aliases. Do not
     * return that record to the free pool during the same live request. */
    if (ctx->matched_ref >= 0) {
        luaL_unref(L, LUA_REGISTRYINDEX, ctx->matched_ref);
        ctx->matched_ref = LUA_NOREF;
    }
    if (ctx->match_opts_ref == LUA_NOREF) {
        /* Requires lazy_route_opts and no observable filter options. Stable
         * Lua already pools matched_route_record after log; active requests
         * still have distinct matched tables. Filter-owned records stay put. */
        if (ctx->main->matched_pool == 1) {
            ctx->matched_ref = ac_table_pool_fetch(L, ctx->main,
                ctx->main->matched_pool_state, 3 + path->names.nelts);
            ac_push(L, ctx->matched_ref);
        } else {
            lua_createtable(L, 0, 3 + path->names.nelts);
        }
    } else {
        ac_push(L, ctx->match_opts_ref);
        lua_getfield(L, -1, "matched");
    }
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
    lua_settop(L, top);
    ac_route_context(L, ctx, 0);
    ac_http_match_finish(L, ctx);
    AC_ROUTE_END(accept_started, AC_RP_ACCEPT_CONTEXT);
}

void
ac_match_complete(lua_State *L, ac_request_t *ctx, ngx_flag_t accepted)
{
    int *captures = NULL;
    ngx_flag_t matched;
    ac_candidate_t *candidates = ctx->candidates.elts;
    ac_candidate_t *candidate = &candidates[ctx->candidate_cursor - 1];
    AC_ROUTE_BEGIN(path_started);
    matched = accepted && ac_path_matches(candidate->path, ctx, &captures);
    AC_ROUTE_END(path_started, AC_RP_PATH_MATCH);
    if (matched) {
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
        AC_ROUTE_BEGIN(eligibility_started);
        candidate = &candidates[ctx->candidate_cursor++];
        route = candidate->route;
        if (route->methods && !(route->methods & ctx->request->method)) {
            AC_ROUTE_END(eligibility_started, AC_RP_ELIGIBILITY_EXPR);
            continue;
        }
        if (route->cidrs.nelts && ngx_cidr_match(ctx->request->connection->sockaddr, &route->cidrs) != NGX_OK) {
            AC_ROUTE_END(eligibility_started, AC_RP_ELIGIBILITY_EXPR);
            continue;
        }
        if (route->vars_ref != LUA_REFNIL && route->vars_ref != LUA_NOREF) {
            ngx_flag_t matches;
            matches = ac_expr_eval(L, route->expression, ctx);
            if (!matches) {
                AC_ROUTE_END(eligibility_started, AC_RP_ELIGIBILITY_EXPR);
                continue;
            }
        }
        AC_ROUTE_END(eligibility_started, AC_RP_ELIGIBILITY_EXPR);
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
