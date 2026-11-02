/* SPDX-License-Identifier: Apache-2.0 */
#include "engine.h"
#include <math.h>

/* Exported APIs behind ngx.balancer in the pinned runtime. Native decisions
 * and transport arguments already exist in C, so no Lua option tables or FFI
 * conversions are needed to pass them back to the runtime. */
extern int ngx_http_lua_ffi_balancer_set_current_peer(ngx_http_request_t *r,
    const u_char *address, size_t length, int port, unsigned int pool_crc,
    unsigned int pool_size, char **error);
extern int ngx_http_lua_ffi_balancer_enable_keepalive(ngx_http_request_t *r,
    unsigned long timeout, unsigned int max_requests, char **error);
extern int ngx_http_lua_ffi_balancer_set_more_tries(ngx_http_request_t *r, int count, char **error);
extern int ngx_http_lua_ffi_balancer_get_last_failure(ngx_http_request_t *r, int *status, char **error);
extern int ngx_http_lua_ffi_balancer_set_timeouts(ngx_http_request_t *r,
    long connect_timeout, long send_timeout, long read_timeout, char **error);
extern int ngx_http_lua_ffi_balancer_recreate_request(ngx_http_request_t *r, char **error);

typedef struct ac_node_s {
    ngx_addr_t address;
    ngx_str_t host;
    in_port_t port;
    ngx_int_t weight;
    ngx_int_t priority;
    ngx_int_t current;
    ngx_uint_t active;
    ngx_uint_t fails;
    ngx_msec_t checked;
    double ewma;
    struct ac_node_s *origin;
} ac_node_t;

struct ac_upstream_s {
    ngx_array_t nodes;
    ngx_str_t type;
    ngx_str_t scheme;
    ngx_str_t pass_host;
    ngx_str_t upstream_host;
    ngx_msec_t connect_timeout;
    ngx_msec_t send_timeout;
    ngx_msec_t read_timeout;
    ngx_uint_t retries;
    ngx_msec_t retry_timeout;
    ngx_msec_t fail_timeout;
    ngx_uint_t max_fails;
    int conf_ref;
    int hash_cache_ref;
    ngx_str_t hash_on;
    ngx_str_t key;
    ngx_uint_t keepalive_size;
    ngx_uint_t keepalive_requests;
    lua_Number keepalive_idle;
    unsigned domain:1;
};

typedef struct {
    ac_request_t *ctx;
    ac_upstream_t *upstream;
    ac_node_t *selected;
    uintptr_t *tried;
    ngx_msec_t started;
    int ring_ref;
    lua_Number ring_index;
    ngx_int_t ring_priority;
} ac_peer_t;

static struct { const char *name; const char *value; } ac_native_vars[] = {
    {"upstream_scheme", "http"}, {"upstream_host", ""}, {"upstream_uri", ""},
    {"upstream_unresolved_host", ""}, {"upstream_upgrade", ""}, {"upstream_connection", ""},
    {"request_line", ""}, {"llm_content_risk_level", ""}, {"apisix_request_id", ""},
    {"request_type", "traditional_http"}, {"apisix_upstream_response_time", ""},
    {"llm_time_to_first_token", "0"}, {"request_llm_model", ""}, {"llm_model", ""},
    {"llm_prompt_tokens", "0"}, {"llm_completion_tokens", "0"}, {"llm_total_tokens", "0"},
    {"llm_stream", "false"}, {"llm_has_tool_calls", "false"}, {"llm_tool_count", "0"},
    {"llm_end_user_id", ""}, {"llm_cache_read_input_tokens", "0"},
    {"llm_cache_creation_input_tokens", "0"}, {"llm_reasoning_tokens", "0"},
    {"upstream_mirror_host", ""}, {"upstream_mirror_uri", ""}, {"upstream_mirror_grpc_path", ""},
    {"upstream_cache_zone", ""}, {"upstream_cache_zone_info", ""}, {"upstream_no_cache", ""},
    {"upstream_cache_key", ""}, {"upstream_cache_bypass", ""}, {"rate_limiting_info", ""},
    {"original_x_forwarded_for", ""}, {"original_x_forwarded_proto", ""}, {"original_x_forwarded_host", ""},
    {"original_x_forwarded_port", ""}, {"original_forwarded", ""}
};

static ngx_int_t ac_peer_get(ngx_peer_connection_t *pc, void *data, lua_State *L);
static void ac_peer_free(ngx_peer_connection_t *pc, void *data, ngx_uint_t state);

static ngx_str_t
ac_string_default(lua_State *L, ngx_pool_t *pool, int table, const char *field, const char *fallback)
{
    ngx_str_t result;
    lua_getfield(L, table, field);
    if (lua_isnil(L, -1)) { lua_pop(L, 1); lua_pushstring(L, fallback); }
    result = ac_string(L, pool, -1); lua_pop(L, 1); return result;
}

static ngx_msec_t
ac_timeout(lua_State *L, int table, const char *field, ngx_msec_t fallback)
{
    ngx_msec_t result;
    lua_getfield(L, table, field);
    result = lua_isnil(L, -1) ? fallback : (ngx_msec_t) (luaL_checknumber(L, -1) * 1000);
    lua_pop(L, 1); return result;
}

