/* SPDX-License-Identifier: Apache-2.0 */
#include "engine.h"

/* Explicit native-plugin mode compiles immutable standalone configuration.
 * Request decisions below have no Lua operations. The optional adapter only
 * publishes the result into the existing Lua ABI for the middle experiment. */
typedef struct ac_key_consumer_s ac_key_consumer_t;
struct ac_key_consumer_s {
    ngx_str_t key, id, username, custom_id, credential_id;
    int ref, group_ref;
    ac_key_consumer_t *next;
};
struct ac_key_auth_index_s {
    ngx_uint_t mask;
    ac_key_consumer_t **buckets;
};
struct ac_key_auth_conf_s {
    ngx_str_t header, query, challenge, anonymous_name;
    ngx_flag_t hide, anonymous_configured;
    ac_key_consumer_t *anonymous;
};
typedef struct { ngx_str_t key, value; ngx_flag_t boolean; } ac_arg_t;

static ngx_str_t
ac_key_string(lua_State *L, ngx_pool_t *pool, int index, const char *name)
{
    ngx_str_t value = ngx_null_string;
    lua_getfield(L, index, name);
    if (lua_isstring(L, -1)) value = ac_string(L, pool, -1);
    lua_pop(L, 1); return value;
}

static ngx_flag_t
ac_key_equal(ngx_str_t a, ngx_str_t b)
{
    return a.len == b.len && (!a.len || ngx_memcmp(a.data, b.data, a.len) == 0);
}

static void
ac_key_consumer_fields(lua_State *L, ngx_pool_t *pool, ac_key_consumer_t *c, int value)
{
    value = ac_abs(L, value);
    c->id = ac_key_string(L, pool, value, "consumer_name");
    c->username = ac_key_string(L, pool, value, "username");
    c->custom_id = ac_key_string(L, pool, value, "custom_id");
    c->credential_id = ac_key_string(L, pool, value, "credential_id");
    c->ref = ac_ref(L, value);
}

void
ac_key_auth_prepare(lua_State *L, ac_main_conf_t *main)
{
    int top = lua_gettop(L), group, nodes, value, conf;
    ngx_uint_t i, n, size = 1, bucket;
    ac_key_consumer_t *c;
    ac_key_auth_index_t *index = ngx_pcalloc(main->pool, sizeof(*index));
    if (!index) luaL_error(L, "native key-auth index allocation failed");
    main->key_auth_index = index;
    ac_push(L, main->consumer_plugins_ref); lua_getfield(L, -1, "key-auth");
    group = lua_gettop(L);
    if (lua_istable(L, group)) lua_getfield(L, group, "nodes"); else lua_pushnil(L);
    nodes = lua_gettop(L); n = lua_istable(L, nodes) ? lua_objlen(L, nodes) : 0;
    while (size < n * 2 && size < 0x40000000) size <<= 1;
    index->mask = size - 1;
    index->buckets = ngx_pcalloc(main->pool, size * sizeof(c));
    if (!index->buckets) luaL_error(L, "native key-auth buckets allocation failed");
    for (i = 1; i <= n; i++) {
        lua_rawgeti(L, nodes, i); value = lua_gettop(L);
        c = ngx_pcalloc(main->pool, sizeof(*c));
        if (!c) luaL_error(L, "native key-auth consumer allocation failed");
        ac_key_consumer_fields(L, main->pool, c, value); c->group_ref = ac_ref(L, group);
        lua_getfield(L, value, "auth_conf"); conf = lua_gettop(L);
        c->key = ac_key_string(L, main->pool, conf, "key");
        if (!c->key.data || (c->key.len >= 10 && ngx_strncmp(c->key.data, "$secret://", 10) == 0)
            || (c->key.len >= 7 && ngx_strncasecmp(c->key.data, (u_char *) "$ENV://", 7) == 0))
            luaL_error(L, "native key-auth requires literal consumer keys; secret providers are unsupported");
        bucket = ngx_hash_key(c->key.data, c->key.len) & index->mask;
        /* Latest duplicate wins, matching the Lua consumer index. */
        c->next = index->buckets[bucket]; index->buckets[bucket] = c;
        lua_pop(L, 2);
    }
    lua_settop(L, top);
}

