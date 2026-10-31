/* SPDX-License-Identifier: Apache-2.0 */
#include "engine.h"

static void
ac_pipeline_cleanup(void *data)
{
    ac_request_t *ctx = data;
    /* No Lua refs belong to this request. The watcher owns generation GC. */
    ac_snapshot_release(NULL, ctx);
    ctx->released = 1; ctx->request = NULL;
}

static ngx_int_t
ac_pipeline_log(ngx_http_request_t *r)
{
    ac_request_t *ctx = ngx_http_get_module_ctx(r, ngx_http_apisix_core_module);
    if (ctx && ctx->c_pipeline) { ac_upstream_log(NULL, ctx); ctx->log_done = 1; }
    return NGX_OK;
}

static ngx_int_t
ac_pipeline_access(ngx_http_request_t *r)
{
    ac_main_conf_t *main = ac_main();
    ac_loc_conf_t *loc = ngx_http_get_module_loc_conf(r, ngx_http_apisix_core_module);
    ac_request_t *ctx;
    ngx_pool_cleanup_t *cleanup;
    ngx_int_t rc;
    ngx_str_t body = ngx_null_string;
    ngx_buf_t *buffer; ngx_chain_t chain;
    if (!loc->proxy || main->native_pipeline != 1) return NGX_DECLINED;
    ctx = ngx_http_get_module_ctx(r, ngx_http_apisix_core_module);
    if (ctx) return NGX_DECLINED;
    if (!main->compiled) return NGX_HTTP_SERVICE_UNAVAILABLE;
    ctx = ngx_pcalloc(r->pool, sizeof(*ctx)); cleanup = ngx_pool_cleanup_add(r->pool, 0);
    if (!ctx || !cleanup) return NGX_HTTP_INTERNAL_SERVER_ERROR;
    ctx->request = r; ctx->main = main; ctx->c_pipeline = 1;
    ctx->table_ref = ctx->match_opts_ref = ctx->matched_ref = ctx->span_ref = LUA_NOREF;
    ac_snapshot_acquire(ctx);
    cleanup->data = ctx; cleanup->handler = ac_pipeline_cleanup;
    ngx_http_set_ctx(r, ctx, ngx_http_apisix_core_module);
    rc = ac_http_prepare_native(ctx);
    if (rc == NGX_OK) rc = ac_match_native(ctx);
    if (rc == NGX_HTTP_NOT_FOUND) ngx_str_set(&body, "{\"error_msg\":\"404 Route Not Found\"}\n");
    if (rc == NGX_OK && ctx->route->key_auth) rc = ac_key_auth_run(ctx, ctx->route->key_auth, &body);
    if (rc == NGX_OK) rc = ac_upstream_prepare_native(ctx);
    if (rc == NGX_OK) return NGX_DECLINED;
    if (!body.data) return rc;
    r->headers_out.status = rc;
    r->headers_out.content_length_n = body.len;
    ngx_str_set(&r->headers_out.content_type, "application/json");
    if (ngx_http_discard_request_body(r) != NGX_OK) return NGX_HTTP_INTERNAL_SERVER_ERROR;
    rc = ngx_http_send_header(r);
    if (rc == NGX_ERROR || rc > NGX_OK) return rc;
    if (r->header_only) {
        /* NGX_OK from an access handler permits the proxy phase to continue.
         * A rejected HEAD must terminate just like a rejected GET. */
        ngx_http_finalize_request(r, NGX_OK);
        return NGX_DONE;
    }
    buffer = ngx_calloc_buf(r->pool);
    if (!buffer) return NGX_HTTP_INTERNAL_SERVER_ERROR;
    buffer->pos = body.data; buffer->last = body.data + body.len;
    buffer->memory = 1; buffer->last_buf = r == r->main; buffer->last_in_chain = 1;
    chain.buf = buffer; chain.next = NULL;
    rc = ngx_http_output_filter(r, &chain);
    ngx_http_finalize_request(r, rc);
    return NGX_DONE;
}

