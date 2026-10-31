#!/usr/bin/env python3
"""Apply the narrow research changes to a version-pinned, API7-patched bundle.

The delivery also exports a conventional patch. This adapter checks each anchor
instead of silently editing a different source version. Run once on fresh source.
"""
import argparse
from pathlib import Path
import re


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("source", type=Path)
    root = p.parse_args().source.resolve() / "bundle"
    ng = root / "nginx-1.27.1"
    lu = root / "ngx_lua-0.10.28"
    here = Path(__file__).resolve().parent

    def edit(path, old, new, count=1):
        s = path.read_text()
        found = s.count(old)
        assert found == count, (str(path), old[:100], found, count)
        path.write_text(s.replace(old, new))

    def insert(path, anchor, new):
        edit(path, anchor, new + anchor)

    for name in ("ngx_http_response_phase.c", "ngx_http_response_phase.h"):
        (ng / "src/http" / name).write_bytes((here / "core" / name).read_bytes())
    for name in ("ngx_http_lua_responseby.c", "ngx_http_lua_responseby.h"):
        (lu / "src" / name).write_bytes((here / "core" / name).read_bytes())

    edit(ng/"src/http/ngx_http.h", "typedef struct ngx_http_request_s",
         "typedef struct ngx_http_response_ctx_s ngx_http_response_ctx_t;\n\ntypedef struct ngx_http_request_s")
    edit(ng/"src/http/ngx_http.h", "#include <ngx_http_core_module.h>",
         "#include <ngx_http_core_module.h>\n#include <ngx_http_response_phase.h>")
    path = ng/"src/http/ngx_http_request.h"
    s = path.read_text()
    s, n = re.subn(r"(    ngx_http_upstream_t\s+\*upstream;\n)",
                   r"\1    ngx_http_response_ctx_t *response_ctx;\n", s)
    assert n == 1
    path.write_text(s)
    path = ng/"src/http/ngx_http_upstream.h"
    s = path.read_text()
    s, n = re.subn(r"(    unsigned\s+buffering:1;)",
                   "    unsigned response_phase_supported:1;\n\\1", s)
    assert n == 1
    path.write_text(s)
    edit(ng/"auto/modules", "src/http/ngx_http_upstream.c \\",
         "src/http/ngx_http_response_phase.c \\\n                         src/http/ngx_http_upstream.c \\")
    edit(ng/"auto/modules", "src/http/ngx_http_core_module.h \\",
         "src/http/ngx_http_response_phase.h \\\n                         src/http/ngx_http_core_module.h \\")
    edit(ng/"src/http/ngx_http_core_module.h", "    NGX_HTTP_CONTENT_PHASE,\n",
         "    NGX_HTTP_CONTENT_PHASE,\n\n    NGX_HTTP_RESPONSE_PHASE, /* independently resumed upstream phase */\n")
    insert(ng/"src/http/ngx_http_core_module.h", "    off_t         client_max_body_size;",
           "    ngx_http_complex_value_t *response_phase;\n    size_t response_body_limit;\n"
           "    ngx_msec_t response_timeout;\n    ngx_msec_t response_collect_timeout;\n\n")
    path = ng/"src/http/ngx_http.c"
    anchor = "    if (ngx_array_init(&cmcf->phases[NGX_HTTP_LOG_PHASE].handlers,"
    insert(path, anchor, "    if (ngx_array_init(&cmcf->phases[NGX_HTTP_RESPONSE_PHASE].handlers,\n"
           "                       cf->pool, 1, sizeof(ngx_http_handler_pt)) != NGX_OK)\n"
           "    { return NGX_ERROR; }\n\n")
    edit(path, "    for (i = 0; i < NGX_HTTP_LOG_PHASE; i++) {\n",
         "    for (i = 0; i < NGX_HTTP_LOG_PHASE; i++) {\n"
         "        if (i == NGX_HTTP_RESPONSE_PHASE) { continue; }\n", count=2)
    path = ng/"src/http/ngx_http_core_module.c"
    anchor = '    { ngx_string("keepalive_timeout"),'
    directives = ""
    for name, setter in (("response_phase", "complex_value"), ("response_body_limit", "size"),
                         ("response_timeout", "msec"), ("response_collect_timeout", "msec")):
        directives += (f'    {{ ngx_string("{name}"),\n'
            '      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,\n'
            f'      ngx_http_set_complex_value_slot,\n' if setter == "complex_value" else
            f'    {{ ngx_string("{name}"),\n'
            '      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,\n'
            f'      ngx_conf_set_{setter}_slot,\n')
        directives += (f'      NGX_HTTP_LOC_CONF_OFFSET,\n'
            f'      offsetof(ngx_http_core_loc_conf_t, {name}), NULL }},\n\n')
    insert(path, anchor, directives)
    insert(path, "    clcf->keepalive_timeout = NGX_CONF_UNSET_MSEC;",
           "    clcf->response_phase = NGX_CONF_UNSET_PTR;\n"
           "    clcf->response_body_limit = NGX_CONF_UNSET_SIZE;\n"
           "    clcf->response_timeout = NGX_CONF_UNSET_MSEC;\n"
           "    clcf->response_collect_timeout = NGX_CONF_UNSET_MSEC;\n")
    insert(path, "    ngx_conf_merge_msec_value(conf->keepalive_timeout,",
           "    ngx_conf_merge_ptr_value(conf->response_phase, prev->response_phase, NULL);\n"
           "    ngx_conf_merge_size_value(conf->response_body_limit, prev->response_body_limit, 4096);\n"
           "    ngx_conf_merge_msec_value(conf->response_timeout, prev->response_timeout, 1500);\n"
           "    ngx_conf_merge_msec_value(conf->response_collect_timeout, prev->response_collect_timeout, 1200);\n")
    edit(path, "    return ngx_http_top_header_filter(r);",
         "    if (r->response_ctx) {\n"
         "        if (r->response_ctx->state != NGX_HTTP_RESPONSE_COMMITTING) { return NGX_ERROR; }\n"
         "        r->response_ctx->header_calls++;\n"
         '        ngx_http_response_trace(r, "header_chain_enter");\n'
         "    }\n    return ngx_http_top_header_filter(r);")
    path = ng/"src/http/ngx_http_upstream.c"
    edit(path, "    rc = ngx_http_send_header(r);\n",
         "    if (ngx_http_response_enabled(r)) {\n"
         "        ngx_http_response_start(r);\n        return;\n    }\n\n"
         "    rc = ngx_http_send_header(r);\n")
    insert(path, "    if (!u->header_sent\n",
           "    if (r->response_ctx && r->response_ctx->state == NGX_HTTP_RESPONSE_FINISHED) {\n"
           "        ngx_http_finalize_request(r, rc);\n        return;\n    }\n\n")
    path.write_text(path.read_text() + r'''
/* Research response-stage bridges: detach the peer without finalizing r. */
void
ngx_http_upstream_response_detach(ngx_http_request_t *r)
{
    ngx_http_upstream_t *u = r->upstream;
    ngx_connection_t *c = u->peer.connection;
    u->keepalive = 0;
    if (u->state) {
        if (u->state->response_time == (ngx_msec_t) -1) {
            u->state->response_time = ngx_current_msec - u->start_time;
        }
        if (c) { u->state->bytes_sent = c->sent; }
    }
    if (u->peer.free && u->peer.sockaddr) {
        u->peer.free(&u->peer, u->peer.data, 0);
        u->peer.sockaddr = NULL;
    }
    if (c) {
#if (NGX_HTTP_SSL)
        if (c->ssl) {
            c->ssl->no_wait_shutdown = 1;
            c->ssl->no_send_shutdown = 1;
            (void) ngx_ssl_shutdown(c);
        }
#endif
        if (c->pool) { ngx_destroy_pool(c->pool); }
        ngx_close_connection(c);
        u->peer.connection = NULL;
    }
    if (u->pipe) { u->pipe->upstream = NULL; }
}

void
ngx_http_upstream_response_finalize(ngx_http_request_t *r, ngx_int_t rc)
{
    ngx_http_upstream_finalize_request(r, r->upstream, rc);
}
''')
    edit(ng/"src/http/modules/ngx_http_proxy_module.c",
         "    u->input_filter_init = ngx_http_proxy_input_filter_init;",
         "    u->response_phase_supported = 1;\n    u->input_filter_init = ngx_http_proxy_input_filter_init;")

    path = lu/"src/ngx_http_lua_common.h"
    insert(path, "#define NGX_HTTP_LUA_CONTEXT_SET ", "#define NGX_HTTP_LUA_CONTEXT_RESPONSE           0x10000\n")
    edit(path, "uint16_t                 context;", "uint32_t                 context;")
    insert(path, "    ngx_http_handler_pt     content_handler;",
           "    ngx_http_handler_pt     response_handler;\n    u_char *response_chunkname;\n"
           "    ngx_http_complex_value_t response_src;\n    u_char *response_src_key;\n    int response_src_ref;\n")
    insert(path, "    unsigned         entered_content_phase:1;",
           "    unsigned         entered_response_phase:1;\n    unsigned response_done:1;\n    ngx_int_t response_rc;\n")
    for name in ("headerfilterby", "bodyfilterby"):
        path = lu/f"src/ngx_http_lua_{name}.c"
        edit(path, "uint16_t                     old_context;", "uint32_t                     old_context;")
    path = lu/"src/ngx_http_lua_util.h"
    insert(path, "/* key in Lua vm registry", "#define NGX_HTTP_LUA_CONTEXT_RESPONSE_YIELDABLE \\\n    (NGX_HTTP_LUA_CONTEXT_YIELDABLE | NGX_HTTP_LUA_CONTEXT_RESPONSE)\n\n")
    edit(path, ': "(unknown)")', ': (c) == NGX_HTTP_LUA_CONTEXT_RESPONSE ? "response_by_lua*" : "(unknown)")')
    path = lu/"src/ngx_http_lua_util.c"
    insert(path, '#include "ngx_http_lua_util.h"', '#include "ngx_http_lua_responseby.h"\n')
    edit(path, "    ctx->entered_content_phase = 0;",
         "    ctx->entered_content_phase = 0;\n    ctx->entered_response_phase = 0;\n    ctx->response_done = 0;")
    edit(path, "    ngx_http_lua_inject_req_api(log, L);",
         "    ngx_http_lua_inject_req_api(log, L);\n    ngx_http_lua_inject_response_api(L);")

    for name in ("socket_tcp", "sleep", "coroutine"):
        path = lu/f"src/ngx_http_lua_{name}.c"
        s = path.read_text().replace("NGX_HTTP_LUA_CONTEXT_YIELDABLE", "NGX_HTTP_LUA_CONTEXT_RESPONSE_YIELDABLE")
        if name in ("socket_tcp", "sleep"):
            s = '#include "ngx_http_lua_responseby.h"\n' + s
            # All API yield sites choose the response writer, never the request engine.
            s = s.replace("if (ctx->entered_content_phase) {\n        r->write_event_handler = ngx_http_lua_content_wev_handler;",
                "if (ctx->entered_response_phase) {\n        r->write_event_handler = ngx_http_lua_response_wev_handler;\n\n"
                "    } else if (ctx->entered_content_phase) {\n        r->write_event_handler = ngx_http_lua_content_wev_handler;")
            # Some API yield sites are indented by an extra level.
            s = s.replace("if (ctx->entered_content_phase) {\n            r->write_event_handler = ngx_http_lua_content_wev_handler;",
                "if (ctx->entered_response_phase) {\n            r->write_event_handler = ngx_http_lua_response_wev_handler;\n\n"
                "        } else if (ctx->entered_content_phase) {\n            r->write_event_handler = ngx_http_lua_content_wev_handler;")
            # Event callbacks that otherwise directly invoke the content resumer.
            s = re.sub(r"if \(ctx->entered_content_phase\) \{\n        \(void\) (ngx_http_lua_[a-z_]+)\(r\);",
                lambda m: "if (ctx->entered_response_phase) {\n        ctx->resume_handler = " + m[1] + ";\n"
                "        ngx_http_lua_response_wev_handler(r);\n\n    } else " + m[0], s)
            # Resume returns completion to the new phase's writer, not finalize().
            s = s.replace("    if (ctx->entered_content_phase) {\n        ngx_http_lua_finalize_request(r, rc);",
                "    if (ctx->entered_response_phase) { return rc; }\n\n"
                "    if (ctx->entered_content_phase) {\n        ngx_http_lua_finalize_request(r, rc);")
        path.write_text(s)

    path = root/"lua-resty-core-0.1.31/lib/resty/core/phase.lua"
    edit(path, '    [0x8000] = "server_rewrite",', '    [0x8000] = "server_rewrite",\n    [0x10000] = "response",')
    path = lu/"src/ngx_http_lua_module.c"
    insert(path, '#include "ngx_http_lua_util.h"', '#include "ngx_http_lua_responseby.h"\n')
    cmds = ""
    for kind, args, handler in (("block", "NGX_CONF_BLOCK|NGX_CONF_NOARGS", "inline"), ("file", "NGX_CONF_TAKE1", "file")):
        setter = "ngx_http_lua_response_by_lua_block" if kind == "block" else "ngx_http_lua_response_by_lua"
        cmds += (f'    {{ ngx_string("response_by_lua_{kind}"),\n'
            f'      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|{args},\n'
            f'      {setter}, NGX_HTTP_LOC_CONF_OFFSET, 0,\n'
            f'      (void *) ngx_http_lua_response_handler_{handler} }},\n\n')
    insert(path, '    /* content_by_lua "<inline script>" */', cmds)
    edit(path, "    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);\n",
         "    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);\n"
         "    h = ngx_array_push(&cmcf->phases[NGX_HTTP_RESPONSE_PHASE].handlers);\n"
         "    if (h == NULL) { return NGX_ERROR; }\n    *h = ngx_http_lua_response_handler;\n")
    edit(path, "    conf->content_src_ref = LUA_REFNIL;", "    conf->content_src_ref = LUA_REFNIL;\n    conf->response_src_ref = LUA_REFNIL;")
    insert(path, "    if (conf->content_src.value.len == 0) {",
           "    if (conf->response_src.value.len == 0) {\n        conf->response_src = prev->response_src;\n"
           "        conf->response_handler = prev->response_handler;\n        conf->response_src_ref = prev->response_src_ref;\n"
           "        conf->response_src_key = prev->response_src_key;\n        conf->response_chunkname = prev->response_chunkname;\n    }\n\n")
    # Reuse the version's existing directive parser and code cache loaders.
    path = lu/"src/ngx_http_lua_directive.c"
    s = path.read_text()
    block = s[s.index("char *\nngx_http_lua_content_by_lua_block("):s.index("char *\nngx_http_lua_log_by_lua_block(")]
    block = block[:block.index("    /*  register location content handler */")] + "    return NGX_CONF_OK;\n}\n\n"
    # Removing registration also leaves an unused clcf declaration.
    block = re.sub(r"    ngx_http_core_loc_conf_t\s+\*clcf;\n", "", block)
    block = block.replace("content_", "response_").replace('"content_by_lua"', '"response_by_lua"')
    path.write_text('#include "ngx_http_lua_responseby.h"\n' + s + "\n" + block)
    path = lu/"src/ngx_http_lua_contentby.c"
    s = path.read_text()
    loaders = s[s.index("ngx_int_t\nngx_http_lua_content_handler_file("):s.index("ngx_int_t\nngx_http_lua_content_run_posted_threads(")]
    path = lu/"src/ngx_http_lua_responseby.c"
    path.write_text(path.read_text() + "\n" + loaders.replace("content_", "response_"))
    edit(lu/"config", "$ngx_addon_dir/src/ngx_http_lua_contentby.c \\",
         "$ngx_addon_dir/src/ngx_http_lua_responseby.c \\\n            $ngx_addon_dir/src/ngx_http_lua_contentby.c \\")
    edit(lu/"config", "$ngx_addon_dir/src/ngx_http_lua_contentby.h \\",
         "$ngx_addon_dir/src/ngx_http_lua_responseby.h \\\n            $ngx_addon_dir/src/ngx_http_lua_contentby.h \\")
    print("Native response phase adapter applied; compile and runtime verification still required.")


if __name__ == "__main__":
    main()