static void
ac_add_node(lua_State *L, ac_main_conf_t *main, ac_upstream_t *upstream,
    ngx_str_t host, in_port_t port, ngx_int_t weight, ngx_int_t priority)
{
    ac_node_t *node;
    ngx_str_t endpoint;
    u_char *last;
    ngx_url_t parsed;
    if (weight < 0 || weight > 65535) luaL_error(L, "invalid upstream node weight");
    node = ngx_array_push(&upstream->nodes);
    if (!node) luaL_error(L, "native upstream node allocation failed");
    ngx_memzero(node, sizeof(*node));
    node->host = host; node->port = port; node->weight = weight; node->priority = priority;
    endpoint.data = ngx_pnalloc(main->pool, host.len + 10);
    if (!endpoint.data) luaL_error(L, "native endpoint allocation failed");
    if (ngx_strlchr(host.data, host.data + host.len, ':') && host.data[0] != '[') {
        last = ngx_snprintf(endpoint.data, host.len + 10, "[%V]:%ui", &host, (ngx_uint_t) port);
    } else last = ngx_snprintf(endpoint.data, host.len + 10, "%V:%ui", &host, (ngx_uint_t) port);
    endpoint.len = last - endpoint.data;
    ngx_memzero(&parsed, sizeof(parsed)); parsed.url = endpoint; parsed.no_resolve = 1; parsed.default_port = port;
    if (ngx_parse_url(main->pool, &parsed) != NGX_OK) luaL_error(L, "invalid upstream node address");
    if (!parsed.naddrs) upstream->domain = 1;
    else node->address = parsed.addrs[0];
    node->address.name = endpoint;
}

static ac_upstream_t *
ac_compile_upstream(lua_State *L, ac_main_conf_t *main, int conf, int route_timeout)
{
    ac_upstream_t *upstream;
    ngx_uint_t i;
    int nodes, value, timeout;
    ngx_int_t weight, priority;
    ngx_str_t host;
    in_port_t port;
    ngx_url_t url;
    conf = ac_abs(L, conf); route_timeout = ac_abs(L, route_timeout);
    upstream = ngx_pcalloc(main->pool, sizeof(*upstream));
    if (!upstream || ngx_array_init(&upstream->nodes, main->pool, 4, sizeof(ac_node_t)) != NGX_OK) {
        luaL_error(L, "native upstream allocation failed");
    }
    upstream->conf_ref = ac_ref(L, conf);
    upstream->hash_cache_ref = LUA_NOREF;
    upstream->type = ac_string_default(L, main->pool, conf, "type", "roundrobin");
    upstream->scheme = ac_string_default(L, main->pool, conf, "scheme", "http");
    upstream->pass_host = ac_string_default(L, main->pool, conf, "pass_host", "pass");
    upstream->upstream_host = ac_string_default(L, main->pool, conf, "upstream_host", "");
    upstream->hash_on = ac_string_default(L, main->pool, conf, "hash_on", "vars");
    upstream->key = ac_string_default(L, main->pool, conf, "key", "remote_addr");
    if (ngx_strcmp(upstream->type.data, "chash") == 0) {
        ac_require(L, "resty.lrucache"); lua_getfield(L, -1, "new"); lua_pushinteger(L, 32); lua_call(L, 1, 1);
        if (!lua_toboolean(L, -1)) luaL_error(L, "chash cache allocation failed");
        upstream->hash_cache_ref = ac_ref(L, -1); lua_pop(L, 2);
    } else if (ngx_strcmp(upstream->type.data, "roundrobin") != 0
               && ngx_strcmp(upstream->type.data, "least_conn") != 0
               && ngx_strcmp(upstream->type.data, "ewma") != 0) {
        luaL_error(L, "unknown upstream balancer type");
    }
    upstream->connect_timeout = upstream->send_timeout = upstream->read_timeout = 6000;
    upstream->keepalive_size = 320; upstream->keepalive_requests = 1000; upstream->keepalive_idle = 60;
    int settings_top = lua_gettop(L), pool_options;
    ac_push(L, main->local_ref); lua_getfield(L, -1, "nginx_config");
    if (lua_istable(L, -1)) lua_getfield(L, -1, "http");
    if (lua_istable(L, -1)) lua_getfield(L, -1, "upstream");
    if (lua_istable(L, -1)) {
        pool_options = lua_gettop(L);
        lua_getfield(L, pool_options, "keepalive");
        if (lua_isnumber(L, -1)) upstream->keepalive_size = (ngx_uint_t) lua_tointeger(L, -1); lua_pop(L, 1);
        lua_getfield(L, pool_options, "keepalive_requests");
        if (lua_isnumber(L, -1)) upstream->keepalive_requests = (ngx_uint_t) lua_tointeger(L, -1); lua_pop(L, 1);
        lua_getfield(L, pool_options, "keepalive_timeout");
        if (lua_toboolean(L, -1)) {
            ac_require(L, "apisix.core.config_util"); lua_getfield(L, -1, "parse_time_unit"); lua_pushvalue(L, -3);
            lua_call(L, 1, 1); if (lua_isnumber(L, -1)) upstream->keepalive_idle = lua_tonumber(L, -1); lua_pop(L, 2);
        }
    }
    lua_settop(L, settings_top);
    lua_getfield(L, conf, "keepalive_pool");
    if (lua_istable(L, -1)) {
        pool_options = lua_gettop(L);
        lua_getfield(L, pool_options, "size");
        if (lua_isnumber(L, -1)) upstream->keepalive_size = (ngx_uint_t) lua_tointeger(L, -1); lua_pop(L, 1);
        lua_getfield(L, pool_options, "requests");
        if (lua_isnumber(L, -1)) upstream->keepalive_requests = (ngx_uint_t) lua_tointeger(L, -1); lua_pop(L, 1);
        lua_getfield(L, pool_options, "idle_timeout");
        if (lua_isnumber(L, -1)) upstream->keepalive_idle = lua_tonumber(L, -1); lua_pop(L, 1);
    }
    lua_pop(L, 1);
    upstream->max_fails = 0; upstream->fail_timeout = 10000;
    lua_getfield(L, conf, "retries"); upstream->retries = lua_isnil(L, -1) ? NGX_CONF_UNSET_UINT : (ngx_uint_t) luaL_checkinteger(L, -1); lua_pop(L, 1);
    upstream->retry_timeout = ac_timeout(L, conf, "retry_timeout", 0);
    lua_getfield(L, conf, "timeout"); timeout = lua_gettop(L);
    if (lua_istable(L, route_timeout)) { lua_pushvalue(L, route_timeout); lua_replace(L, timeout); }
    if (lua_istable(L, timeout)) {
        upstream->connect_timeout = ac_timeout(L, timeout, "connect", 6000);
        upstream->send_timeout = ac_timeout(L, timeout, "send", 6000);
        upstream->read_timeout = ac_timeout(L, timeout, "read", 6000);
    }
    lua_pop(L, 1);
    lua_getfield(L, conf, "nodes"); nodes = lua_gettop(L);
    if (lua_istable(L, nodes)) {
        if (lua_objlen(L, nodes)) {
            for (i = 1; i <= lua_objlen(L, nodes); i++) {
                lua_rawgeti(L, nodes, (int) i); value = lua_gettop(L);
                lua_getfield(L, value, "host"); host = ac_string(L, main->pool, -1); lua_pop(L, 1);
                lua_getfield(L, value, "port"); port = lua_isnil(L, -1) ? (ngx_strcmp(upstream->scheme.data, "https") == 0 ? 443 : 80) : (in_port_t) luaL_checkinteger(L, -1); lua_pop(L, 1);
                lua_getfield(L, value, "weight"); weight = lua_isnil(L, -1) ? 1 : luaL_checkinteger(L, -1); lua_pop(L, 1);
                lua_getfield(L, value, "priority"); priority = lua_isnil(L, -1) ? 0 : luaL_checkinteger(L, -1); lua_pop(L, 1);
                ac_add_node(L, main, upstream, host, port, weight, priority); lua_pop(L, 1);
            }
        } else {
            lua_pushnil(L);
            while (lua_next(L, nodes)) {
                ngx_memzero(&url, sizeof(url));
                url.url = ac_string(L, main->pool, -2); url.no_resolve = 1;
                url.default_port = ngx_strcmp(upstream->scheme.data, "https") == 0 ? 443 : 80;
                if (ngx_parse_url(main->pool, &url) != NGX_OK) luaL_error(L, "invalid upstream node key");
                weight = luaL_checkinteger(L, -1);
                ac_add_node(L, main, upstream, url.host, url.port, weight, 0); lua_pop(L, 1);
            }
        }
    }
    lua_pop(L, 1); return upstream;
}

