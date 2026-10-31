/* SPDX-License-Identifier: Apache-2.0 */
#include "engine.h"

/* The same exported runtime primitive used by resty.core.var. It owns variable
 * validation, set handlers, indexed slots and request-pool string lifetimes. */
extern int ngx_http_lua_ffi_var_set(ngx_http_request_t *r, u_char *name_data,
    size_t name_len, u_char *lowcase_buf, u_char *value, size_t value_len,
    u_char *errbuf, size_t *errlen);

ngx_int_t
ac_native_var_write(ngx_http_request_t *r, const char *name,
    ngx_str_t value, u_char *error, size_t *error_length)
{
    u_char lower[128];
    size_t length = ngx_strlen(name);
    if (ac_main()->native_var_writes == 1) {
        ngx_int_t rc = ac_native_var_write_fast(r, name, value);
        if (rc == NGX_OK) return NGX_OK;
        if (rc == NGX_ERROR) {
            *error_length = ngx_snprintf(error, *error_length, "no memory") - error;
            return NGX_ERROR;
        }
    }
    if (length > sizeof(lower)) {
        *error_length = ngx_snprintf(error, *error_length, "native variable name too long") - error;
        return NGX_ERROR;
    }
    return ngx_http_lua_ffi_var_set(r, (u_char *) name, length, lower,
                                  value.data, value.len, error, error_length);
}

void
ac_native_var_set(lua_State *L, ngx_http_request_t *r, const char *name, ngx_str_t value)
{
    u_char error[256];
    size_t error_length = sizeof(error);
    if (ac_native_var_write(r, name, value, error, &error_length) != NGX_OK) {
        lua_pushlstring(L, (char *) error, error_length);
        lua_error(L);
    }
}

/* Store the same view that core.ctx exposes, without invoking user getters. */
void
ac_var_assign(lua_State *L, ac_request_t *ctx, const char *name,
    ngx_str_t value, ngx_flag_t write_through)
{
    if (write_through) ac_native_var_set(L, ctx->request, name, value);
    ac_push(L, ctx->table_ref); lua_getfield(L, -1, "var");
    lua_getfield(L, -1, "_cache");
    if (value.data) lua_pushlstring(L, (char *) value.data, value.len);
    else lua_pushnil(L);
    lua_setfield(L, -2, name); lua_pop(L, 3);
}

/* Hold a cache only across a contiguous group of native writes. Raw presence
 * checks avoid invoking arbitrary ctx/var __index callbacks while deciding if
 * batching is safe. A cache with a metatable retains the ordinary per-write
 * lookup path: its __newindex can replace ctx.var or _cache between writes. */
int
ac_var_batch_begin(lua_State *L, ac_request_t *ctx)
{
    int top = lua_gettop(L);
    if (ctx->main->batch_vars != 1) return 0;
    ac_push(L, ctx->table_ref);
    lua_pushliteral(L, "var"); lua_rawget(L, -2);
    if (!lua_istable(L, -1)) goto fallback;
    lua_pushliteral(L, "_cache"); lua_rawget(L, -2);
    if (!lua_istable(L, -1) || lua_getmetatable(L, -1)) goto fallback;
    return lua_gettop(L);
fallback:
    lua_settop(L, top);
    return 0;
}

void
ac_var_batch_assign(lua_State *L, ac_request_t *ctx, int cache,
    const char *name, ngx_str_t value, ngx_flag_t write_through)
{
    if (!cache) {
        ac_var_assign(L, ctx, name, value, write_through);
        return;
    }
    /* These groups write only this module's C variables/header primitives.
     * No Lua callback/getter may be inserted between begin and end. */
    if (write_through) ac_native_var_set(L, ctx->request, name, value);
    if (value.data) lua_pushlstring(L, (char *) value.data, value.len);
    else lua_pushnil(L);
    lua_setfield(L, cache, name);
}

void
ac_var_batch_end(lua_State *L, int cache)
{
    if (cache) lua_settop(L, cache - 3);
}

