/* SPDX-License-Identifier: Apache-2.0 */
#include "engine.h"

static void
ac_response_source(lua_State *L)
{
    ac_ngx(L, "ctx", "api_ctx");
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "_resp_source");
        if (lua_isnil(L, -1)) {
            lua_pushliteral(L, "apisix"); lua_setfield(L, -3, "_resp_source");
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
}

static int
ac_set_header(lua_State *L)
{
    int i, j, value, n = lua_gettop(L), append = lua_toboolean(L, lua_upvalueindex(1));
    if (n % 2) return luaL_error(L, "invalid argument count, expected header/value pairs");
    if (!append) {
        for (i = 1; i <= n; i += 2) {
            if (lua_type(L, i) != LUA_TSTRING || (!lua_isnil(L, i + 1) && !lua_isstring(L, i + 1))) break;
        }
        if (i > n) {
            ngx_http_request_t *request = ngx_http_lua_get_request(L);
            if (!request) return luaL_error(L, "no request found");
            for (i = 1; i <= n; i += 2) {
                ngx_str_t name, field = ngx_null_string;
                name.data = (u_char *) lua_tolstring(L, i, &name.len);
                if (!lua_isnil(L, i + 1)) {
                    lua_pushvalue(L, i + 1); field.data = (u_char *) lua_tolstring(L, -1, &field.len);
                }
                ac_native_header_set(L, request, name, field, 1, 0);
                if (!lua_isnil(L, i + 1)) lua_pop(L, 1);
            }
            return 0;
        }
    }
    ac_ngx(L, "header", NULL);
    for (i = 1; i <= n; i += 2) {
        lua_pushvalue(L, i);
        value = i + 1;
        if (append) {
            lua_gettable(L, n + 1);
            if (lua_isnil(L, -1)) lua_pop(L, 1);
            else {
                if (!lua_istable(L, -1)) {
                    lua_createtable(L, 2, 0);
                    lua_pushvalue(L, -2); lua_rawseti(L, -2, 1);
                    lua_remove(L, -2);
                }
                j = (int) lua_objlen(L, -1);
                lua_pushvalue(L, value); lua_rawseti(L, -2, j + 1);
                value = lua_gettop(L);
            }
            lua_pushvalue(L, i);
        }
        lua_pushvalue(L, value);
        lua_settable(L, n + 1);
        lua_settop(L, n + 1);
    }
    return 0;
}

static int
ac_exit_register(lua_State *L)
{
    int list, n;
    lua_settop(L, 2);
    ac_ngx(L, "ctx", NULL);
    lua_getfield(L, -1, "apisix_exit_callback_funcs");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1); lua_newtable(L);
        lua_pushvalue(L, -1); lua_setfield(L, -3, "apisix_exit_callback_funcs");
    }
    list = lua_gettop(L); n = (int) lua_objlen(L, list);
    lua_pushvalue(L, 1); lua_rawseti(L, list, n + 1);
    lua_pushvalue(L, 2); lua_rawseti(L, list, n + 2);
    return 0;
}

static int ac_exit_step(lua_State *L, int state);

static int
ac_exit_resume(lua_State *L)
{
    int state = lua_upvalueindex(1);
    if (!lua_toboolean(L, 1)) { lua_pushvalue(L, 2); return lua_error(L); }
    lua_settop(L, 4);
    lua_pushvalue(L, 2); lua_setfield(L, state, "code");
    lua_pushvalue(L, 3); lua_setfield(L, state, "body");
    lua_pushvalue(L, 4); lua_setfield(L, state, "headers");
    return ac_exit_step(L, state);
}

static int
ac_exit_finished(lua_State *L)
{
    if (!lua_toboolean(L, 1)) { lua_pushvalue(L, 2); return lua_error(L); }
    lua_pushnil(L);
    return 1;
}