void
ac_upstream_compile(lua_State *L, ac_main_conf_t *main, ac_route_t *route)
{
    int value, timeout;
    ac_push(L, route->value_ref); value = lua_gettop(L);
    lua_getfield(L, value, "timeout"); timeout = lua_gettop(L);
    lua_getfield(L, value, "upstream_id");
    if (!lua_isnil(L, -1)) {
        ac_resource_get(L, main, "upstreams", -1);
        if (lua_istable(L, -1)) { lua_getfield(L, -1, "value"); route->upstream = ac_compile_upstream(L, main, -1, timeout); lua_pop(L, 1); }
        else route->resolve_status = route->resolve_status ? route->resolve_status : 503;
        lua_pop(L, 1);
    } else {
        lua_getfield(L, value, "upstream");
        if (lua_istable(L, -1)) route->upstream = ac_compile_upstream(L, main, -1, timeout);
        lua_pop(L, 1);
    }
    lua_pop(L, 3);
}

int
ac_upstream_prepare(lua_State *L, ac_request_t *ctx)
{
    ac_upstream_t *upstream = ctx->upstream ? ctx->upstream : ctx->route->upstream;
    ngx_http_request_t *r = ctx->request;
    if (!upstream || !upstream->nodes.nelts) { lua_pushinteger(L, 503); return 1; }
    ctx->upstream = upstream;
    ac_push(L, ctx->table_ref); lua_getfield(L, -1, "upstream_scheme");
    if (lua_isstring(L, -1)) ctx->upstream_scheme = ac_string(L, r->pool, -1);
    lua_pop(L, 2);
    if (!ctx->upstream_scheme.data) ctx->upstream_scheme = upstream->scheme;
    if (!ctx->upstream_host.data) {
        if (ngx_strcmp(upstream->pass_host.data, "rewrite") == 0) ctx->upstream_host = upstream->upstream_host;
        else if (r->headers_in.host) ctx->upstream_host = r->headers_in.host->value;
        else ctx->upstream_host = r->headers_in.server;
    }
    if (!ctx->upstream_uri.data) {
        ctx->upstream_uri = r->valid_unparsed_uri ? r->unparsed_uri : ac_http_proxy_uri(L, ctx, r->uri);
    }
    ac_push(L, ctx->table_ref); ac_push(L, upstream->conf_ref); lua_setfield(L, -2, "upstream_conf"); lua_pop(L, 1);
    if (!ctx->native_peer) {
        ac_peer_t *peer = ngx_pcalloc(r->pool, sizeof(*peer));
        if (!peer) return luaL_error(L, "native peer state allocation failed");
        peer->ctx = ctx; peer->upstream = upstream; peer->started = ngx_current_msec;
        peer->ring_ref = LUA_NOREF;
        peer->tried = ngx_pcalloc(r->pool, ((upstream->nodes.nelts + 8 * sizeof(uintptr_t) - 1)
                                / (8 * sizeof(uintptr_t))) * sizeof(uintptr_t));
        if (!peer->tried) return luaL_error(L, "native peer retry allocation failed");
        ctx->native_peer = peer;
        ngx_peer_connection_t pc; ngx_memzero(&pc, sizeof(pc));
        if (ac_peer_get(&pc, peer, L) != NGX_OK) { lua_pushinteger(L, 502); return 1; }
    }
    ac_var_assign(L, ctx, "upstream_uri", ctx->upstream_uri, 1);
    ac_var_assign(L, ctx, "upstream_scheme", ctx->upstream_scheme, 1);
    ac_var_assign(L, ctx, "upstream_host", ctx->upstream_host, 1);
    return 0;
}