static void
ac_pipeline_plan(lua_State *L, ac_plan_t *plan, ac_route_t *route)
{
    ac_call_t *calls = plan->calls.elts;
    ngx_uint_t i;
    for (i = 0; i < plan->calls.nelts; i++) {
        if (!route || !calls[i].key_auth || calls[i].pre_ref != LUA_NOREF || calls[i].expression)
            luaL_error(L, "native key-auth experiment requires route-only key-auth without Lua metadata callbacks");
        ac_push(L, calls[i].conf_ref); lua_getfield(L, -1, "_meta");
        if (lua_istable(L, -1)) {
            lua_getfield(L, -1, "error_response");
            if (!lua_isnil(L, -1)) luaL_error(L, "native key-auth error_response is unsupported");
            lua_pop(L, 1);
        }
        lua_pop(L, 2); route->key_auth = calls[i].key_auth;
    }
}

/* Check the closed-world contract before publishing a configuration, including
 * on hot reload. Unsupported settings cannot silently enter the hot path. */
void
ac_pipeline_compile(lua_State *L, ac_main_conf_t *main)
{
    ac_route_t *routes = main->routes.elts;
    ngx_uint_t i;
    int top = lua_gettop(L), values, j, value, plugins;
    ac_plugin_t *enabled = main->plugins.elts;
    if (main->plugins.nelts != 1 || enabled[0].name.len != 8
        || ngx_memcmp(enabled[0].name.data, "key-auth", 8))
        luaL_error(L, "native key-auth experiment requires a key-auth-only plugin allowlist");
    ac_pipeline_plan(L, &main->global, NULL);
    for (i = 0; i < main->routes.nelts; i++) {
        if (routes[i].filter_ref != LUA_NOREF || routes[i].expression)
            luaL_error(L, "native key-auth pipeline does not support Lua route predicates");
        ac_push(L, routes[i].value_ref); lua_getfield(L, -1, "script");
        if (!lua_isnil(L, -1)) luaL_error(L, "native key-auth pipeline does not support Lua route scripts");
        lua_pop(L, 2);
        ac_pipeline_plan(L, &routes[i].plan, &routes[i]);
        if (main->native_pipeline == 1) ac_upstream_validate_native(L, main, &routes[i]);
    }
    /* A consumer or group cannot introduce an unexecuted plugin after auth. */
    for (i = 0; i < 2; i++) {
        ac_resource(L, main, i ? "consumer_groups" : "consumers");
        lua_getfield(L, -1, "values"); values = lua_gettop(L);
        for (j = 1; j <= (int) lua_objlen(L, values); j++) {
            lua_rawgeti(L, values, j); lua_getfield(L, -1, "value"); value = lua_gettop(L);
            lua_getfield(L, value, "group_id");
            if (lua_toboolean(L, -1)) {
                ac_resource_get(L, main, "consumer_groups", -1);
                if (!lua_istable(L, -1)) luaL_error(L, "native key-auth references a missing consumer group");
                lua_pop(L, 1);
            }
            lua_pop(L, 1); lua_getfield(L, value, "plugins"); plugins = lua_gettop(L);
            if (lua_istable(L, plugins)) {
                lua_pushnil(L);
                while (lua_next(L, plugins)) {
                    size_t n; const char *name = lua_tolstring(L, -2, &n);
                    if (i || !name || n != 8 || ngx_memcmp(name, "key-auth", 8))
                        luaL_error(L, "native key-auth consumer/group contains an unsupported plugin");
                    lua_pop(L, 1);
                }
            }
            lua_pop(L, 3);
        }
        lua_pop(L, 2);
    }
    if (main->native_pipeline == 1 && (!main->native_filters || main->tracing || main->servlet_uri
        || main->encoded_slash || main->trusted_ref >= 0))
        luaL_error(L, "native pipeline currently requires no tracing/servlet/encoded-slash/trusted-address extension");
    lua_settop(L, top);
}

ngx_int_t
ac_pipeline_register(ngx_conf_t *cf)
{
    ngx_http_core_main_conf_t *core = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);
    ngx_http_handler_pt *handler;
    handler = ngx_array_push(&core->phases[NGX_HTTP_ACCESS_PHASE].handlers);
    if (!handler) return NGX_ERROR;
    *handler = ac_pipeline_access;
    handler = ngx_array_push(&core->phases[NGX_HTTP_LOG_PHASE].handlers);
    if (!handler) return NGX_ERROR;
    *handler = ac_pipeline_log;
    return NGX_OK;
}
