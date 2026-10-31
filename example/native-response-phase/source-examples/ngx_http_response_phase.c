/* Technical prototype: native HTTP proxy responses, small uncompressed text.
 * The normal header/body filter chains are entered ONLY at commit. */
#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

static void ngx_http_response_collect(ngx_http_request_t *r,
    ngx_http_upstream_t *u);
static void ngx_http_response_timeout(ngx_event_t *ev);
static void ngx_http_response_cleanup(void *data);
static void ngx_http_response_commit(ngx_http_request_t *r);

void
ngx_http_response_trace(ngx_http_request_t *r, const char *event)
{
    ngx_http_response_ctx_t *ctx = r->response_ctx;
    if (ctx == NULL) { return; }
    ngx_log_error(NGX_LOG_INFO, r->connection->log, 0,
                  "native_response r=%p seq=%ui event=%s state=%ui "
                  "header_sent=%ui header_calls=%ui count=%ui bytes=%uz uri=%V",
                  r, ++ctx->sequence, event, ctx->state,
                  (ngx_uint_t) r->header_sent, ctx->header_calls,
                  (ngx_uint_t) r->main->count, ctx->body.len, &r->uri);
}

ngx_flag_t
ngx_http_response_enabled(ngx_http_request_t *r)
{
    ngx_str_t v;
    ngx_http_core_loc_conf_t *clcf;
    clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);
    if (clcf->response_phase == NULL) { return 0; }
    if (ngx_http_complex_value(r, clcf->response_phase, &v) != NGX_OK) {
        return 1; /* configuration evaluation failure must not bypass audit */
    }
    return v.len != 0 && !(v.len == 1 && v.data[0] == '0')
           && !(v.len == 3 && ngx_strncmp(v.data, "off", 3) == 0);
}

static void
ngx_http_response_stop_lua(ngx_http_response_ctx_t *ctx)
{
    ngx_pool_cleanup_pt cleanup;
    if (ctx->lua_cleanup) {
        cleanup = ctx->lua_cleanup;
        ctx->lua_cleanup = NULL;
        cleanup(ctx->lua_data);
        ctx->lua_data = NULL;
    }
}

static void
ngx_http_response_cleanup(void *data)
{
    ngx_http_response_ctx_t *ctx = data;
    if (ctx->deadline.timer_set) { ngx_del_timer(&ctx->deadline); }
    ngx_http_response_stop_lua(ctx);
    ngx_http_response_trace(ctx->request, "cleanup");
    ctx->state = NGX_HTTP_RESPONSE_FINISHED;
    ctx->body.len = 0;
    ctx->body.data = NULL;
}

ngx_int_t
ngx_http_response_clear_headers(ngx_http_request_t *r)
{
    ngx_uint_t status = r->headers_out.status;
    ngx_memzero(&r->headers_out, sizeof(r->headers_out));
    if (ngx_list_init(&r->headers_out.headers, r->pool, 8,
                      sizeof(ngx_table_elt_t)) != NGX_OK
        || ngx_list_init(&r->headers_out.trailers, r->pool, 2,
                         sizeof(ngx_table_elt_t)) != NGX_OK)
    { return NGX_ERROR; }
    r->headers_out.status = status;
    r->headers_out.content_length_n = -1;
    r->headers_out.last_modified_time = -1;
    ngx_str_set(&r->headers_out.content_type, "text/plain; charset=utf-8");
    r->headers_out.content_type_len = sizeof("text/plain; charset=utf-8") - 1;
    return NGX_OK;
}

void
ngx_http_response_fail(ngx_http_request_t *r, ngx_uint_t status, const char *body)
{
    ngx_http_response_ctx_t *ctx = r->response_ctx;
    if (ctx == NULL || ctx->state >= NGX_HTTP_RESPONSE_COMMITTING) { return; }
    ngx_http_response_trace(r, "fail_closed");
    if (ctx->deadline.timer_set) { ngx_del_timer(&ctx->deadline); }
    ngx_http_response_stop_lua(ctx);
    ngx_http_upstream_response_detach(r);
    if (ngx_http_response_clear_headers(r) != NGX_OK) {
        ctx->state = NGX_HTTP_RESPONSE_FINISHED;
        ngx_http_upstream_response_finalize(r, NGX_ERROR);
        return;
    }
    r->headers_out.status = status;
    r->err_status = 0;
    ctx->body.data = (u_char *) body; /* immutable, static error string */
    ctx->body.len = ngx_strlen(body);
    ngx_http_response_commit(r);
}

static void
ngx_http_response_timeout(ngx_event_t *ev)
{
    ngx_http_request_t *r = ev->data;
    ngx_http_response_ctx_t *ctx = r->response_ctx;
    if (ctx->state == NGX_HTTP_RESPONSE_COLLECTING) {
        ngx_http_response_fail(r, 504, "upstream collection timeout\n");
    } else if (ctx->state == NGX_HTTP_RESPONSE_RUNNING) {
        ngx_http_response_fail(r, 504, "response phase timeout\n");
    }
}

