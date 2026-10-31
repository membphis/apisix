/* SPDX-License-Identifier: Apache-2.0 */
#include "engine.h"

static ngx_http_output_header_filter_pt next_header;
static ngx_http_output_body_filter_pt next_body;

ngx_int_t
ac_header_fields(ac_request_t *ctx)
{
    ngx_str_t header = ngx_string("Server"), name = ngx_string("upstream_status");
    ngx_str_t value = ctx->main->server_tokens ? (ngx_str_t) ngx_string("APISIX/3.19.0")
                                              : (ngx_str_t) ngx_string("APISIX");
    ngx_http_variable_value_t *status;
    char *error = NULL;
    if (ac_native_header_write(ctx->request, header, value, 1, 0, &error) != NGX_OK) goto failed;
    status = ngx_http_get_variable(ctx->request, &name, ngx_hash_key(name.data, name.len));
    if (status && !status->not_found) {
        ngx_int_t last = status->len >= 3 ? ngx_atoi(status->data + status->len - 3, 3) : -1;
        if (ctx->main->show_upstream_status || (last >= 500 && last <= 599)) {
            ngx_str_set(&header, "X-APISIX-Upstream-Status");
            value.data = status->data; value.len = status->len;
            if (ac_native_header_write(ctx->request, header, value, 1, 0, &error) != NGX_OK) goto failed;
        }
    }
    return NGX_OK;
failed:
    ngx_log_error(NGX_LOG_ERR, ctx->request->connection->log, 0,
                  "native header processing failed: %s", error ? error : "invalid request context");
    return NGX_ERROR;
}

static ngx_int_t
ac_native_header(ngx_http_request_t *request)
{
    ac_request_t *ctx = ngx_http_get_module_ctx(request, ngx_http_apisix_core_module);
    if (ctx && ctx->main->native_filters && ac_upstream_enabled(ctx)) {
        ctx->phase = AC_HEADER_FILTER;
        ctx->phase_entered = 0;
        if (ac_header_fields(ctx) != NGX_OK) return NGX_ERROR;
        if (request->headers_out.status == 101) {
            ngx_str_t type = ngx_string("websocket");
            u_char error[256]; size_t length = sizeof(error);
            if (ac_native_var_write(request, "request_type", type, error, &length) != NGX_OK) {
                ngx_log_error(NGX_LOG_ERR, request->connection->log, 0,
                              "native upgrade variable update failed: %*s", length, error);
                return NGX_ERROR;
            }
        }
    }
    return next_header(request);
}

static ngx_int_t
ac_native_body(ngx_http_request_t *request, ngx_chain_t *buffers)
{
    ac_request_t *ctx = ngx_http_get_module_ctx(request, ngx_http_apisix_core_module);
    if (ctx && ctx->main->native_filters && ac_upstream_enabled(ctx)) {
        if (ctx->c_pipeline) return next_body(request, buffers);
        /* Startup/transactional compilation reject tracing, scripts and Lua
         * filter callbacks in this mode. Both body callback plans are empty. */
        ctx->phase = AC_BODY_FILTER;
        ctx->phase_entered = 0;
        ac_phase_begin(ctx->main->vm, ctx, AC_BODY_FILTER);
        ac_phase_finish(ctx->main->vm, ctx);
    }
    return next_body(request, buffers);
}

ngx_int_t
ac_native_filters_register(ngx_conf_t *cf)
{
    ac_main_conf_t *main = ngx_http_conf_get_module_main_conf(cf, ngx_http_apisix_core_module);
    if (main->native_filters == NGX_CONF_UNSET) main->native_filters = 0;
    next_header = ngx_http_top_header_filter;
    ngx_http_top_header_filter = ac_native_header;
    next_body = ngx_http_top_body_filter;
    ngx_http_top_body_filter = ac_native_body;
    return NGX_OK;
}