enum {
    AC_HTTP_XFP, AC_HTTP_XFH, AC_HTTP_XFPORT, AC_HTTP_FORWARDED, AC_HTTP_XFF,
    AC_HTTP_SCHEME, AC_HTTP_HOST_HEADER, AC_HTTP_HOST, AC_HTTP_PORT,
    AC_HTTP_REALIP, AC_HTTP_REMOTE, AC_HTTP_READ_FIELDS
};

static struct { ngx_str_t name; ngx_uint_t hash; } ac_http_read_plan[] = {
    {ngx_string("http_x_forwarded_proto"), 0},
    {ngx_string("http_x_forwarded_host"), 0},
    {ngx_string("http_x_forwarded_port"), 0},
    {ngx_string("http_forwarded"), 0},
    {ngx_string("http_x_forwarded_for"), 0},
    {ngx_string("scheme"), 0},
    {ngx_string("http_host"), 0},
    {ngx_string("host"), 0},
    {ngx_string("server_port"), 0},
    {ngx_string("realip_remote_addr"), 0},
    {ngx_string("remote_addr"), 0}
};

static ngx_str_t
ac_read_var(lua_State *L, ac_request_t *ctx, ngx_uint_t field)
{
    ngx_str_t key, result = ngx_null_string;
    const char *name = (char *) ac_http_read_plan[field].name.data;
    ngx_uint_t hash;
    ngx_http_variable_value_t *value;
    ngx_flag_t custom = 0;
    ac_push(L, ac_main()->var_getters_ref);
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, name); custom = lua_istable(L, -1); lua_pop(L, 1);
    }
    lua_pop(L, 1);
    if (ctx->main->http_var_plan == 1) {
        key = ac_http_read_plan[field].name;
        hash = ac_http_read_plan[field].hash;
    } else {
        key.data = (u_char *) name; key.len = ngx_strlen(name);
        hash = ngx_hash_key(key.data, key.len);
    }
    if (!custom) {
        value = ngx_http_get_variable(ctx->request, &key, hash);
        if (value && !value->not_found) { result.data = value->data; result.len = value->len; }
        return result;
    }
    ac_variable(L, ctx, key);
    if (!lua_isnil(L, -1)) result = ac_string(L, ctx->request->pool, -1);
    lua_pop(L, 1); return result;
}

static ngx_flag_t
ac_equal(ngx_str_t a, ngx_str_t b)
{
    return a.len == b.len && (!a.len || ngx_memcmp(a.data, b.data, a.len) == 0);
}

static ngx_int_t
ac_hex(u_char byte)
{
    if (byte >= '0' && byte <= '9') return byte - '0';
    byte = ngx_tolower(byte);
    return byte >= 'a' && byte <= 'f' ? byte - 'a' + 10 : NGX_ERROR;
}

static ngx_str_t
ac_decode(lua_State *L, ac_request_t *ctx, ngx_str_t path, ngx_flag_t keep_slash)
{
    ngx_str_t decoded;
    size_t i;
    ngx_int_t hi, lo;
    u_char *out;
    decoded.data = ngx_pnalloc(ctx->request->pool, path.len + 1);
    if (!decoded.data) {
        if (L) luaL_error(L, "URI decoding allocation failed");
        decoded.len = 0; return decoded;
    }
    out = decoded.data;
    for (i = 0; i < path.len; i++) {
        if (path.data[i] == '%' && i + 2 < path.len
            && (hi = ac_hex(path.data[i + 1])) != NGX_ERROR
            && (lo = ac_hex(path.data[i + 2])) != NGX_ERROR) {
            if (keep_slash && hi * 16 + lo == '/') out = ngx_cpymem(out, "%2F", 3);
            else *out++ = (u_char) (hi * 16 + lo);
            i += 2;
        } else *out++ = path.data[i];
    }
    decoded.len = out - decoded.data; return decoded;
}