static int
ac_exit_step(lua_State *L, int state)
{
    int list, index, args, headers, body;
    lua_Integer code;
    state = ac_abs(L, state);
    lua_getfield(L, state, "callbacks"); list = lua_gettop(L);
    if (lua_istable(L, list)) {
        lua_getfield(L, state, "index"); index = (int) lua_tointeger(L, -1); lua_pop(L, 1);
        if (index <= (int) lua_objlen(L, list)) {
            lua_pushinteger(L, index + 2); lua_setfield(L, state, "index");
            lua_pushvalue(L, state); lua_pushcclosure(L, ac_exit_resume, 1);
            lua_rawgeti(L, list, index);
            lua_createtable(L, 4, 1); args = lua_gettop(L);
            lua_getfield(L, state, "code"); lua_rawseti(L, args, 1);
            lua_getfield(L, state, "body"); lua_rawseti(L, args, 2);
            lua_getfield(L, state, "headers"); lua_rawseti(L, args, 3);
            lua_rawgeti(L, list, index + 1); lua_rawseti(L, args, 4);
            lua_pushinteger(L, 4); lua_setfield(L, args, "n");
            return 3;
        }
    }
    lua_pop(L, 1);
    lua_getfield(L, state, "code");
    code = lua_toboolean(L, -1) ? luaL_checkinteger(L, -1) : 0;
    lua_pop(L, 1);
    if (code) {
        lua_getglobal(L, "ngx"); lua_pushinteger(L, code); lua_setfield(L, -2, "status"); lua_pop(L, 1);
    }
    lua_getfield(L, state, "headers"); headers = lua_gettop(L);
    if (lua_istable(L, headers)) {
        ac_ngx(L, "header", NULL);
        lua_pushnil(L);
        while (lua_next(L, headers)) {
            lua_pushvalue(L, -2); lua_pushvalue(L, -2); lua_settable(L, headers + 1);
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    lua_getfield(L, state, "body"); body = lua_gettop(L);
    if (!lua_isnil(L, body)) {
        if (lua_istable(L, body)) {
            ac_require(L, "cjson.safe"); lua_getfield(L, -1, "encode");
            lua_pushvalue(L, body); lua_call(L, 1, 2);
            if (lua_isnil(L, -2)) return luaL_error(L, "failed to encode data: %s", lua_tostring(L, -1));
            lua_pop(L, 1);
            lua_pushliteral(L, "\n"); lua_concat(L, 2); lua_replace(L, body);
            lua_settop(L, body);
        }
        ac_ngx(L, NULL, "print"); lua_pushvalue(L, body); lua_call(L, 1, 0);
    }
    lua_getfield(L, state, "fragments");
    if (lua_istable(L, -1)) {
        ac_ngx(L, NULL, "print"); lua_pushvalue(L, -2); lua_call(L, 1, 0);
    }
    lua_pop(L, 1);
    if (!code) { lua_pushnil(L); return 1; }
    ac_response_source(L);
    lua_pushcfunction(L, ac_exit_finished);
    ac_ngx(L, NULL, "exit");
    lua_createtable(L, 1, 1);
    lua_pushinteger(L, code); lua_rawseti(L, -2, 1);
    lua_pushinteger(L, 1); lua_setfield(L, -2, "n");
    return 3;
}

static int
ac_exit_start(lua_State *L)
{
    int i, n = lua_gettop(L), state, fragments, callbacks, body = 0, offset = 2, count = 0;
    lua_newtable(L); state = lua_gettop(L);
    if (lua_type(L, 1) == LUA_TNUMBER) {
        lua_pushvalue(L, 1); lua_setfield(L, state, "code");
    } else if (lua_toboolean(L, 1)) offset = 1;
    ac_ngx(L, "ctx", "apisix_exit_callback_funcs"); callbacks = lua_gettop(L);
    if (lua_istable(L, callbacks)) {
        lua_pushvalue(L, callbacks); lua_setfield(L, state, "callbacks");
        lua_pushinteger(L, 1); lua_setfield(L, state, "index");
        for (i = 2; i <= n; i++) if (!lua_isnil(L, i)) { body = i; break; }
        if (!body && offset == 1) body = 1;
        if (body) { lua_pushvalue(L, body); lua_setfield(L, state, "body"); }
        lua_newtable(L); lua_setfield(L, state, "headers");
    } else {
        lua_newtable(L); fragments = lua_gettop(L);
        for (i = offset; i <= n; i++) {
            if (lua_istable(L, i)) {
                ac_require(L, "cjson.safe"); lua_getfield(L, -1, "encode");
                lua_pushvalue(L, i); lua_call(L, 1, 2);
                if (lua_isnil(L, -2)) return luaL_error(L, "failed to encode data: %s", lua_tostring(L, -1));
                lua_pop(L, 1); lua_rawseti(L, fragments, ++count); lua_pop(L, 1);
                lua_pushliteral(L, "\n"); lua_rawseti(L, fragments, ++count);
            } else if (!lua_isnil(L, i)) {
                lua_pushvalue(L, i); lua_rawseti(L, fragments, ++count);
            }
        }
        if (count) { lua_pushvalue(L, fragments); lua_setfield(L, state, "fragments"); }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    return ac_exit_step(L, state);
}

static int
ac_source_set(lua_State *L)
{
    if (lua_istable(L, 1)) { lua_pushvalue(L, 2); lua_setfield(L, 1, "_resp_source"); }
    return 0;
}

static int
ac_last_token(lua_State *L)
{
    size_t len, end, start;
    const u_char *text;
    if (lua_isnoneornil(L, 1)) return 0;
    text = (u_char *) luaL_checklstring(L, 1, &len);
    for (end = len; end && (text[end - 1] == ',' || isspace(text[end - 1])); end--) { }
    if (!end) return 0;
    for (start = end; start && text[start - 1] != ',' && !isspace(text[start - 1]); start--) { }
    lua_pushlstring(L, (char *) text + start, end - start);
    return 1;
}

static int
ac_source_get(lua_State *L)
{
    const char *source = "apisix";
    lua_settop(L, 1);
    if (!lua_istable(L, 1)) { lua_pushstring(L, source); return 1; }
    lua_getfield(L, 1, "_resp_source");
    if (lua_toboolean(L, -1)) return 1;
    lua_pop(L, 1);
    if (ac_bool_field(L, 1, "_apisix_proxied")) {
        ac_ngx(L, "var", "upstream_header_time");
        lua_pushcfunction(L, ac_last_token); lua_pushvalue(L, -2); lua_call(L, 1, 1);
        source = lua_isnil(L, -1) || ngx_strcmp(lua_tostring(L, -1), "-") == 0 ? "nginx" : "upstream";
        lua_pushstring(L, source); lua_setfield(L, 1, "_resp_source");
    }
    lua_pushstring(L, source);
    return 1;
}

static int
ac_upstream_status(lua_State *L)
{
    size_t len;
    const char *value;
    lua_getfield(L, 1, "var"); lua_getfield(L, -1, "upstream_status");
    value = lua_tolstring(L, -1, &len);
    if (!value) return 0;
    lua_pushlstring(L, value + (len > 3 ? len - 3 : 0), len > 3 ? 3 : len);
    if (lua_isnumber(L, -1)) { lua_pushnumber(L, lua_tonumber(L, -1)); return 1; }
    return 0;
}

static int
ac_clear_body_headers(lua_State *L)
{
    static const char *headers[] = {"content_length", "content_encoding", "last_modified", "etag", NULL};
    int i;
    ac_ngx(L, "header", NULL);
    for (i = 0; headers[i]; i++) { lua_pushnil(L); lua_setfield(L, -2, headers[i]); }
    return 0;
}

static int
ac_hold_body(lua_State *L)
{
    int buffers, key, chunk, eof, buffer, n, args;
    size_t length, bytes;
    lua_Number maximum;
    lua_settop(L, 4);
    maximum = lua_isnil(L, 3) ? -1 : luaL_checknumber(L, 3);
    lua_getfield(L, 1, "_body_buffer");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1); lua_newtable(L); lua_pushvalue(L, -1); lua_setfield(L, 1, "_body_buffer");
    }
    buffers = lua_gettop(L);
    if (lua_isnil(L, 4)) lua_getfield(L, 1, "_plugin_name"); else lua_pushvalue(L, 4);
    key = lua_gettop(L);
    ac_ngx(L, "arg", NULL); args = lua_gettop(L);
    lua_rawgeti(L, args, 1); chunk = lua_gettop(L);
    lua_rawgeti(L, args, 2); eof = lua_toboolean(L, -1); lua_pop(L, 1);
    lua_pushvalue(L, key); lua_rawget(L, buffers); buffer = lua_gettop(L);
    if (lua_type(L, chunk) == LUA_TSTRING && lua_objlen(L, chunk)) {
        if (lua_istable(L, buffer) && ac_bool_field(L, buffer, "done")) return 0;
        if (lua_isnil(L, buffer)) {
            lua_pop(L, 1); lua_newtable(L);
            lua_pushvalue(L, key); lua_pushvalue(L, buffer); lua_rawset(L, buffers);
            n = 1; bytes = lua_objlen(L, chunk);
        } else {
            lua_getfield(L, buffer, "n"); n = (int) lua_tointeger(L, -1) + 1; lua_pop(L, 1);
            lua_getfield(L, buffer, "bytes"); bytes = (size_t) lua_tonumber(L, -1) + lua_objlen(L, chunk); lua_pop(L, 1);
        }
        lua_pushvalue(L, chunk); lua_rawseti(L, buffer, n);
        lua_pushinteger(L, n); lua_setfield(L, buffer, "n");
        lua_pushnumber(L, (lua_Number) bytes); lua_setfield(L, buffer, "bytes");
        if (maximum >= 0 && (lua_Number) bytes >= maximum) {
            lua_getglobal(L, "table"); lua_getfield(L, -1, "concat");
            lua_pushvalue(L, buffer); lua_pushliteral(L, ""); lua_pushinteger(L, 1); lua_pushinteger(L, n);
            lua_call(L, 4, 1);
            const char *text = lua_tolstring(L, -1, &length);
            lua_pushlstring(L, text, (size_t) maximum < length ? (size_t) maximum : length);
            lua_pushboolean(L, 1); lua_setfield(L, buffer, "done");
            return 1;
        }
    }
    if (eof) {
        if (lua_isnil(L, buffer)) {
            if (maximum >= 0 && lua_isstring(L, chunk) && lua_objlen(L, chunk) >= (size_t) maximum) {
                const char *text = lua_tolstring(L, chunk, &length);
                lua_pushlstring(L, text, (size_t) maximum); return 1;
            }
            lua_pushvalue(L, chunk); return 1;
        }
        if (ac_bool_field(L, buffer, "done")) return 0;
        lua_getglobal(L, "table"); lua_getfield(L, -1, "concat");
        lua_pushvalue(L, buffer); lua_pushliteral(L, ""); lua_pushinteger(L, 1); lua_getfield(L, buffer, "n");
        lua_call(L, 4, 1);
        lua_pushvalue(L, key); lua_pushnil(L); lua_rawset(L, buffers);
        return 1;
    }
    if (!lua_toboolean(L, 2)) { lua_pushnil(L); lua_rawseti(L, args, 1); }
    return 0;
}

static int
ac_response_open(lua_State *L)
{
    static const luaL_Reg functions[] = {
        {"exit_insert_callback", ac_exit_register}, {"get_response_source", ac_source_get},
        {"set_response_source", ac_source_set}, {"get_last_upstream_token", ac_last_token},
        {"get_upstream_status", ac_upstream_status}, {"clear_header_as_body_modified", ac_clear_body_headers},
        {"hold_body_chunk", ac_hold_body}, {NULL, NULL}
    };
    lua_newtable(L); luaL_register(L, NULL, functions);
    ac_bind(L, ac_exit_start, 0); lua_setfield(L, -2, "exit");
    ac_ngx(L, NULL, "say"); lua_setfield(L, -2, "say");
    lua_pushboolean(L, 0); lua_pushcclosure(L, ac_set_header, 1); lua_setfield(L, -2, "set_header");
    lua_pushboolean(L, 1); lua_pushcclosure(L, ac_set_header, 1); lua_setfield(L, -2, "add_header");
    return 1;
}

static int
ac_core_index(lua_State *L)
{
    static struct { const char *field; const char *module; } members[] = {
        {"version", "apisix.core.version"}, {"log", "apisix.core.log"}, {"config", "apisix.core.config"},
        {"config_util", "apisix.core.config_util"}, {"json", "apisix.core.json"}, {"table", "apisix.core.table"},
        {"request", "apisix.core.request"}, {"response", "apisix.core.response"}, {"lrucache", "apisix.core.lrucache"},
        {"schema", "apisix.core.schema"}, {"string", "apisix.core.string"}, {"ctx", "apisix.core.ctx"},
        {"timer", "apisix.core.timer"}, {"id", "apisix.core.id"}, {"ip", "apisix.core.ip"},
        {"io", "apisix.core.io"}, {"utils", "apisix.core.utils"}, {"dns_client", "apisix.core.dns.client"},
        {"etcd", "apisix.core.etcd"}, {"tablepool", "tablepool"}, {"resolver", "apisix.core.resolver"},
        {"os", "apisix.core.os"}, {"pubsub", "apisix.core.pubsub"}, {"math", "apisix.core.math"},
        {"event", "apisix.core.event"}, {"env", "apisix.core.env"},
        {"data_encryption", "apisix.core.data_encryption"}, {"websocket", "apisix.core.websocket"}, {NULL, NULL}
    };
    const char *field = luaL_checkstring(L, 2);
    int i;
    if (ngx_strcmp(field, "sleep") == 0) {
        ac_require(L, "apisix.core.utils"); lua_getfield(L, -1, "sleep");
    } else {
        for (i = 0; members[i].field; i++) if (ngx_strcmp(field, members[i].field) == 0) break;
        if (!members[i].field) return 0;
        ac_require(L, members[i].module);
    }
    lua_pushvalue(L, 2); lua_pushvalue(L, -2); lua_rawset(L, 1);
    return 1;
}

static int
ac_core_open(lua_State *L)
{
    lua_newtable(L); lua_newtable(L);
    lua_pushcfunction(L, ac_core_index); lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2);
    return 1;
}

static int
ac_version_open(lua_State *L)
{
    lua_newtable(L); lua_pushliteral(L, "3.19.0"); lua_setfield(L, -2, "VERSION"); return 1;
}

ngx_int_t
ac_response_register(ngx_conf_t *cf)
{
    if (ngx_http_lua_add_package_preload(cf, "apisix.core.response", ac_response_open) != NGX_OK
        || ngx_http_lua_add_package_preload(cf, "apisix.core", ac_core_open) != NGX_OK
        || ngx_http_lua_add_package_preload(cf, "apisix.core.version", ac_version_open) != NGX_OK) return NGX_ERROR;
    return ac_consumer_register(cf);
}