static ngx_int_t
ngx_http_response_capture(ngx_http_request_t *r)
{
    ngx_chain_t *cl;
    size_t n;
    ngx_http_upstream_t *u = r->upstream;
    ngx_http_response_ctx_t *ctx = r->response_ctx;
    for (cl = u->out_bufs; cl; cl = cl->next) {
        n = cl->buf->last - cl->buf->pos;
        if (n > ctx->limit - ctx->body.len) { return NGX_DECLINED; }
        ngx_memcpy(ctx->body.data + ctx->body.len, cl->buf->pos, n);
        ctx->body.len += n;
        cl->buf->pos = cl->buf->last;
    }
    ngx_chain_update_chains(r->pool, &u->free_bufs, &u->busy_bufs,
                           &u->out_bufs, u->output.tag);
    u->buffer.pos = u->buffer.start;
    u->buffer.last = u->buffer.start;
    return NGX_OK;
}

static void
ngx_http_response_body_complete(ngx_http_request_t *r)
{
    ngx_http_core_loc_conf_t *clcf;
    ngx_http_response_ctx_t *ctx = r->response_ctx;
    ngx_http_response_trace(r, "body_complete");
    ngx_http_upstream_response_detach(r);
    ctx->state = NGX_HTTP_RESPONSE_RUNNING;
    clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);
    if (ctx->deadline.timer_set) { ngx_del_timer(&ctx->deadline); }
    ngx_add_timer(&ctx->deadline, clcf->response_timeout);
    /* This is now a response continuation, not the old upstream's writer. */
    r->read_event_handler = ngx_http_test_reading;
    r->write_event_handler = ngx_http_request_empty_handler;
    ngx_http_response_trace(r, "response_enter");
    ngx_http_response_continue(r);
}