int
ac_upstream_next(lua_State *L, ac_request_t *ctx)
{
    ac_upstream_t *upstream;
    ac_node_t *nodes;
    ngx_uint_t i;
    if (ctx->early_status || ctx->dns_ready || !ctx->route) return 0;
    upstream = ctx->upstream ? ctx->upstream : ctx->route->upstream;
    if (!upstream || !upstream->domain) { ctx->dns_ready = 1; return 0; }
    if (ctx->dns_cursor == 0) {
        ac_upstream_t *copy = ngx_palloc(ctx->request->pool, sizeof(*copy));
        if (!copy) return luaL_error(L, "DNS upstream snapshot allocation failed");
        *copy = *upstream;
        if (ngx_array_init(&copy->nodes, ctx->request->pool, upstream->nodes.nelts, sizeof(ac_node_t)) != NGX_OK) {
            return luaL_error(L, "DNS upstream node allocation failed");
        }
        nodes = ngx_array_push_n(&copy->nodes, upstream->nodes.nelts);
        if (!nodes) return luaL_error(L, "DNS upstream node copy failed");
        ngx_memcpy(nodes, upstream->nodes.elts, upstream->nodes.nelts * sizeof(*nodes));
        for (i = 0; i < upstream->nodes.nelts; i++) {
            nodes[i].origin = &((ac_node_t *) upstream->nodes.elts)[i];
        }
        ctx->upstream = copy;
        upstream = copy;
    }
    nodes = upstream->nodes.elts;
    for (i = ctx->dns_cursor; i < upstream->nodes.nelts; i++) {
        ctx->dns_cursor = i + 1;
        if (nodes[i].address.sockaddr || !nodes[i].weight) continue;
        ac_require(L, "apisix.core.resolver"); lua_getfield(L, -1, "parse_domain"); lua_remove(L, -2);
        lua_pushlstring(L, (char *) nodes[i].host.data, nodes[i].host.len);
        ctx->callback_pending = 1; ctx->dns_pending = 1; return 2;
    }
    ctx->dns_ready = 1; return 0;
}

void
ac_upstream_resolved(lua_State *L, ac_request_t *ctx)
{
    ac_node_t *nodes = ctx->upstream->nodes.elts;
    ac_node_t *node = &nodes[ctx->dns_cursor - 1];
    ngx_str_t address;
    if (!lua_isstring(L, 2)) {
        node->weight = 0;
        ngx_log_error(NGX_LOG_ERR, ctx->request->connection->log, 0, "native DNS failed resolving %V", &node->host);
        return;
    }
    address.data = (u_char *) lua_tolstring(L, 2, &address.len);
    if (ngx_parse_addr(ctx->request->pool, &node->address, address.data, address.len) != NGX_OK) {
        node->weight = 0; return;
    }
    ngx_inet_set_port(node->address.sockaddr, node->port);
    node->address.name.data = ngx_pnalloc(ctx->request->pool, NGX_SOCKADDR_STRLEN);
    if (!node->address.name.data) { node->weight = 0; return; }
    node->address.name.len = ngx_sock_ntop(node->address.sockaddr, node->address.socklen,
                                         node->address.name.data, NGX_SOCKADDR_STRLEN, 1);
}

static ac_node_t *
ac_node_state(ac_node_t *node)
{
    return node->origin ? node->origin : node;
}

static ngx_flag_t
ac_node_eligible(ac_peer_t *peer, ngx_uint_t i)
{
    ac_node_t *node = &((ac_node_t *) peer->upstream->nodes.elts)[i];
    ac_node_t *state = ac_node_state(node);
    uintptr_t bit = (uintptr_t) 1 << (i % (8 * sizeof(uintptr_t)));
    if (peer->tried[i / (8 * sizeof(uintptr_t))] & bit) return 0;
    if (!node->weight || !node->address.sockaddr) return 0;
    if (peer->upstream->max_fails && peer->upstream->nodes.nelts > 1
        && state->fails >= peer->upstream->max_fails
        && ngx_current_msec - state->checked <= peer->upstream->fail_timeout) return 0;
    return 1;
}