static ngx_flag_t
ac_only_decoded_or_merged(lua_State *L, ac_request_t *ctx, ngx_str_t path, ngx_str_t uri)
{
    ngx_str_t decoded;
    size_t i, written = 0;
    /* Equality alone is not sufficient if an earlier rewrite installed an
     * escaped URI. Keep all percent-bearing paths on the original decoder. */
    if (ctx->main->plain_uri == 1 && ac_equal(path, uri)
        && !ngx_strlchr(path.data, path.data + path.len, '%')) return 1;
    decoded = ac_decode(L, ctx, path, 0);
    if (!decoded.data) return 0;
    if (ac_equal(decoded, uri)) return 1;
    for (i = 0; i < decoded.len; i++) {
        if (decoded.data[i] == '/' && written && decoded.data[written - 1] == '/') continue;
        decoded.data[written++] = decoded.data[i];
    }
    decoded.len = written; return ac_equal(decoded, uri);
}

/* Escape the decoded path and append the query as query data. */
ngx_str_t
ac_http_proxy_uri(lua_State *L, ac_request_t *ctx, ngx_str_t uri)
{
    ngx_str_t result, args = ctx->request->args;
    uintptr_t escaped = ngx_escape_uri(NULL, uri.data, uri.len, NGX_ESCAPE_URI);
    u_char *out;
    result.data = ngx_pnalloc(ctx->request->pool, uri.len + escaped * 2 + args.len + 1);
    if (!result.data) luaL_error(L, "proxy URI allocation failed");
    out = (u_char *) ngx_escape_uri(result.data, uri.data, uri.len, NGX_ESCAPE_URI);
    if (args.len) { *out++ = '?'; out = ngx_cpymem(out, args.data, args.len); }
    result.len = out - result.data; return result;
}

static ngx_uint_t
ac_servlet(lua_State *L, ac_request_t *ctx, ngx_str_t uri, ngx_str_t *normalized)
{
    size_t start = 0, end, cut, len;
    u_char *out;
    normalized->data = ngx_pnalloc(ctx->request->pool, uri.len + 1);
    if (!normalized->data) luaL_error(L, "servlet URI allocation failed");
    out = normalized->data;
    while (start <= uri.len) {
        for (end = start; end < uri.len && uri.data[end] != '/'; end++) { }
        for (cut = start; cut < end && uri.data[cut] != ';'; cut++) { }
        len = cut - start;
        if (cut < end) {
            if ((!len && end < uri.len) || (len == 1 && uri.data[start] == '.')
                || (len == 2 && ngx_memcmp(uri.data + start, "..", 2) == 0)
                || (len == 3 && ngx_strncasecmp(uri.data + start, (u_char *) "%2e", 3) == 0)
                || (len == 6 && ngx_strncasecmp(uri.data + start, (u_char *) "%2e%2e", 6) == 0)) {
                ngx_log_error(NGX_LOG_ERR, ctx->request->connection->log, 0,
                              "failed to normalize servlet URI: invalid parameter segment");
                return 400;
            }
        }
        out = ngx_cpymem(out, uri.data + start, len);
        if (end == uri.len) break;
        *out++ = '/'; start = end + 1;
    }
    normalized->len = out - normalized->data; return 0;
}

static ngx_flag_t
ac_trusted(lua_State *L, int address)
{
    ngx_flag_t trusted;
    address = ac_abs(L, address);
    if (ac_main()->trusted_ref == LUA_NOREF) return 0;
    ac_push(L, ac_main()->trusted_ref);
    lua_getfield(L, -1, "match"); lua_pushvalue(L, -2); lua_pushvalue(L, address);
    lua_call(L, 2, 1); trusted = lua_toboolean(L, -1); lua_pop(L, 2);
    return trusted;
}

static void
ac_set_header(lua_State *L, ac_request_t *ctx, const char *name, ngx_str_t value)
{
    ngx_str_t header;
    header.data = (u_char *) name; header.len = ngx_strlen(name);
    ac_native_header_set(L, ctx->request, header, value, 0, 0);
}