void
ngx_http_response_start(ngx_http_request_t *r)
{
    ngx_http_cleanup_t *cln;
    ngx_http_core_loc_conf_t *clcf;
    ngx_http_response_ctx_t *ctx;
    ngx_http_upstream_t *u = r->upstream;
    ssize_t preread;
    ngx_int_t rc;
    ngx_str_t type;

    clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);
    ctx = ngx_pcalloc(r->pool, sizeof(*ctx));
    if (ctx == NULL) {
        ngx_http_upstream_response_finalize(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
        return;
    }
    r->response_ctx = ctx;
    ctx->state = NGX_HTTP_RESPONSE_COLLECTING;
    ctx->request = r;
    ctx->limit = clcf->response_body_limit;
    cln = ngx_http_cleanup_add(r, 0);
    if (cln == NULL) {
        ngx_http_upstream_response_finalize(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
        return;
    }
    cln->handler = ngx_http_response_cleanup;
    cln->data = ctx;
    ctx->deadline.handler = ngx_http_response_timeout;
    ctx->deadline.data = r;
    ctx->deadline.log = r->connection->log;
    ngx_http_response_trace(r, "collect_start");

    type = r->headers_out.content_type;
    if (r != r->main || r->method != NGX_HTTP_GET
        || r->http_version >= NGX_HTTP_VERSION_20 || r->header_only
        || !u->response_phase_supported || u->upgrade || u->store || u->cacheable
        || ctx->limit == 0 || ctx->limit > 65536
        || type.len < 10 || ngx_strncasecmp(type.data, (u_char *) "text/plain", 10)
        || (type.len > 10 && type.data[10] != ';' && type.data[10] != ' ')
        || r->headers_out.content_encoding != NULL)
    {
        ngx_http_response_fail(r, 502, "unsupported response for native phase\n");
        return;
    }
    if (u->headers_in.content_length_n > (off_t) ctx->limit) {
        ngx_http_response_fail(r, 502, "upstream body too large\n");
        return;
    }
    ctx->body.data = ngx_pnalloc(r->pool, ctx->limit);
    if (ctx->body.data == NULL) {
        ngx_http_response_fail(r, 500, "response allocation failure\n");
        return;
    }
    if (u->input_filter_init(u->input_filter_ctx) != NGX_OK) {
        ngx_http_response_fail(r, 502, "upstream framing failure\n");
        return;
    }
    u->keepalive = 0; /* no upstream pooling in this prototype */
    if (u->peer.connection->read->timer_set) {
        ngx_del_timer(u->peer.connection->read);
    }
    ngx_add_timer(&ctx->deadline, clcf->response_collect_timeout);
    u->read_event_handler = ngx_http_response_collect;
    preread = u->buffer.last - u->buffer.pos;
    u->buffer.last = u->buffer.pos;
    if (preread) {
        if (u->input_filter(u->input_filter_ctx, preread) != NGX_OK) {
            ngx_http_response_fail(r, 502, "upstream framing failure\n");
            return;
        }
        rc = ngx_http_response_capture(r);
        if (rc != NGX_OK) {
            ngx_http_response_fail(r, 502, "upstream body too large\n");
            return;
        }
    } else {
        u->buffer.pos = u->buffer.start;
        u->buffer.last = u->buffer.start;
    }
    ngx_http_response_collect(r, u);
}

static void
ngx_http_response_collect(ngx_http_request_t *r, ngx_http_upstream_t *u)
{
    ngx_connection_t *c = u->peer.connection;
    ngx_buf_t *b = &u->buffer;
    ssize_t n;
    ngx_http_response_ctx_t *ctx = r->response_ctx;
    if (ctx->state != NGX_HTTP_RESPONSE_COLLECTING) { return; }
    for ( ;; ) {
        if (u->length == 0) { ngx_http_response_body_complete(r); return; }
        n = c->recv(c, b->last, b->end - b->last);
        if (n == NGX_AGAIN) {
            if (ngx_handle_read_event(c->read, 0) != NGX_OK) {
                ngx_http_response_fail(r, 502, "upstream event failure\n");
            }
            return;
        }
        if (n == 0 && u->length == -1) {
            ngx_http_response_body_complete(r);
            return;
        }
        if (n <= 0) {
            ngx_http_response_fail(r, 502, "upstream read failure\n");
            return;
        }
        u->state->bytes_received += n;
        u->state->response_length += n;
        if (u->input_filter(u->input_filter_ctx, n) != NGX_OK) {
            ngx_http_response_fail(r, 502, "upstream framing failure\n");
            return;
        }
        if (ngx_http_response_capture(r) != NGX_OK) {
            ngx_http_response_fail(r, 502, "upstream body too large\n");
            return;
        }
    }
}

void
ngx_http_response_continue(ngx_http_request_t *r)
{
    ngx_http_handler_pt *handlers;
    ngx_http_core_main_conf_t *cmcf;
    ngx_http_response_ctx_t *ctx = r->response_ctx;
    ngx_int_t rc;
    if (ctx == NULL || ctx->state != NGX_HTTP_RESPONSE_RUNNING) { return; }
    cmcf = ngx_http_get_module_main_conf(r, ngx_http_core_module);
    handlers = cmcf->phases[NGX_HTTP_RESPONSE_PHASE].handlers.elts;
    if (cmcf->phases[NGX_HTTP_RESPONSE_PHASE].handlers.nelts == 0) {
        ngx_http_response_fail(r, 500, "no native response handler\n");
        return;
    }
    while (ctx->handler < cmcf->phases[NGX_HTTP_RESPONSE_PHASE].handlers.nelts) {
        rc = handlers[ctx->handler](r);
        if (rc == NGX_AGAIN || rc == NGX_DONE) {
            ngx_http_response_trace(r, "yield");
            return;
        }
        if (rc != NGX_OK && rc != NGX_DECLINED) {
            ngx_http_response_fail(r, 500, "response Lua failure\n");
            return;
        }
        ctx->handler++;
    }
    ngx_http_response_trace(r, "response_leave");
    ngx_http_response_commit(r);
}

static void
ngx_http_response_commit(ngx_http_request_t *r)
{
    ngx_int_t rc;
    ngx_buf_t *b;
    ngx_chain_t out;
    ngx_http_response_ctx_t *ctx = r->response_ctx;
    if (ctx->deadline.timer_set) { ngx_del_timer(&ctx->deadline); }
    ngx_http_response_stop_lua(ctx);
    ctx->state = NGX_HTTP_RESPONSE_COMMITTING;
    ngx_http_clear_content_length(r);
    r->headers_out.content_length_n = ctx->body.len;
    r->write_event_handler = ngx_http_request_empty_handler;
    ngx_http_response_trace(r, "commit");
    b = ngx_calloc_buf(r->pool);
    if (b == NULL) { ngx_http_upstream_response_finalize(r, NGX_ERROR); return; }
    b->pos = ctx->body.data;
    b->last = ctx->body.data + ctx->body.len;
    b->memory = ctx->body.len != 0;
    b->sync = ctx->body.len == 0;
    b->last_buf = 1;
    b->last_in_chain = 1;
    out.buf = b;
    out.next = NULL;
    rc = ngx_http_send_header(r);
    if (rc == NGX_OK && !r->header_only) { rc = ngx_http_output_filter(r, &out); }
    /* The supplied buffer already carries last_buf: upstream must not add EOF. */
    ctx->state = NGX_HTTP_RESPONSE_FINISHED;
    ngx_http_upstream_response_finalize(r, rc);
}