static ngx_str_t
ac_key_header(ngx_http_request_t *r, ngx_str_t name)
{
    ngx_list_part_t *part;
    ngx_table_elt_t *h;
    ngx_uint_t i, j, count, pass;
    ngx_str_t none = ngx_null_string;
    /* ngx.req.get_headers defaults to 100 fields and picks the first duplicate.
     * Its fallback header lookup accepts underscores as hyphens. */
    for (pass = 0; pass < 2; pass++) {
        part = &r->headers_in.headers.part; h = part->elts; count = 0;
        for (i = 0; ; i++) {
            if (i >= part->nelts) {
                if (!part->next) break;
                part = part->next; h = part->elts; i = 0;
            }
            if (++count > 100) break;
            if (!h[i].hash || h[i].key.len != name.len) continue;
            for (j = 0; j < name.len; j++) {
                u_char b = pass && name.data[j] == '_' ? '-' : ngx_tolower(name.data[j]);
                if (b != ngx_tolower(h[i].key.data[j])) break;
            }
            if (j == name.len) return h[i].value;
        }
        if (!ngx_strlchr(name.data, name.data + name.len, '_')) break;
    }
    return none;
}

static ngx_int_t
ac_key_decode(ngx_http_request_t *r, u_char *start, size_t length, ngx_str_t *out)
{
    u_char *p, *src, *dst;
    size_t i;
    out->data = ngx_pnalloc(r->pool, length + 1);
    if (!out->data) return NGX_ERROR;
    for (i = 0; i < length; i++) out->data[i] = start[i] == '+' ? ' ' : start[i];
    src = dst = p = out->data;
    ngx_unescape_uri(&dst, &src, length, 0);
    out->len = dst - p; return NGX_OK;
}

static ngx_int_t
ac_key_args(ngx_http_request_t *r, ngx_array_t *args)
{
    u_char *p = r->args.data, *end = p + r->args.len, *last, *equal;
    ac_arg_t *a;
    if (ngx_array_init(args, r->pool, 4, sizeof(ac_arg_t)) != NGX_OK) return NGX_ERROR;
    /* core.request.get_uri_args calls ngx.req.get_uri_args(0): unlimited. */
    while (p < end) {
        last = ngx_strlchr(p, end, '&'); if (!last) last = end;
        equal = ngx_strlchr(p, last, '=');
        if ((equal ? equal : last) != p) {
            a = ngx_array_push(args);
            if (!a) return NGX_ERROR;
            a->boolean = equal == NULL;
            if (ac_key_decode(r, p, (equal ? equal : last) - p, &a->key) != NGX_OK) return NGX_ERROR;
            a->value.len = 0; a->value.data = (u_char *) "";
            if (equal && ac_key_decode(r, equal + 1, last - equal - 1, &a->value) != NGX_OK) return NGX_ERROR;
        }
        p = last + (last != end);
    }
    return NGX_OK;
}

static ngx_int_t
ac_key_hide_query(ngx_http_request_t *r, ngx_array_t *args, ngx_str_t key)
{
    ac_arg_t *a = args->elts;
    ngx_uint_t i;
    size_t length = 0;
    u_char *out, *start;
    for (i = 0; i < args->nelts; i++) {
        if (ac_key_equal(a[i].key, key)) continue;
        length += a[i].key.len + 3 * ngx_escape_uri(NULL, a[i].key.data, a[i].key.len, NGX_ESCAPE_URI_COMPONENT) + 2;
        if (!a[i].boolean)
            length += a[i].value.len + 3 * ngx_escape_uri(NULL, a[i].value.data, a[i].value.len, NGX_ESCAPE_URI_COMPONENT);
    }
    start = out = ngx_pnalloc(r->pool, length + 1);
    if (!out) return NGX_ERROR;
    for (i = 0; i < args->nelts; i++) {
        if (ac_key_equal(a[i].key, key)) continue;
        if (out != start) *out++ = '&';
        out = (u_char *) ngx_escape_uri(out, a[i].key.data, a[i].key.len, NGX_ESCAPE_URI_COMPONENT);
        if (!a[i].boolean) {
            *out++ = '=';
            out = (u_char *) ngx_escape_uri(out, a[i].value.data, a[i].value.len, NGX_ESCAPE_URI_COMPONENT);
        }
    }
    r->args.data = start; r->args.len = out - start; r->valid_unparsed_uri = 0;
    return NGX_OK;
}