static void
ac_forwarded(lua_State *L, ac_request_t *ctx)
{
    static struct { const char *header, *original; ngx_uint_t field; } names[] = {
        {"X-Forwarded-Proto", "original_x_forwarded_proto", AC_HTTP_XFP},
        {"X-Forwarded-Host", "original_x_forwarded_host", AC_HTTP_XFH},
        {"X-Forwarded-Port", "original_x_forwarded_port", AC_HTTP_XFPORT},
        {"Forwarded", "original_forwarded", AC_HTTP_FORWARDED}
    };
    ngx_str_t originals[4], safe[4], inbound, empty = ngx_string(""), nil = ngx_null_string;
    ngx_str_t peer, port, host;
    size_t i, colon;
    int cache;
    ngx_flag_t trusted = 0;
    for (i = 0; i < 4; i++) {
        originals[i] = ac_read_var(L, ctx, names[i].field);
        ac_var_assign(L, ctx, names[i].original, originals[i].data ? originals[i] : empty, 1);
    }
    inbound = ac_read_var(L, ctx, AC_HTTP_XFF);
    if (inbound.data) ac_var_assign(L, ctx, "original_x_forwarded_for", inbound, 1);
    safe[0] = ac_read_var(L, ctx, AC_HTTP_SCHEME);
    host = ac_read_var(L, ctx, AC_HTTP_HOST_HEADER);
    safe[1] = host.len ? host : ac_read_var(L, ctx, AC_HTTP_HOST);
    port = ac_read_var(L, ctx, AC_HTTP_PORT);
    /* Match the original map's final :digits suffix (including IPv6 literals). */
    for (colon = host.len; colon && host.data[colon - 1] != ':'; colon--) { }
    if (colon && colon < host.len) {
        for (i = colon; i < host.len && host.data[i] >= '0' && host.data[i] <= '9'; i++) { }
        if (i == host.len) { port.data = host.data + colon; port.len = host.len - colon; }
    }
    safe[2] = port; safe[3] = nil;
    if (ctx->main->trusted_ref != LUA_NOREF) {
        peer = ac_read_var(L, ctx, AC_HTTP_REALIP);
        if (!peer.data) peer = ac_read_var(L, ctx, AC_HTTP_REMOTE);
        lua_pushlstring(L, (char *) peer.data, peer.len);
        trusted = ac_trusted(L, -1); lua_pop(L, 1);
    }
    cache = ac_var_batch_begin(L, ctx);
    for (i = 0; i < 4; i++) {
        if (trusted && originals[i].len) safe[i] = originals[i];
        ac_set_header(L, ctx, names[i].header, safe[i]);
        ac_var_batch_assign(L, ctx, cache,
            (char *) ac_http_read_plan[names[i].field].name.data, safe[i], 0);
    }
    if (ctx->main->trusted_ref != LUA_NOREF && !trusted && inbound.data) {
        ac_set_header(L, ctx, "X-Forwarded-For", nil);
        ac_var_batch_assign(L, ctx, cache, "http_x_forwarded_for", nil, 0);
    }
    ac_var_batch_end(L, cache);
}

