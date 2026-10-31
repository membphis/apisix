/* Technical prototype only; not a supported Nginx API. */
#ifndef _NGX_HTTP_RESPONSE_PHASE_H_INCLUDED_
#define _NGX_HTTP_RESPONSE_PHASE_H_INCLUDED_

#define NGX_HTTP_RESPONSE_COLLECTING  1
#define NGX_HTTP_RESPONSE_RUNNING     2
#define NGX_HTTP_RESPONSE_COMMITTING  3
#define NGX_HTTP_RESPONSE_FINISHED    4

struct ngx_http_response_ctx_s {
    ngx_uint_t          state;
    ngx_uint_t          handler;
    ngx_uint_t          sequence;
    ngx_uint_t          header_calls;
    size_t              limit;
    ngx_str_t           body;
    ngx_event_t         deadline;
    ngx_http_request_t *request;
    ngx_pool_cleanup_pt lua_cleanup;
    void               *lua_data;
};

ngx_flag_t ngx_http_response_enabled(ngx_http_request_t *r);
void ngx_http_response_start(ngx_http_request_t *r);
void ngx_http_response_continue(ngx_http_request_t *r);
void ngx_http_response_trace(ngx_http_request_t *r, const char *event);
ngx_int_t ngx_http_response_clear_headers(ngx_http_request_t *r);
void ngx_http_response_fail(ngx_http_request_t *r, ngx_uint_t status,
    const char *body);
/* Bridges to the existing, private upstream lifecycle functions. */
void ngx_http_upstream_response_detach(ngx_http_request_t *r);
void ngx_http_upstream_response_finalize(ngx_http_request_t *r, ngx_int_t rc);

#endif