static ngx_int_t
ac_key_header_write(ngx_http_request_t *r, const char *name, ngx_str_t value, ngx_flag_t response)
{
    char *error = NULL;
    ngx_str_t n; n.data = (u_char *) name; n.len = ngx_strlen(name);
    if (ac_native_header_write(r, n, value, response, 0, &error) == NGX_OK) return NGX_OK;
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "native key-auth header failed: %s", error);
    return NGX_ERROR;
}

static ngx_int_t
ac_key_execute(ac_request_t *ctx, ac_key_auth_conf_t *conf, ac_key_consumer_t **consumer, const char **message)
{
    ngx_http_request_t *r = ctx->request;
    ngx_str_t key = ac_key_header(r, conf->header), nil = ngx_null_string;
    ac_key_auth_index_t *index = ctx->main->key_auth_index;
    ac_key_consumer_t *c = NULL;
    ngx_flag_t header = key.data != NULL, invalid_type = 0, missing;
    ngx_array_t args; ngx_memzero(&args, sizeof(args));
    ac_arg_t *a; ngx_uint_t i, found = 0;
    if (!header) {
        if (ac_key_args(r, &args) != NGX_OK) return 500;
        a = args.elts;
        for (i = 0; i < args.nelts; i++) if (ac_key_equal(a[i].key, conf->query)) {
            key = a[i].value; found++; invalid_type = a[i].boolean;
        }
        if (found > 1) invalid_type = 1;
    }
    missing = !header && !found;
    if (!missing && !invalid_type) {
        for (c = index->buckets[ngx_hash_key(key.data, key.len) & index->mask]; c; c = c->next)
            if (ac_key_equal(c->key, key)) break;
    }
    if (!c) {
        *message = missing ? "Missing API key in request" : "Invalid API key in request";
        if (!missing) ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                                   "failed to find consumer: invalid api key");
        if (conf->anonymous_configured) {
            if (conf->hide) {
                char *error = NULL;
                if (header && ac_native_header_write(r, conf->header, nil, 0, 0, &error) != NGX_OK) return 500;
                if (!args.elts && ac_key_args(r, &args) != NGX_OK) return 500;
                /* set_uri_args is only called when the credential field exists. */
                a = args.elts;
                for (i = 0; i < args.nelts; i++) if (ac_key_equal(a[i].key, conf->query)) break;
                if (i < args.nelts && ac_key_hide_query(r, &args, conf->query) != NGX_OK) return 500;
            }
            c = conf->anonymous;
            if (!c) {
                *message = "Invalid user authorization";
                ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                    "key-auth failed to authenticate the request, code: 401. error: failed to get anonymous consumer %V",
                    &conf->anonymous_name);
            }
        }
        if (!c) {
            if (ac_key_header_write(r, "WWW-Authenticate", conf->challenge, 1) != NGX_OK) return 500;
            return 401;
        }
    } else if (conf->hide) {
        char *error = NULL;
        if (header) {
            if (ac_native_header_write(r, conf->header, nil, 0, 0, &error) != NGX_OK) return 500;
        } else if (ac_key_hide_query(r, &args, conf->query) != NGX_OK) return 500;
    }
    if (ac_key_header_write(r, "X-Consumer-Username", c->username, 0) != NGX_OK
        || ac_key_header_write(r, "X-Credential-Identifier", c->credential_id, 0) != NGX_OK
        || ac_key_header_write(r, "X-Consumer-Custom-ID", c->custom_id, 0) != NGX_OK) return 500;
    *consumer = c; return NGX_OK;
}

ngx_int_t
ac_key_auth_run(ac_request_t *ctx, ac_key_auth_conf_t *conf, ngx_str_t *body)
{
    ac_key_consumer_t *consumer = NULL;
    const char *message = NULL;
    ngx_int_t rc = ac_key_execute(ctx, conf, &consumer, &message);
    if (rc == NGX_OK) ctx->native_consumer = consumer;
    if (rc == NGX_OK) ngx_log_error(NGX_LOG_INFO, ctx->request->connection->log, 0, "hit key-auth rewrite");
    if (rc == 401) {
        size_t length = ngx_strlen(message);
        body->data = ngx_pnalloc(ctx->request->pool, length + 16);
        if (!body->data) return 500;
        body->len = ngx_snprintf(body->data, length + 16, "{\"message\":\"%s\"}\n", message) - body->data;
    }
    return rc;
}

