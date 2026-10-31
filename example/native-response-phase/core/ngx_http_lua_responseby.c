/* A separate yieldable context. Never pretends to be content_by_lua. */
#include "ngx_http_lua_responseby.h"
#include "ngx_http_lua_util.h"
#include "ngx_http_lua_cache.h"

static void
ngx_http_lua_response_cleanup(void *data)
{
    ngx_http_lua_ctx_t *ctx = data;
    ctx->entered_response_phase = 0;
    if (ctx->cleanup) { ngx_http_lua_request_cleanup_handler(ctx); }
}

void
ngx_http_lua_response_wev_handler(ngx_http_request_t *r)
{
    ngx_int_t rc;
    ngx_http_lua_ctx_t *ctx;
    if (r->response_ctx == NULL
        || r->response_ctx->state != NGX_HTTP_RESPONSE_RUNNING) { return; }
    ctx = ngx_http_get_module_ctx(r, ngx_http_lua_module);
    if (ctx == NULL || !ctx->entered_response_phase) { return; }
    ngx_http_response_trace(r, "resume");
    rc = ctx->resume_handler(r);
    if (rc == NGX_AGAIN || rc == NGX_DONE) {
        ngx_http_response_trace(r, "yield");
        return;
    }
    ctx->response_done = 1;
    ctx->response_rc = rc;
    ngx_http_response_continue(r);
}

ngx_int_t
ngx_http_lua_response_by_chunk(lua_State *L, ngx_http_request_t *r)
{
    int co_ref;
    ngx_int_t rc;
    lua_State *co;
    ngx_pool_cleanup_t *cln;
    ngx_http_lua_ctx_t *ctx = ngx_http_get_module_ctx(r, ngx_http_lua_module);
    if (ctx == NULL) {
        ctx = ngx_http_lua_create_ctx(r);
        if (ctx == NULL) { return NGX_ERROR; }
    } else {
        ngx_http_lua_reset_ctx(r, L, ctx);
    }
    ctx->context = NGX_HTTP_LUA_CONTEXT_RESPONSE;
    ctx->entered_response_phase = 1;
    ctx->response_done = 0;
    co = ngx_http_lua_new_thread(r, L, &co_ref);
    if (co == NULL) { return NGX_ERROR; }
    lua_xmove(L, co, 1);
#ifndef OPENRESTY_LUAJIT
    ngx_http_lua_get_globals_table(co);
    lua_setfenv(co, -2);
#endif
    ngx_http_lua_set_req(co, r);
    ctx->cur_co_ctx = &ctx->entry_co_ctx;
    ctx->cur_co_ctx->co = co;
    ctx->cur_co_ctx->co_ref = co_ref;
#ifdef NGX_LUA_USE_ASSERT
    ctx->cur_co_ctx->co_top = 1;
#endif
    ngx_http_lua_attach_co_ctx_to_L(co, ctx->cur_co_ctx);
    if (ctx->cleanup == NULL) {
        cln = ngx_pool_cleanup_add(r->pool, 0);
        if (cln == NULL) { return NGX_ERROR; }
        cln->handler = ngx_http_lua_request_cleanup_handler;
        cln->data = ctx;
        ctx->cleanup = &cln->handler;
    }
    r->response_ctx->lua_cleanup = ngx_http_lua_response_cleanup;
    r->response_ctx->lua_data = ctx;
    r->write_event_handler = ngx_http_lua_response_wev_handler;
    rc = ngx_http_lua_run_thread(L, r, ctx, 0);
    if (rc == NGX_AGAIN) { return NGX_AGAIN; }
    if (rc == NGX_DONE) {
        /* ngx_lua documents NGX_DONE as an additional request reference. */
        ngx_http_lua_finalize_request(r, NGX_DONE);
        return NGX_AGAIN;
    }
    ctx->response_done = 1;
    ctx->response_rc = rc;
    ctx->entered_response_phase = 0;
    return rc;
}

ngx_int_t
ngx_http_lua_response_handler(ngx_http_request_t *r)
{
    ngx_http_lua_ctx_t *ctx;
    ngx_http_lua_loc_conf_t *llcf;
    ctx = ngx_http_get_module_ctx(r, ngx_http_lua_module);
    if (ctx && ctx->entered_response_phase) {
        if (!ctx->response_done) { return NGX_AGAIN; }
        ctx->entered_response_phase = 0;
        return ctx->response_rc;
    }
    llcf = ngx_http_get_module_loc_conf(r, ngx_http_lua_module);
    if (llcf->response_handler == NULL) { return NGX_ERROR; }
    return llcf->response_handler(r);
}

static ngx_http_request_t *
ngx_http_lua_response_request(lua_State *L)
{
    ngx_http_request_t *r = ngx_http_lua_get_req(L);
    ngx_http_lua_ctx_t *ctx;
    if (r == NULL) { luaL_error(L, "no request"); return NULL; }
    ctx = ngx_http_get_module_ctx(r, ngx_http_lua_module);
    if (ctx == NULL || ctx->context != NGX_HTTP_LUA_CONTEXT_RESPONSE
        || r->response_ctx == NULL
        || r->response_ctx->state != NGX_HTTP_RESPONSE_RUNNING)
    { luaL_error(L, "API requires native response phase"); return NULL; }
    return r;
}

static int
ngx_http_lua_response_get_body(lua_State *L)
{
    ngx_http_request_t *r = ngx_http_lua_response_request(L);
    lua_pushlstring(L, (char *) r->response_ctx->body.data,
                   r->response_ctx->body.len);
    return 1;
}

static int
ngx_http_lua_response_set_body(lua_State *L)
{
    size_t len;
    const char *body = luaL_checklstring(L, 1, &len);
    ngx_http_request_t *r = ngx_http_lua_response_request(L);
    if (len > r->response_ctx->limit) {
        return luaL_error(L, "replacement response body too large");
    }
    ngx_memcpy(r->response_ctx->body.data, body, len);
    r->response_ctx->body.len = len;
    return 0;
}

static int
ngx_http_lua_response_clear_headers(lua_State *L)
{
    ngx_http_request_t *r = ngx_http_lua_response_request(L);
    if (ngx_http_response_clear_headers(r) != NGX_OK) {
        return luaL_error(L, "response header allocation failure");
    }
    return 0;
}

void
ngx_http_lua_inject_response_api(lua_State *L)
{
    lua_createtable(L, 0, 3);
    lua_pushcfunction(L, ngx_http_lua_response_get_body);
    lua_setfield(L, -2, "get_body");
    lua_pushcfunction(L, ngx_http_lua_response_set_body);
    lua_setfield(L, -2, "set_body");
    lua_pushcfunction(L, ngx_http_lua_response_clear_headers);
    lua_setfield(L, -2, "clear_headers");
    lua_setfield(L, -2, "response");
}

/* Inline/file code-cache loaders are adapted from contentby.c by adapt.py. */
