# Complete native source examples

These four C/H files are frozen copies of the newly added files from the tested,
API7-patched OpenResty tree. They are research artifacts, not APISIX modules to
install or compile in this repository.

| File | Tested-tree location |
| --- | --- |
| [ngx_http_response_phase.c](ngx_http_response_phase.c) | `bundle/nginx-1.27.1/src/http/ngx_http_response_phase.c` |
| [ngx_http_response_phase.h](ngx_http_response_phase.h) | `bundle/nginx-1.27.1/src/http/ngx_http_response_phase.h` |
| [ngx_http_lua_responseby.c](ngx_http_lua_responseby.c) | `bundle/ngx_lua-0.10.28/src/ngx_http_lua_responseby.c` |
| [ngx_http_lua_responseby.h](ngx_http_lua_responseby.h) | `bundle/ngx_lua-0.10.28/src/ngx_http_lua_responseby.h` |

The Lua source here includes the generated code-cache loaders adapted from
`ngx_http_lua_contentby.c` (OpenResty/ngx_lua, copyright Xiaozhe Wang and Yichun
Zhang). The corresponding adapter input in `../core/` deliberately leaves those
loaders to `adapt.py`. [Upstream notices](../UPSTREAM-NOTICES.md) retain the source
copyrights and terms; the original archives are pinned in `prepare.py`.

[SHA256SUMS](SHA256SUMS) records these exact files. The complete integration changes
to the existing Nginx/ngx_lua files are in [native-response.patch](../native-response.patch).
Use the [reproduction guide](../README.zh-CN.md) to rebuild the fixed source baseline.