static void
ac_chash_key(lua_State *L, ac_peer_t *peer)
{
    ac_upstream_t *upstream = peer->upstream;
    ngx_str_t key = upstream->key;
    if (ngx_strcmp(upstream->hash_on.data, "consumer") == 0) {
        ac_push(L, peer->ctx->table_ref); lua_getfield(L, -1, "consumer_name"); lua_remove(L, -2);
    } else if (ngx_strcmp(upstream->hash_on.data, "vars_combinations") == 0) {
        ac_require(L, "apisix.core.utils"); lua_getfield(L, -1, "resolve_var"); lua_remove(L, -2);
        lua_pushlstring(L, (char *) key.data, key.len);
        ac_push(L, peer->ctx->table_ref); lua_getfield(L, -1, "var"); lua_remove(L, -2); lua_call(L, 2, 3);
        if (lua_tonumber(L, -1) == 0) { lua_pop(L, 3); lua_pushnil(L); }
        else lua_pop(L, 2);
    } else {
        const char *prefix = ngx_strcmp(upstream->hash_on.data, "header") == 0 ? "http_"
            : ngx_strcmp(upstream->hash_on.data, "cookie") == 0 ? "cookie_" : "";
        size_t length = ngx_strlen(prefix);
        if (length) {
            key.data = ngx_pnalloc(peer->ctx->request->pool, length + key.len);
            if (!key.data) luaL_error(L, "chash variable allocation failed");
            ngx_memcpy(key.data, prefix, length); ngx_memcpy(key.data + length, upstream->key.data, key.len); key.len += length;
        }
        ac_variable(L, peer->ctx, key);
    }
    if (!lua_toboolean(L, -1)) {
        lua_pop(L, 1); ngx_str_t fallback = ngx_string("remote_addr"); ac_variable(L, peer->ctx, fallback);
    }
}

/* resty.chash supplies the existing C hash ring primitive. The APISIX key,
 * weight normalization, priority and retry policy above it are native here. */
static ac_node_t *
ac_chash_pick(lua_State *L, ac_peer_t *peer, ngx_int_t priority, ngx_uint_t *index)
{
    ac_upstream_t *upstream = peer->upstream;
    ac_node_t *nodes = upstream->nodes.elts, *selected = NULL;
    ngx_uint_t i, divisor = 0, left, right, swap, attempt, points;
    int top = lua_gettop(L), weights, cache, ring;
    size_t capacity, length;
    ngx_str_t signature, id;
    u_char *write;
    ngx_flag_t retry = peer->ring_ref != LUA_NOREF && peer->ring_priority == priority;
    for (i = 0; i < upstream->nodes.nelts; i++) {
        if (nodes[i].priority != priority || !nodes[i].weight || !nodes[i].address.sockaddr) continue;
        left = divisor; right = nodes[i].weight;
        while (right) { swap = left % right; left = right; right = swap; }
        divisor = left;
    }
    if (!divisor) return NULL;
    if (upstream->nodes.nelts > (NGX_MAX_SIZE_T_VALUE - 32) / (NGX_SOCKADDR_STRLEN + 32)) {
        luaL_error(L, "chash node signature overflow"); return NULL;
    }
    capacity = upstream->nodes.nelts * (NGX_SOCKADDR_STRLEN + 32) + 32;
    signature.data = ngx_pnalloc(peer->ctx->request->pool, capacity);
    if (!signature.data) { luaL_error(L, "chash signature allocation failed"); return NULL; }
    write = ngx_snprintf(signature.data, capacity, "%i;", priority);
    lua_newtable(L); weights = lua_gettop(L);
    for (i = 0; i < upstream->nodes.nelts; i++) {
        if (nodes[i].priority != priority || !nodes[i].weight || !nodes[i].address.sockaddr) continue;
        id = nodes[i].address.name;
        write = ngx_snprintf(write, signature.data + capacity - write, "%V=%ui;", &id,
                              (ngx_uint_t) nodes[i].weight / divisor);
        u_char *binary = ngx_pnalloc(peer->ctx->request->pool, id.len);
        if (!binary) { luaL_error(L, "chash server id allocation failed"); return NULL; }
        for (length = 0; length < id.len; length++) binary[length] = id.data[length] == ':' ? 0 : id.data[length];
        lua_pushlstring(L, (char *) binary, id.len); lua_pushinteger(L, nodes[i].weight / divisor); lua_rawset(L, weights);
    }
    signature.len = write - signature.data;
    ac_push(L, upstream->hash_cache_ref); cache = lua_gettop(L);
    lua_getfield(L, cache, "get"); lua_pushvalue(L, cache);
    lua_pushlstring(L, (char *) signature.data, signature.len); lua_call(L, 2, 1);
    if (!lua_toboolean(L, -1)) {
        lua_pop(L, 1); ac_require(L, "resty.chash"); lua_getfield(L, -1, "new");
        lua_pushvalue(L, -2); lua_pushvalue(L, weights); lua_call(L, 2, 1); lua_remove(L, -2);
        lua_getfield(L, cache, "set"); lua_pushvalue(L, cache);
        lua_pushlstring(L, (char *) signature.data, signature.len); lua_pushvalue(L, -4);
        lua_pushinteger(L, 300); lua_call(L, 4, 0);
    }
    ring = lua_gettop(L);
    if (peer->ring_ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, peer->ring_ref);
    peer->ring_ref = ac_ref(L, ring); peer->ring_priority = priority;
    lua_getfield(L, ring, "npoints"); points = (ngx_uint_t) lua_tonumber(L, -1); lua_pop(L, 1);
    for (attempt = 0; attempt < points; attempt++) {
        lua_getfield(L, ring, retry || attempt ? "next" : "find"); lua_pushvalue(L, ring);
        if (retry || attempt) lua_pushnumber(L, peer->ring_index); else ac_chash_key(L, peer);
        lua_call(L, 2, 2);
        id.data = (u_char *) lua_tolstring(L, -2, &id.len); peer->ring_index = lua_tonumber(L, -1);
        for (i = 0; id.data && i < upstream->nodes.nelts; i++) {
            if (nodes[i].priority != priority || !ac_node_eligible(peer, i) || id.len != nodes[i].address.name.len) continue;
            for (length = 0; length < id.len; length++) {
                u_char byte = nodes[i].address.name.data[length];
                if (id.data[length] != (byte == ':' ? 0 : byte)) break;
            }
            if (length == id.len) { selected = &nodes[i]; *index = i; break; }
        }
        lua_pop(L, 2);
        if (selected) break;
    }
    lua_settop(L, top); return selected;
}