ngx_uint_t
ac_http_prepare(lua_State *L, ac_request_t *ctx)
{
    ngx_http_request_t *r = ctx->request;
    ngx_str_t uri = r->uri, raw = r->unparsed_uri, path = raw, normalized = uri, joined;
    u_char *query, *out;
    ngx_uint_t code;
    int cache;
    query = ngx_strlchr(raw.data, raw.data + raw.len, '?');
    if (query) path.len = query - path.data;
    if (ctx->main->delete_tail_slash && normalized.len && normalized.data[normalized.len - 1] == '/') normalized.len--;
    if (ctx->main->servlet_uri) {
        code = ac_servlet(L, ctx, uri, &normalized);
        if (code) return code;
        ctx->upstream_uri = ac_http_proxy_uri(L, ctx, uri);
        ac_var_assign(L, ctx, "upstream_uri", ctx->upstream_uri, 1);
    } else if (!ac_only_decoded_or_merged(L, ctx, path, uri)) {
        ac_ngx(L, "req", "set_uri");
        lua_pushlstring(L, (char *) uri.data, uri.len);
        lua_pushboolean(L, 0); lua_pushboolean(L, 1); lua_call(L, 3, 0);
    }
    ctx->normalized_uri = normalized;
    ctx->routing_uri = normalized;
    cache = ac_var_batch_begin(L, ctx);
    ac_var_batch_assign(L, ctx, cache, "uri", normalized, 0);
    ac_var_batch_assign(L, ctx, cache, "real_request_uri", raw, 0);
    if (ctx->main->plain_uri == 1 && !r->args.len) {
        /* ac_var_batch_assign copies the Lua string; this buffer is not mutated. */
        joined = normalized;
    } else {
        joined.data = ngx_pnalloc(r->pool, normalized.len + r->args.len + 1);
        if (!joined.data) return luaL_error(L, "normalized request URI allocation failed");
        out = ngx_cpymem(joined.data, normalized.data, normalized.len);
        if (r->args.len) { *out++ = '?'; out = ngx_cpymem(out, r->args.data, r->args.len); }
        joined.len = out - joined.data;
    }
    ac_var_batch_assign(L, ctx, cache, "request_uri", joined, 0);
    ac_var_batch_assign(L, ctx, cache, "request_line", r->request_line, 1);
    ac_var_batch_end(L, cache);
    ac_forwarded(L, ctx);
    if (ctx->main->encoded_slash) {
        ngx_str_t decoded = ac_decode(L, ctx, path, 0);
        if (ac_equal(decoded, normalized)) ctx->routing_uri = ac_decode(L, ctx, path, 1);
    }
    ac_var_assign(L, ctx, "uri", ctx->routing_uri, 0);
    return 0;
}

void
ac_http_match_finish(lua_State *L, ac_request_t *ctx)
{
    ac_var_assign(L, ctx, "uri", ctx->normalized_uri, 0);
}

/* C-only request preparation for the explicitly validated native-plugin mode.
 * Uses the same URI policy; no observable Lua cache/getter exists in this mode. */
ngx_int_t
ac_http_prepare_native(ac_request_t *ctx)
{
    ngx_http_request_t *r = ctx->request;
    ngx_str_t path = r->unparsed_uri, host, port, value, nil = ngx_null_string;
    ngx_str_t names[4] = {ngx_string("X-Forwarded-Proto"), ngx_string("X-Forwarded-Host"),
                         ngx_string("X-Forwarded-Port"), ngx_string("Forwarded")};
    ngx_str_t values[4];
    ngx_http_variable_value_t *v;
    ngx_str_t port_name = ngx_string("server_port"), scheme = ngx_string("scheme");
    u_char *query; size_t i, colon;
    char *error = NULL;
    query = ngx_strlchr(path.data, path.data + path.len, '?');
    if (query) path.len = query - path.data;
    if (!ac_only_decoded_or_merged(NULL, ctx, path, r->uri)) r->valid_unparsed_uri = 0;
    ctx->normalized_uri = ctx->routing_uri = r->uri;
    if (ctx->main->delete_tail_slash && ctx->routing_uri.len
        && ctx->routing_uri.data[ctx->routing_uri.len - 1] == '/') ctx->routing_uri.len--;
    ctx->native_variables[6] = r->request_line;
    ctx->native_variable_set |= (uint64_t) 1 << 6;
    v = ngx_http_get_variable(r, &ac_http_read_plan[AC_HTTP_XFF].name,
                             ac_http_read_plan[AC_HTTP_XFF].hash);
    if (v && !v->not_found) {
        ctx->native_variables[33].data = v->data; ctx->native_variables[33].len = v->len;
    }
    host = r->headers_in.host ? r->headers_in.host->value : r->headers_in.server;
    v = ngx_http_get_variable(r, &port_name, ngx_hash_key(port_name.data, port_name.len));
    port = nil; if (v && !v->not_found) { port.data = v->data; port.len = v->len; }
    for (colon = host.len; colon && host.data[colon - 1] != ':'; colon--) { }
    if (colon && colon < host.len) {
        for (i = colon; i < host.len && host.data[i] >= '0' && host.data[i] <= '9'; i++) { }
        if (i == host.len) { port.data = host.data + colon; port.len = host.len - colon; }
    }
    v = ngx_http_get_variable(r, &scheme, ngx_hash_key(scheme.data, scheme.len));
    value = nil; if (v && !v->not_found) { value.data = v->data; value.len = v->len; }
    values[0] = value; values[1] = host; values[2] = port; values[3] = nil;
    for (i = 0; i < 4; i++) {
        /* Originals are still exposed through the same NGINX variable names. */
        ngx_http_variable_value_t *original;
        ngx_str_t name = ac_http_read_plan[i].name;
        original = ngx_http_get_variable(r, &name, ac_http_read_plan[i].hash);
        ctx->native_variables[34 + i].data = original && !original->not_found ? original->data : (u_char *) "";
        ctx->native_variables[34 + i].len = original && !original->not_found ? original->len : 0;
        if (ac_native_header_write(r, names[i], values[i], 0, 0, &error) != NGX_OK) return 500;
    }
    return NGX_OK;
}

