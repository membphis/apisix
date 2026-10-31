/* Technical prototype only. */
#ifndef _NGX_HTTP_LUA_RESPONSEBY_H_INCLUDED_
#define _NGX_HTTP_LUA_RESPONSEBY_H_INCLUDED_
#include "ngx_http_lua_common.h"

ngx_int_t ngx_http_lua_response_handler(ngx_http_request_t *r);
ngx_int_t ngx_http_lua_response_handler_inline(ngx_http_request_t *r);
ngx_int_t ngx_http_lua_response_handler_file(ngx_http_request_t *r);
ngx_int_t ngx_http_lua_response_by_chunk(lua_State *L, ngx_http_request_t *r);
void ngx_http_lua_response_wev_handler(ngx_http_request_t *r);
void ngx_http_lua_inject_response_api(lua_State *L);
char *ngx_http_lua_response_by_lua(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
char *ngx_http_lua_response_by_lua_block(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);

#endif