static ngx_int_t
ac_peer_get(ngx_peer_connection_t *pc, void *data, lua_State *L)
{
    ac_peer_t *peer = data;
    ac_upstream_t *upstream = peer->upstream;
    ac_node_t *nodes = upstream->nodes.elts, *node, *state, *selected = NULL;
    ngx_uint_t i, best_index = 0;
    ngx_int_t total = 0, best_priority = -NGX_MAX_INT_T_VALUE - 1;
    double score, best_score = HUGE_VAL;
    for (i = 0; i < upstream->nodes.nelts; i++) {
        if (ac_node_eligible(peer, i) && nodes[i].priority > best_priority) best_priority = nodes[i].priority;
    }
    if (ngx_strcmp(upstream->type.data, "chash") == 0) {
        selected = ac_chash_pick(L, peer, best_priority, &best_index);
    } else for (i = 0; i < upstream->nodes.nelts; i++) {
        node = &nodes[i];
        if (node->priority != best_priority || !ac_node_eligible(peer, i)) continue;
        state = ac_node_state(node);
        if (ngx_strcmp(upstream->type.data, "least_conn") == 0) score = (1.0 + state->active) / node->weight;
        else if (ngx_strcmp(upstream->type.data, "ewma") == 0) score = (state->active + 1) * (state->ewma > 0 ? state->ewma : 1) / node->weight;
        else { state->current += node->weight; total += node->weight; score = -(double) state->current; }
        if (!selected || score < best_score) { selected = node; best_score = score; best_index = i; }
    }
    if (!selected) return NGX_BUSY;
    state = ac_node_state(selected);
    if (ngx_strcmp(upstream->type.data, "roundrobin") == 0) state->current -= total;
    peer->tried[best_index / (8 * sizeof(uintptr_t))] |= (uintptr_t) 1 << (best_index % (8 * sizeof(uintptr_t)));
    state->active++; peer->selected = selected;
    pc->sockaddr = selected->address.sockaddr; pc->socklen = selected->address.socklen;
    pc->name = &selected->address.name; pc->cached = 0; pc->connection = NULL;
    if (ngx_strcmp(upstream->pass_host.data, "node") == 0) {
        ngx_str_t host = selected->host;
        ngx_uint_t standard = ngx_strcmp(peer->ctx->upstream_scheme.data, "https") == 0 ? 443 : 80;
        if (selected->port != standard) {
            host.data = ngx_pnalloc(peer->ctx->request->pool, selected->host.len + 10);
            if (!host.data) return NGX_ERROR;
            host.len = ngx_snprintf(host.data, selected->host.len + 10, "%V:%ui", &selected->host,
                                    (ngx_uint_t) selected->port) - host.data;
        }
        peer->ctx->upstream_host = host;
        ac_var_assign(L, peer->ctx, "upstream_host", host, 1);
    }
    ac_push(L, peer->ctx->table_ref);
    u_char ip[NGX_SOCKADDR_STRLEN];
    size_t length = ngx_sock_ntop(selected->address.sockaddr, selected->address.socklen, ip, sizeof(ip), 0);
    lua_pushlstring(L, (char *) ip, length); lua_setfield(L, -2, "balancer_ip");
    lua_pushinteger(L, selected->port); lua_setfield(L, -2, "balancer_port");
    ac_set_string(L, -1, "upstream_unresolved_host", selected->host);
    lua_pop(L, 1);
    ac_var_assign(L, peer->ctx, "upstream_unresolved_host", selected->host, 1);
    return NGX_OK;
}