void
ac_http_worker(lua_State *L)
{
    ac_main_conf_t *main = ac_main();
    int top = lua_gettop(L), apisix;
    main->server_tokens = 1;
    ac_push(L, main->local_ref); lua_getfield(L, -1, "apisix");
    if (lua_istable(L, -1)) {
        apisix = lua_gettop(L);
        main->delete_tail_slash = ac_bool_field(L, apisix, "delete_uri_tail_slash");
        main->servlet_uri = ac_bool_field(L, apisix, "normalize_uri_like_servlet");
        main->encoded_slash = ac_bool_field(L, apisix, "match_uri_encoded_slash");
        lua_getfield(L, apisix, "enable_server_tokens");
        main->server_tokens = !(lua_isboolean(L, -1) && !lua_toboolean(L, -1)); lua_pop(L, 1);
        main->show_upstream_status = ac_bool_field(L, apisix, "show_upstream_status_in_response_header");
        lua_getfield(L, apisix, "trusted_addresses");
        if (lua_toboolean(L, -1)) {
            ac_require(L, "resty.ipmatcher"); lua_getfield(L, -1, "new");
            lua_pushvalue(L, -3); lua_call(L, 1, 2);
            if (lua_toboolean(L, -2)) {
                if (main->trusted_ref != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, main->trusted_ref);
                main->trusted_ref = ac_ref(L, -2);
            } else ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                                  "failed to create trusted address matcher: %s", lua_tostring(L, -1));
        }
    }
    lua_settop(L, top);
}

static int ac_trusted_init(lua_State *L) { ac_http_worker(L); return 0; }
static int ac_is_trusted(lua_State *L) { lua_pushboolean(L, ac_trusted(L, 1)); return 1; }
static int ac_is_configured(lua_State *L) { lua_pushboolean(L, ac_main()->trusted_ref != LUA_NOREF); return 1; }

static int
ac_trusted_open(lua_State *L)
{
    static const luaL_Reg functions[] = {
        {"init_worker", ac_trusted_init}, {"is_trusted", ac_is_trusted}, {"is_configured", ac_is_configured}, {NULL, NULL}
    };
    lua_newtable(L); luaL_register(L, NULL, functions); return 1;
}

ngx_int_t
ac_http_register(ngx_conf_t *cf)
{
    ngx_uint_t i;
    for (i = 0; i < AC_HTTP_READ_FIELDS; i++) {
        ac_http_read_plan[i].hash = ngx_hash_key(ac_http_read_plan[i].name.data,
                                               ac_http_read_plan[i].name.len);
    }
    return ngx_http_lua_add_package_preload(cf, "apisix.utils.trusted-addresses", ac_trusted_open);
}