static int
ac_key_callback(lua_State *L)
{
    ac_key_auth_conf_t *conf = lua_touserdata(L, lua_upvalueindex(1));
    ac_request_t *ctx = ac_request(L);
    ac_key_consumer_t *c = NULL;
    const char *message = NULL;
    ngx_int_t rc = ac_key_execute(ctx, conf, &c, &message);
    if (rc != NGX_OK) {
        lua_pushinteger(L, rc);
        if (rc == 401) { lua_newtable(L); lua_pushstring(L, message); lua_setfield(L, -2, "message"); }
        else lua_pushnil(L);
        return 2;
    }
    ngx_log_error(NGX_LOG_INFO, ctx->request->connection->log, 0, "hit key-auth rewrite");
    ctx->native_consumer = c;
    ac_key_auth_publish(L, ctx);
    /* Invalidate the ordinary SDK's cached headers after direct C mutations. */
    ac_push(L, ctx->table_ref); lua_pushnil(L); lua_setfield(L, -2, "headers"); lua_pop(L, 1);
    return 0;
}

void
ac_key_auth_publish(lua_State *L, ac_request_t *ctx)
{
    ac_key_consumer_t *c = ctx->native_consumer;
    if (!c) return;
    ac_push(L, ctx->table_ref);
    ac_push(L, c->ref); lua_setfield(L, -2, "consumer");
    ac_push(L, c->ref); lua_getfield(L, -1, "consumer_name"); lua_setfield(L, -3, "consumer_name");
    lua_getfield(L, -1, "group_id"); lua_setfield(L, -3, "consumer_group_id"); lua_pop(L, 1);
    ac_push(L, c->group_ref); lua_getfield(L, -1, "conf_version"); lua_setfield(L, -3, "consumer_ver");
    lua_pop(L, 2);
}

void
ac_key_auth_compile(lua_State *L, ac_main_conf_t *main, ac_call_t *call, ngx_pool_t *pool)
{
    int top = lua_gettop(L), value, wrapper;
    ac_key_auth_conf_t *conf = ngx_pcalloc(pool, sizeof(*conf));
    ac_key_consumer_t *c;
    if (!conf) luaL_error(L, "native key-auth configuration allocation failed");
    ac_push(L, call->conf_ref); value = lua_gettop(L);
    conf->header = ac_key_string(L, pool, value, "header");
    conf->query = ac_key_string(L, pool, value, "query");
    ngx_str_t realm = ac_key_string(L, pool, value, "realm");
    conf->challenge.data = ngx_pnalloc(pool, realm.len + 16);
    if (!conf->challenge.data) luaL_error(L, "native key-auth realm allocation failed");
    conf->challenge.len = ngx_snprintf(conf->challenge.data, realm.len + 16, "apikey realm=\"%V\"", &realm) - conf->challenge.data;
    conf->hide = ac_bool_field(L, value, "hide_credentials");
    lua_getfield(L, value, "anonymous_consumer");
    conf->anonymous_configured = lua_toboolean(L, -1);
    if (conf->anonymous_configured) {
        conf->anonymous_name = ac_string(L, pool, -1);
        ac_resource_get(L, main, "consumers", -1); wrapper = lua_gettop(L);
        if (lua_istable(L, wrapper)) {
            lua_getfield(L, wrapper, "value");
            c = ngx_pcalloc(pool, sizeof(*c));
            if (!c) luaL_error(L, "native anonymous consumer allocation failed");
            ac_key_consumer_fields(L, pool, c, -1);
            c->id = ac_key_string(L, pool, lua_gettop(L), "id");
            lua_pushlstring(L, (char *) c->id.data, c->id.len); lua_setfield(L, -2, "consumer_name");
            lua_newtable(L); lua_getfield(L, wrapper, "modifiedIndex"); lua_setfield(L, -2, "conf_version");
            c->group_ref = ac_ref(L, -1); conf->anonymous = c;
        }
    }
    call->key_auth = conf;
    lua_pushlightuserdata(L, conf); lua_pushcclosure(L, ac_key_callback, 1);
    call->native_ref = ac_ref(L, -1);
    lua_settop(L, top);
}