static void
ac_peer_free(ngx_peer_connection_t *pc, void *data, ngx_uint_t state)
{
    ac_peer_t *peer = data;
    ac_node_t *node = peer->selected ? ac_node_state(peer->selected) : NULL;
    if (node) {
        if (node->active) node->active--;
        if (state & NGX_PEER_FAILED) { node->fails++; node->checked = ngx_current_msec; }
        else node->fails = 0;
    }
    peer->selected = NULL;
    if (pc->tries) pc->tries--;
}

void
ac_upstream_log(lua_State *L, ac_request_t *ctx)
{
    ngx_http_upstream_t *u = ctx->request->upstream;
    ac_node_t *nodes;
    ngx_uint_t i;
    if (ctx->native_peer) {
        ac_peer_t *peer = ctx->native_peer;
        ngx_peer_connection_t pc; ngx_memzero(&pc, sizeof(pc));
        ngx_uint_t failed = u && u->state && (u->state->status == 502 || u->state->status == 504) ? NGX_PEER_FAILED : 0;
        ac_peer_free(&pc, peer, failed);
    }
    if (!ctx->upstream || !u || !u->state) return;
    nodes = ctx->upstream->nodes.elts;
    for (i = 0; i < ctx->upstream->nodes.nelts; i++) {
        if (u->state->peer && u->state->peer->len == nodes[i].address.name.len
            && ngx_memcmp(u->state->peer->data, nodes[i].address.name.data, u->state->peer->len) == 0) {
            ac_node_t *state = ac_node_state(&nodes[i]);
            state->ewma = state->ewma == 0 ? u->state->response_time
                : state->ewma * 0.9 + u->state->response_time * 0.1;
            break;
        }
    }
}

static ngx_int_t
ac_var_get(ngx_http_request_t *r, ngx_http_variable_value_t *value, uintptr_t data)
{
    ac_request_t *ctx = ngx_http_get_module_ctx(r, ngx_http_apisix_core_module);
    ngx_str_t result;
    result.data = NULL; result.len = 0;
    if (ctx) {
        if (data == 0) result = ctx->upstream_scheme;
        else if (data == 1) result = ctx->upstream_host;
        else if (data == 2) result = ctx->upstream_uri;
        else result = ctx->native_variables[data];
    }
    if (!result.data) {
        if (ctx && (ctx->native_variable_set & ((uint64_t) 1 << data))) {
            value->not_found = 1; return NGX_OK;
        }
        if (data == 1 && r->headers_in.host) result = r->headers_in.host->value;
        else if (data == 8 || data == 10) {
            ngx_str_t name = data == 8 ? (ngx_str_t) ngx_string("request_id") : (ngx_str_t) ngx_string("upstream_response_time");
            ngx_http_variable_value_t *base = ngx_http_get_variable(r, &name, ngx_hash_key(name.data, name.len));
            if (base && !base->not_found) { *value = *base; return NGX_OK; }
        } else {
            result.data = (u_char *) ac_native_vars[data].value;
            result.len = ngx_strlen(result.data);
        }
    }
    if (!result.data) { value->not_found = 1; return NGX_OK; }
    value->valid = 1; value->no_cacheable = 1; value->not_found = 0;
    value->data = result.data; value->len = result.len; return NGX_OK;
}

static void
ac_var_set(ngx_http_request_t *r, ngx_http_variable_value_t *value, uintptr_t data)
{
    ac_request_t *ctx = ngx_http_get_module_ctx(r, ngx_http_apisix_core_module);
    ngx_str_t *dest;
    if (!ctx) return;
    dest = data == 0 ? &ctx->upstream_scheme : data == 1 ? &ctx->upstream_host
        : data == 2 ? &ctx->upstream_uri : &ctx->native_variables[data];
    dest->data = value->data; dest->len = value->len;
    ctx->native_variable_set |= (uint64_t) 1 << data;
}

ngx_int_t
ac_upstream_variables(ngx_conf_t *cf)
{
    ngx_http_variable_t *variable;
    ngx_uint_t i;
    ngx_str_t name;
    if (sizeof(ac_native_vars) / sizeof(ac_native_vars[0]) > 64) return NGX_ERROR;
    for (i = 0; i < sizeof(ac_native_vars) / sizeof(ac_native_vars[0]); i++) {
        name.data = (u_char *) ac_native_vars[i].name; name.len = ngx_strlen(name.data);
        variable = ngx_http_add_variable(cf, &name, NGX_HTTP_VAR_CHANGEABLE | NGX_HTTP_VAR_NOCACHEABLE);
        if (!variable) return NGX_ERROR;
        variable->get_handler = ac_var_get; variable->set_handler = ac_var_set; variable->data = i;
    }
    return NGX_OK;
}

static int
ac_upstream_set(lua_State *L)
{
    ac_request_t *ctx = ac_request(L);
    ac_main_conf_t scratch = *ctx->main;
    lua_settop(L, 4);
    luaL_checktype(L, 1, LUA_TTABLE);
    luaL_checktype(L, 4, LUA_TTABLE);
    ac_upstream_cleanup(L, ctx);
    scratch.pool = ctx->request->pool;
    lua_pushnil(L);
    ctx->upstream = ac_compile_upstream(L, &scratch, 4, -1);
    ctx->upstream_owned = 1;
    lua_pushvalue(L, 2); lua_setfield(L, 1, "upstream_key");
    lua_pushvalue(L, 3); lua_setfield(L, 1, "upstream_version");
    lua_pushvalue(L, 4); lua_setfield(L, 1, "upstream_conf");
    return 0;
}

void
ac_upstream_cleanup(lua_State *L, ac_request_t *ctx)
{
    if (ctx->native_peer) {
        ac_peer_t *peer = ctx->native_peer;
        ngx_peer_connection_t pc; ngx_memzero(&pc, sizeof(pc));
        ac_peer_free(&pc, ctx->native_peer, 0); ctx->native_peer = NULL;
        luaL_unref(L, LUA_REGISTRYINDEX, peer->ring_ref);
    }
    if (ctx->upstream_owned && ctx->upstream) {
        luaL_unref(L, LUA_REGISTRYINDEX, ctx->upstream->conf_ref);
        luaL_unref(L, LUA_REGISTRYINDEX, ctx->upstream->hash_cache_ref);
        ctx->upstream_owned = 0;
    }
}

int
ac_balance_enter(lua_State *L)
{
    ac_request_t *ctx = ac_request(L);
    ac_peer_t *peer = ctx->native_peer;
    ngx_peer_connection_t pc;
    if (!peer) { lua_pushboolean(L, 0); lua_pushinteger(L, 502); return 2; }
    if (ctx->balance_attempts) {
        if (peer->upstream->retry_timeout && ngx_current_msec - peer->started > peer->upstream->retry_timeout) {
            lua_pushboolean(L, 0); lua_pushinteger(L, 502); return 2;
        }
        int status = 0; char *error = NULL;
        int state = ngx_http_lua_ffi_balancer_get_last_failure(ctx->request, &status, &error);
        ngx_uint_t failed = state > 0 ? NGX_PEER_FAILED : 0;
        ngx_memzero(&pc, sizeof(pc)); ac_peer_free(&pc, peer, failed);
        if (ac_peer_get(&pc, peer, L) != NGX_OK) { lua_pushboolean(L, 0); lua_pushinteger(L, 502); return 2; }
    }
    lua_pushboolean(L, ctx->balance_attempts > 0); return 1;
}

int
ac_balance_finish(lua_State *L)
{
    ac_request_t *ctx = ac_request(L);
    ac_peer_t *peer = ctx->native_peer;
    ngx_uint_t retries;
    u_char ip[NGX_SOCKADDR_STRLEN];
    u_char pool[NGX_SOCKADDR_STRLEN + 64], *end;
    size_t length;
    char *error = NULL;
    if (!peer || !peer->selected) { lua_pushinteger(L, 502); return 1; }
    if (!ctx->balance_attempts) {
        retries = peer->upstream->retries == NGX_CONF_UNSET_UINT ? peer->upstream->nodes.nelts - 1 : peer->upstream->retries;
        if (retries) {
            if (ngx_http_lua_ffi_balancer_set_more_tries(ctx->request, (int) retries, &error) != NGX_OK)
                return luaL_error(L, "failed to set native retry count: %s", error ? error : "unknown error");
        }
        if (!peer->upstream->connect_timeout || !peer->upstream->send_timeout || !peer->upstream->read_timeout)
            return luaL_error(L, "native upstream timeouts must be positive");
        if (ngx_http_lua_ffi_balancer_set_timeouts(ctx->request, (long) peer->upstream->connect_timeout,
                 (long) peer->upstream->send_timeout, (long) peer->upstream->read_timeout, &error) != NGX_OK)
            return luaL_error(L, "failed to set native timeouts: %s", error ? error : "unknown error");
    } else {
        if (ngx_http_lua_ffi_balancer_recreate_request(ctx->request, &error) != NGX_OK)
            return luaL_error(L, "failed to recreate proxy request: %s", error ? error : "unknown error");
    }
    length = ngx_sock_ntop(peer->selected->address.sockaddr, peer->selected->address.socklen, ip, sizeof(ip), 0);
    end = ngx_snprintf(pool, sizeof(pool), "%V#%*s#%ui", &ctx->upstream_scheme, length, ip,
                       (ngx_uint_t) peer->selected->port);
    if (!peer->upstream->keepalive_size) return luaL_error(L, "native keepalive pool size must be positive");
    if (ngx_http_lua_ffi_balancer_set_current_peer(ctx->request, ip, length, peer->selected->port,
                ngx_crc32_long(pool, end - pool), (unsigned int) peer->upstream->keepalive_size, &error) != NGX_OK)
        return luaL_error(L, "failed to set native current peer: %s", error ? error : "unknown error");
    if (ngx_http_lua_ffi_balancer_enable_keepalive(ctx->request,
                (unsigned long) (peer->upstream->keepalive_idle * 1000),
                (unsigned int) peer->upstream->keepalive_requests, &error) != NGX_OK)
        return luaL_error(L, "failed to enable native keepalive: %s", error ? error : "unknown error");
    ctx->balance_attempts++;
    return 0;
}

ngx_flag_t
ac_upstream_enabled(ac_request_t *ctx)
{
    ac_loc_conf_t *conf = ngx_http_get_module_loc_conf(ctx->request, ngx_http_apisix_core_module);
    return conf->proxy;
}

void
ac_upstream_api(lua_State *L, int module)
{
    module = ac_abs(L, module);
    lua_pushcfunction(L, ac_upstream_set); lua_setfield(L, module, "set");
    lua_pushcfunction(L, ac_filter_upstream); lua_setfield(L, module, "filter_upstream");
}
