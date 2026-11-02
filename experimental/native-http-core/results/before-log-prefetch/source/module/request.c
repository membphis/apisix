/* SPDX-License-Identifier: Apache-2.0 */
#include "engine.h"
#include <stdio.h>
#include <stdlib.h>

/* Existing exported runtime/ngxvar primitives, also used by resty.ngxvar.
 * Calling them here preserves typed values and timing aggregation without
 * re-entering Lua to make the same C calls through FFI. */
extern int ngx_http_lua_ffi_get_resp_status(ngx_http_request_t *r);
extern int ngx_http_lua_var_ffi_uri(ngx_http_request_t *r, ngx_str_t *value);
extern int ngx_http_lua_var_ffi_host(ngx_http_request_t *r, ngx_str_t *value);
extern int ngx_http_lua_var_ffi_remote_addr(ngx_http_request_t *r, ngx_str_t *value);
extern int ngx_http_lua_var_ffi_scheme(ngx_http_request_t *r, ngx_str_t *value);
extern int ngx_http_lua_var_ffi_request_time(ngx_http_request_t *r, u_char *buf);
extern int ngx_http_lua_var_ffi_upstream_response_time(ngx_http_request_t *r,
    u_char *buf, int type);
extern int ngx_http_lua_ffi_req_set_header(ngx_http_request_t *r,
    const u_char *key, size_t key_len, const u_char *value, size_t value_len,
    void *mvals, size_t mvals_len, int override, char **error);
extern int ngx_http_lua_ffi_set_resp_header(ngx_http_request_t *r,
    const char *key, size_t key_len, int is_nil, const char *value, size_t value_len,
    void *mvals, size_t mvals_len, int override, char **error);

ngx_int_t
ac_native_header_write(ngx_http_request_t *r, ngx_str_t name,
    ngx_str_t value, ngx_flag_t response, ngx_flag_t append, char **error)
{
    int status;
    if (append && !value.data) { *error = "invalid header value"; return NGX_ERROR; }
    if (response) {
        status = ngx_http_lua_ffi_set_resp_header(r, (char *) name.data, name.len,
                     !value.data, (char *) value.data, value.len, NULL, 0, !append, error);
    } else {
        status = ngx_http_lua_ffi_req_set_header(r, name.data, name.len,
                     value.data, value.len, NULL, 0, !append, error);
    }
    return status == NGX_OK || status == NGX_DECLINED ? NGX_OK : NGX_ERROR;
}

void
ac_native_header_set(lua_State *L, ngx_http_request_t *r, ngx_str_t name,
    ngx_str_t value, ngx_flag_t response, ngx_flag_t append)
{
    char *error = NULL;
    if (ac_native_header_write(r, name, value, response, append, &error) != NGX_OK)
        luaL_error(L, "native header update failed: %s", error ? error : "invalid request context");
}

static void
ac_native_var_fetch(lua_State *L, ngx_http_request_t *r, const char *key,
    size_t length)
{
    ngx_str_t value = ngx_null_string, name;
    ngx_http_variable_value_t *variable;
    off_t number;
    u_char buffer[1025], lower[128], *data;
    int count, timing;
    size_t i;
    if (ngx_strcmp(key, "status") == 0) {
        lua_pushinteger(L, ngx_http_lua_ffi_get_resp_status(r)); return;
    }
    if (ngx_strcmp(key, "request_method") == 0 || ngx_strcmp(key, "method") == 0) {
        lua_pushlstring(L, (char *) r->method_name.data, r->method_name.len); return;
    }
    if (ngx_strcmp(key, "uri") == 0) ngx_http_lua_var_ffi_uri(r, &value);
    else if (ngx_strcmp(key, "host") == 0) ngx_http_lua_var_ffi_host(r, &value);
    else if (ngx_strcmp(key, "remote_addr") == 0) ngx_http_lua_var_ffi_remote_addr(r, &value);
    else if (ngx_strcmp(key, "scheme") == 0) ngx_http_lua_var_ffi_scheme(r, &value);
    else {
        timing = ngx_strcmp(key, "upstream_response_time") == 0 ? 0
            : ngx_strcmp(key, "upstream_header_time") == 0 ? 1
            : ngx_strcmp(key, "upstream_connect_time") == 0 ? 2 : -1;
        if (timing >= 0 || ngx_strcmp(key, "request_time") == 0) {
            count = timing < 0 ? ngx_http_lua_var_ffi_request_time(r, buffer)
                              : ngx_http_lua_var_ffi_upstream_response_time(r, buffer, timing);
            if (count < 0) lua_pushnil(L);
            else if (count == 0) lua_pushnumber(L, 0);
            else {
                if ((size_t) count >= sizeof(buffer)) { luaL_error(L, "native timing result too long"); return; }
                buffer[count] = '\0'; lua_pushnumber(L, strtod((char *) buffer, NULL));
            }
            return;
        }
        data = length <= sizeof(lower) ? lower : ngx_pnalloc(r->pool, length);
        if (!data) { luaL_error(L, "variable key allocation failed"); return; }
        for (i = 0; i < length; i++) {
            data[i] = ngx_tolower(key[i]);
            if (ngx_strncmp(key, "http_", 5) == 0 && data[i] == '-') data[i] = '_';
        }
        name.data = data; name.len = length;
        variable = ngx_http_get_variable(r, &name, ngx_hash_key(name.data, name.len));
        if (variable && !variable->not_found) {
            value.data = variable->data; value.len = variable->len;
        }
        if (ngx_strcmp(key, "request_length") == 0 || ngx_strcmp(key, "bytes_sent") == 0) {
            number = value.data ? ngx_atoof(value.data, value.len) : NGX_ERROR;
            if (number == NGX_ERROR) lua_pushnil(L); else lua_pushnumber(L, (lua_Number) number);
            return;
        }
    }
    if (value.data) lua_pushlstring(L, (char *) value.data, value.len);
    else lua_pushnil(L);
}

void
ac_ngx(lua_State *L, const char *table, const char *field)
{
    lua_getglobal(L, "ngx");
    if (table) {
        lua_getfield(L, -1, table);
        lua_remove(L, -2);
    }
    if (field) {
        lua_getfield(L, -1, field);
        lua_remove(L, -2);
    }
}

static int
ac_arg_context(lua_State *L, int index)
{
    if (lua_isnoneornil(L, index)) ac_push(L, ac_request(L)->table_ref);
    else lua_pushvalue(L, index);
    luaL_checktype(L, -1, LUA_TTABLE);
    return lua_gettop(L);
}

static int
ac_done(lua_State *L, int count)
{
    lua_pushnil(L);
    lua_insert(L, lua_gettop(L) - count);
    return count + 1;
}

static int
ac_io_call(lua_State *L, int state, lua_CFunction resume, int callable, int args, int count)
{
    int i;
    state = ac_abs(L, state);
    callable = ac_abs(L, callable);
    args = ac_abs(L, args);
    lua_pushvalue(L, state);
    lua_pushcclosure(L, resume, 1);
    lua_pushvalue(L, callable);
    lua_createtable(L, count, 1);
    for (i = 0; i < count; i++) {
        lua_pushvalue(L, args + i);
        lua_rawseti(L, -2, i + 1);
    }
    lua_pushinteger(L, count);
    lua_setfield(L, -2, "n");
    return 3;
}

static int
ac_headers(lua_State *L)
{
    int ctx;
    lua_settop(L, 1);
    ctx = ac_arg_context(L, 1);
    ac_require(L, "resty.apisix.request");
    lua_getfield(L, -1, "is_request_header_set");
    lua_call(L, 0, 1);
    if (lua_toboolean(L, -1)) {
        lua_pop(L, 1);
        lua_getfield(L, -1, "clear_request_header");
        lua_call(L, 0, 0);
        lua_pushnil(L);
        lua_setfield(L, ctx, "headers");
    } else lua_pop(L, 1);
    lua_pop(L, 1);
    lua_getfield(L, ctx, "headers");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        ac_ngx(L, "req", "get_headers");
        lua_call(L, 0, 1);
        lua_pushvalue(L, -1);
        lua_setfield(L, ctx, "headers");
    }
    return 1;
}

static int
ac_header(lua_State *L)
{
    lua_settop(L, 2);
    lua_pushcfunction(L, ac_headers);
    lua_pushvalue(L, 1);
    lua_call(L, 1, 1);
    lua_pushvalue(L, 2);
    lua_gettable(L, -2);
    if (lua_istable(L, -1)) lua_rawgeti(L, -1, 1);
    return 1;
}

static int
ac_modify_header(lua_State *L)
{
    int ctx, name, value, append = lua_toboolean(L, lua_upvalueindex(1));
    int cache, changed;
    size_t length, i;
    const u_char *raw;
    u_char *lower, var_buffer[128], *var_key;
    ngx_http_request_t *r = ngx_http_lua_get_request(L);
    lua_settop(L, 3);
    if (lua_type(L, 1) == LUA_TSTRING) {
        name = 1; value = 2;
        ac_push(L, ac_request(L)->table_ref);
        ctx = lua_gettop(L);
    } else {
        ctx = ac_arg_context(L, 1);
        name = 2; value = 3;
    }
    if (lua_type(L, name) != LUA_TSTRING) return luaL_error(L, "invalid header name: expected string");
    raw = (u_char *) lua_tolstring(L, name, &length);
    lower = ngx_pnalloc(r->pool, length + 6);
    if (!lower) return luaL_error(L, "header cache allocation failed");
    ngx_memcpy(lower, "http_", 5);
    for (i = 0; i < length; i++) lower[5 + i] = ngx_tolower(raw[i]);
    ac_require(L, "resty.apisix.request");
    lua_getfield(L, -1, "is_request_header_set"); lua_call(L, 0, 1);
    changed = lua_toboolean(L, -1); lua_pop(L, 2);
    if (lua_isnil(L, value) || lua_isstring(L, value)) {
        ngx_str_t header, field = ngx_null_string;
        header.data = (u_char *) raw; header.len = length;
        if (!lua_isnil(L, value)) {
            lua_pushvalue(L, value); field.data = (u_char *) lua_tolstring(L, -1, &field.len);
        }
        ac_native_header_set(L, r, header, field, 0, append);
        if (!lua_isnil(L, value)) lua_pop(L, 1);
    } else if (append) {
        ac_require(L, "ngx.req");
        lua_getfield(L, -1, "add_header");
        lua_remove(L, -2);
        lua_pushvalue(L, name); lua_pushvalue(L, value); lua_call(L, 2, 0);
    } else {
        ac_ngx(L, "req", "set_header");
        lua_pushvalue(L, name); lua_pushvalue(L, value); lua_call(L, 2, 0);
    }
    lua_getfield(L, ctx, "var");
    if (lua_istable(L, -1)) {
        var_key = length + 5 <= sizeof(var_buffer) ? var_buffer : ngx_pnalloc(r->pool, length + 5);
        if (!var_key) return luaL_error(L, "header variable allocation failed");
        ngx_memcpy(var_key, lower, length + 5);
        for (i = 5; i < length + 5; i++) if (var_key[i] == '-') var_key[i] = '_';
        lua_pushlstring(L, (char *) var_key, length + 5);
        lua_pushnil(L);
        lua_settable(L, -3);
    }
    lua_pop(L, 1);
    if (!changed) {
        ac_require(L, "resty.apisix.request");
        lua_getfield(L, -1, "clear_request_header"); lua_call(L, 0, 0); lua_pop(L, 1);
        lua_getfield(L, ctx, "headers"); cache = lua_gettop(L);
        if (lua_istable(L, cache)) {
            lua_pushlstring(L, (char *) lower + 5, length); lua_rawget(L, cache);
            if (!append || !lua_toboolean(L, -1)) {
                lua_pushlstring(L, (char *) lower + 5, length); lua_pushvalue(L, value); lua_rawset(L, cache);
            } else if (lua_istable(L, -1)) {
                lua_pushvalue(L, value); lua_rawseti(L, -2, (int) lua_objlen(L, -2) + 1);
            } else {
                lua_newtable(L); lua_pushvalue(L, -2); lua_rawseti(L, -2, 1);
                lua_pushvalue(L, value); lua_rawseti(L, -2, 2);
                lua_pushlstring(L, (char *) lower + 5, length); lua_pushvalue(L, -2); lua_rawset(L, cache);
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
    return 0;
}

static int
ac_uri_args(lua_State *L)
{
    int ctx;
    lua_settop(L, 1);
    ctx = ac_arg_context(L, 1);
    lua_getfield(L, ctx, "req_uri_args");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        ac_ngx(L, "req", "get_uri_args");
        lua_pushinteger(L, 0);
        lua_call(L, 1, 1);
        lua_pushvalue(L, -1);
        lua_setfield(L, ctx, "req_uri_args");
    }
    return 1;
}

static int
ac_set_uri_args(lua_State *L)
{
    int ctx;
    lua_settop(L, 2);
    ctx = ac_arg_context(L, 1);
    lua_pushnil(L);
    lua_setfield(L, ctx, "req_uri_args");
    ac_ngx(L, "req", "set_uri_args");
    lua_pushvalue(L, 2);
    lua_call(L, 1, LUA_MULTRET);
    return lua_gettop(L) - ctx;
}

static int
ac_body_too_large(lua_State *L, lua_Number size, lua_Number max)
{
    if (max < 0 || size <= max) return 0;
    lua_pushnil(L);
    lua_pushfstring(L, "request size %f is greater than the maximum size %f allowed", size, max);
    lua_pushliteral(L, "too_large");
    return 3;
}

static int
ac_body_read(lua_State *L, lua_Number max)
{
    int result, err;
    const char *path;
    size_t length;
    FILE *file;
    ngx_file_info_t info;
    char *buffer;
    ac_ngx(L, "req", "get_body_data");
    lua_call(L, 0, 1);
    if (!lua_isnil(L, -1)) {
        lua_tolstring(L, -1, &length);
        result = ac_body_too_large(L, (lua_Number) length, max);
        return result ? result : 1;
    }
    lua_pop(L, 1);
    ac_ngx(L, "req", "get_body_file");
    lua_call(L, 0, 1);
    if (lua_isnil(L, -1)) return 1;
    path = luaL_checkstring(L, -1);
    if (ngx_file_info(path, &info) == NGX_FILE_ERROR) {
        lua_pushnil(L);
        lua_pushstring(L, strerror(errno));
        return 2;
    }
    length = (size_t) ngx_file_size(&info);
    result = ac_body_too_large(L, (lua_Number) length, max);
    if (result) return result;
    file = fopen(path, "rb");
    if (!file) {
        lua_pushnil(L);
        lua_pushstring(L, strerror(errno));
        return 2;
    }
    buffer = ngx_pnalloc(ngx_http_lua_get_request(L)->pool, length ? length : 1);
    if (!buffer) {
        fclose(file);
        return luaL_error(L, "request body allocation failed");
    }
    if (fread(buffer, 1, length, file) != length) {
        err = errno;
        fclose(file);
        lua_pushnil(L);
        lua_pushstring(L, strerror(err));
        return 2;
    }
    fclose(file);
    lua_pushlstring(L, buffer, length);
    lua_pushnil(L);
    return 2;
}

static int
ac_body_finish(lua_State *L)
{
    int state = lua_upvalueindex(1), mode, result, ctx, body, saved;
    lua_Number max;
    const char *ct;
    if (!lua_toboolean(L, 1)) {
        lua_pushvalue(L, 2);
        return lua_error(L);
    }
    lua_getfield(L, state, "mode");
    mode = (int) lua_tointeger(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, state, "max");
    max = lua_isnil(L, -1) ? -1 : lua_tonumber(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, state, "ctx");
    ctx = lua_gettop(L);
    if (mode == 1) {
        ac_ngx(L, "req", "get_post_args");
        lua_pushinteger(L, 0);
        lua_call(L, 1, 2);
        if (lua_isnil(L, -2)) {
            lua_pop(L, 2);
            lua_newtable(L);
        } else lua_pop(L, 1);
        lua_pushvalue(L, -1);
        lua_setfield(L, ctx, "req_post_args");
        return ac_done(L, 1);
    }
    saved = lua_gettop(L);
    result = ac_body_read(L, max);
    if (mode == 0) return ac_done(L, result);
    body = lua_gettop(L) - result + 1;
    if (lua_isnil(L, body)) {
        lua_pushnil(L);
        lua_pushfstring(L, "could not get body: %s", result >= 2 ? lua_tostring(L, body + 1)
                                                                      : "request body is empty");
        return ac_done(L, 2);
    }
    lua_getfield(L, state, "ct");
    ct = lua_tostring(L, -1);
    if (ngx_strstr(ct, "application/json")) {
        ac_require(L, "cjson.safe");
        lua_getfield(L, -1, "decode");
        lua_pushvalue(L, body);
        lua_call(L, 1, 2);
        if (!lua_toboolean(L, -2)) {
            lua_pushnil(L);
            lua_pushfstring(L, "could not parse JSON request body: %s",
                           lua_tostring(L, -2) ? lua_tostring(L, -2) : "invalid JSON");
            result = 2;
        } else {
            lua_pop(L, 1);
            lua_pushvalue(L, -1);
            lua_setfield(L, ctx, "_request_body_table");
            lua_pushliteral(L, "application/json");
            lua_setfield(L, ctx, "_request_body_type");
            result = 1;
        }
    } else if (ngx_strstr(ct, "application/x-www-form-urlencoded")) {
        lua_settop(L, saved);
        ac_ngx(L, "req", "get_post_args");
        lua_pushinteger(L, 0);
        lua_call(L, 1, 1);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            lua_newtable(L);
        }
        lua_pushvalue(L, -1);
        lua_setfield(L, ctx, "req_post_args");
        lua_pushvalue(L, -1);
        lua_setfield(L, ctx, "_request_body_table");
        lua_pushliteral(L, "application/x-www-form-urlencoded");
        lua_setfield(L, ctx, "_request_body_type");
        result = 1;
    } else {
        ac_require(L, "multipart");
        lua_pushvalue(L, body);
        lua_pushstring(L, ct);
        lua_call(L, 2, 1);
        if (!lua_toboolean(L, -1)) {
            lua_pushnil(L);
            lua_pushliteral(L, "failed to parse multipart form data");
            result = 2;
        } else {
            lua_getfield(L, -1, "get_all");
            lua_pushvalue(L, -2);
            lua_call(L, 1, 1);
            lua_pushvalue(L, -1);
            lua_setfield(L, ctx, "_request_body_table");
            lua_pushliteral(L, "multipart/form-data");
            lua_setfield(L, ctx, "_request_body_type");
            result = 1;
        }
    }
    if (mode == 3 && result > 1) {
        lua_newtable(L);
        lua_pushvalue(L, -2);
        lua_setfield(L, -2, "message");
        lua_replace(L, -2);
    }
    return ac_done(L, result);
}

static int
ac_body_start(lua_State *L)
{
    int mode = (int) lua_tointeger(L, lua_upvalueindex(1));
    int ctx, state, maxarg, ctarg, result;
    lua_Number max;
    const char *ct;
    lua_settop(L, 3);
    ctx = ac_arg_context(L, mode == 0 ? 2 : mode == 3 ? 4 : 1);
    maxarg = mode == 0 || mode == 3 ? 1 : 3;
    max = lua_isnil(L, maxarg) ? -1 : luaL_checknumber(L, maxarg);
    if (mode == 1) {
        lua_getfield(L, ctx, "req_post_args");
        if (!lua_isnil(L, -1)) return ac_done(L, 1);
        lua_pop(L, 1);
    }
    lua_newtable(L);
    state = lua_gettop(L);
    lua_pushvalue(L, ctx);
    lua_setfield(L, state, "ctx");
    lua_pushinteger(L, mode);
    lua_setfield(L, state, "mode");
    lua_pushnumber(L, max);
    lua_setfield(L, state, "max");
    if (mode >= 2) {
        if (mode == 3) lua_pushliteral(L, "application/json");
        else lua_pushvalue(L, 2);
        ctarg = lua_gettop(L);
        lua_getfield(L, ctx, "_request_body_table");
        if (!lua_isnil(L, -1)) {
            if (!lua_isnil(L, ctarg)) {
                lua_getfield(L, ctx, "_request_body_type");
                if (!lua_equal(L, -1, ctarg)) {
                    lua_pushnil(L);
                    lua_pushfstring(L, "request body type mismatch: cached type is %s, expected %s",
                        lua_tostring(L, -2) ? lua_tostring(L, -2) : "unknown", lua_tostring(L, ctarg));
                    return ac_done(L, 2);
                }
                lua_pop(L, 1);
            }
            return ac_done(L, 1);
        }
        lua_pop(L, 1);
        if (lua_isnil(L, ctarg)) {
            lua_pushcfunction(L, ac_header);
            lua_pushvalue(L, ctx);
            lua_pushliteral(L, "Content-Type");
            lua_call(L, 2, 1);
            lua_replace(L, ctarg);
        }
        ct = lua_tostring(L, ctarg);
        if (!ct) ct = "";
        if (!ngx_strstr(ct, "application/json") && !ngx_strstr(ct, "application/x-www-form-urlencoded")
            && !ngx_strstr(ct, "multipart/form-data")) {
            lua_pushnil(L);
            lua_pushfstring(L, "unsupported content-type: %s, supported types are: application/json, application/x-www-form-urlencoded, multipart/form-data", ct);
            return ac_done(L, 2);
        }
        lua_pushstring(L, ct);
        lua_setfield(L, state, "ct");
    }
    if (mode != 1 && max >= 0) {
        lua_getfield(L, ctx, "var");
        lua_getfield(L, -1, "http_content_length");
        if (lua_isnumber(L, -1) && lua_tonumber(L, -1) > max) {
            lua_Number size = lua_tonumber(L, -1);
            ac_ngx(L, "req", "clear_header");
            lua_pushliteral(L, "expect");
            lua_call(L, 1, 0);
            result = ac_body_too_large(L, size, max);
            return ac_done(L, result);
        }
        lua_pop(L, 2);
    }
    ac_ngx(L, "req", "read_body");
    return ac_io_call(L, state, ac_body_finish, -1, -1, 0);
}

static int
ac_request_var(lua_State *L)
{
    const char *name = lua_tostring(L, lua_upvalueindex(1));
    int numeric = lua_toboolean(L, lua_upvalueindex(2));
    int ctx;
    lua_settop(L, 1);
    ctx = ac_arg_context(L, 1);
    lua_getfield(L, ctx, "var");
    if (ngx_strcmp(name, "realip_remote_addr") == 0) {
        lua_getfield(L, -1, name);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            name = "remote_addr";
            lua_getfield(L, -1, name);
        }
    } else lua_getfield(L, -1, name);
    if (numeric) {
        if (lua_isnumber(L, -1)) lua_pushnumber(L, lua_tonumber(L, -1));
        else lua_pushnil(L);
    } else if (lua_isnil(L, -1)) lua_pushliteral(L, "");
    return 1;
}

static ngx_flag_t
ac_name(const char *key, const char *prefix)
{
    return ngx_strncmp(key, prefix, ngx_strlen(prefix)) == 0;
}

static void
ac_var_cache(lua_State *L, int var, int key, int value)
{
    size_t length;
    const char *name;
    var = ac_abs(L, var); key = ac_abs(L, key); value = ac_abs(L, value);
    name = lua_tolstring(L, key, &length);
    if ((length == 4 && ngx_memcmp(name, "args", 4) == 0)
        || (length == 7 && ngx_memcmp(name, "is_args", 7) == 0)) return;
    lua_getfield(L, var, "_cache");
    lua_pushvalue(L, key);
    lua_pushvalue(L, value);
    lua_rawset(L, -3);
    lua_pop(L, 1);
}

static int
ac_var_resume(lua_State *L)
{
    int state = lua_upvalueindex(1), result;
    if (!lua_toboolean(L, 1)) {
        lua_pushvalue(L, 2);
        return lua_error(L);
    }
    lua_settop(L, 2);
    lua_getfield(L, state, "select");
    if (!lua_isnil(L, -1)) {
        lua_pushvalue(L, -1);
        lua_gettable(L, 2);
        if (lua_istable(L, -1)) lua_rawgeti(L, -1, 1);
        lua_replace(L, 2);
    }
    lua_pop(L, 1);
    if (!ac_bool_field(L, state, "uncached")) {
        lua_getfield(L, state, "var");
        lua_getfield(L, state, "key");
        ac_var_cache(L, -2, -1, 2);
        lua_pop(L, 2);
    }
    result = 2;
    lua_pushnil(L);
    lua_pushvalue(L, result);
    return 2;
}

static int
ac_var_index(lua_State *L)
{
    const char *key;
    int ctx, getters, entry, state, callable;
    lua_settop(L, 2);
    if (lua_type(L, 2) != LUA_TSTRING) return luaL_error(L, "invalid argument, expect string value");
    key = lua_tostring(L, 2);
    lua_getfield(L, 1, "_cache");
    lua_pushvalue(L, 2);
    lua_rawget(L, -2);
    if (!lua_isnil(L, -1)) return ac_done(L, 1);
    lua_pop(L, 2);
    lua_getfield(L, 1, "_ctx");
    ctx = lua_gettop(L);
    ac_push(L, ac_main()->var_getters_ref);
    getters = lua_gettop(L);
    lua_pushvalue(L, 2);
    lua_rawget(L, getters);
    entry = lua_gettop(L);
    if (lua_istable(L, entry) || ac_name(key, "post_arg_")) {
        lua_newtable(L);
        state = lua_gettop(L);
        lua_pushvalue(L, 1); lua_setfield(L, state, "var");
        lua_pushvalue(L, 2); lua_setfield(L, state, "key");
        if (lua_istable(L, entry)) {
            lua_getfield(L, entry, "getter");
            callable = lua_gettop(L);
            lua_getfield(L, entry, "uncached");
            lua_setfield(L, state, "uncached");
        } else {
            lua_pushcfunction(L, ac_header);
            lua_pushvalue(L, ctx);
            lua_pushliteral(L, "Content-Type");
            lua_call(L, 2, 1);
            const char *type = lua_tostring(L, -1);
            if (!type || !ac_name(type, "application/x-www-form-urlencoded")) {
                lua_pushnil(L);
                return ac_done(L, 1);
            }
            lua_pop(L, 1);
            lua_pushstring(L, key + 9); lua_setfield(L, state, "select");
            ac_require(L, "apisix.core.request");
            lua_getfield(L, -1, "get_post_args");
            lua_remove(L, -2);
            callable = lua_gettop(L);
        }
        lua_pushvalue(L, ctx);
        return ac_io_call(L, state, ac_var_resume, callable, -1, 1);
    }
    lua_pop(L, 2);
    if (ngx_strcmp(key, "method") == 0) {
        ngx_http_request_t *request = ngx_http_lua_get_request(L);
        lua_pushlstring(L, (char *) request->method_name.data, request->method_name.len);
    } else if (ac_name(key, "arg_")) {
        lua_pushcfunction(L, ac_uri_args);
        lua_pushvalue(L, ctx);
        lua_call(L, 1, 1);
        lua_getfield(L, -1, key + 4);
        if (lua_istable(L, -1)) lua_rawgeti(L, -1, 1);
    } else if (ac_name(key, "uri_param_")) {
        lua_getfield(L, ctx, "curr_req_matched");
        if (lua_istable(L, -1)) lua_getfield(L, -1, key + 10);
    } else if (ngx_strcmp(key, "cookie") == 0 || ac_name(key, "cookie_")) {
        ac_require(L, "resty.cookie");
        lua_getfield(L, -1, "new");
        lua_pushvalue(L, -2);
        lua_call(L, 1, 1);
        if (ac_name(key, "cookie_") && lua_istable(L, -1)) {
            lua_getfield(L, -1, "get");
            lua_pushvalue(L, -2);
            lua_pushstring(L, key + 7);
            lua_call(L, 2, 1);
        }
    } else {
        static const char *names[] = {"balancer_ip", "balancer_port", "consumer_group_id", "consumer_name",
            "resp_body", "route_id", "route_name", "service_id", "service_name", "upstream_unresolved_host", NULL};
        int i, builtin = 0;
        for (i = 0; names[i]; i++) if (ngx_strcmp(key, names[i]) == 0) { builtin = 1; break; }
        if (builtin) {
            lua_getfield(L, ctx, key);
            if (lua_isnil(L, -1) && ngx_strcmp(key, "resp_body") == 0) lua_pushliteral(L, "");
        } else {
            size_t length;
            lua_tolstring(L, 2, &length);
            ac_native_var_fetch(L, ngx_http_lua_get_request(L), key, length);
        }
    }
    ac_var_cache(L, 1, 2, -1);
    return ac_done(L, 1);
}

static int
ac_var_newindex(lua_State *L)
{
    static const char *writable[] = {
        "upstream_scheme", "upstream_host", "upstream_unresolved_host", "upstream_upgrade", "upstream_connection",
        "upstream_uri", "request_line", "llm_content_risk_level", "apisix_request_id", "request_type",
        "apisix_upstream_response_time", "llm_time_to_first_token", "request_llm_model", "llm_model", "llm_prompt_tokens",
        "llm_completion_tokens", "llm_total_tokens", "llm_stream", "llm_has_tool_calls", "llm_tool_count",
        "llm_end_user_id", "llm_cache_read_input_tokens", "llm_cache_creation_input_tokens", "llm_reasoning_tokens",
        "upstream_mirror_host", "upstream_mirror_uri", "upstream_mirror_grpc_path", "upstream_cache_zone",
        "upstream_cache_zone_info", "upstream_no_cache", "upstream_cache_key", "upstream_cache_bypass",
        "rate_limiting_info", "original_x_forwarded_for", NULL
    };
    int i;
    const char *key = luaL_checkstring(L, 2);
    lua_settop(L, 3);
    for (i = 0; writable[i]; i++) {
        if (ngx_strcmp(key, writable[i]) == 0) {
            ngx_str_t value = ngx_null_string;
            if (!lua_isnil(L, 3)) {
                if (!lua_isstring(L, 3)) {
                    lua_getglobal(L, "tostring"); lua_pushvalue(L, 3); lua_call(L, 1, 1);
                } else lua_pushvalue(L, 3);
                value.data = (u_char *) lua_tolstring(L, -1, &value.len);
            }
            ac_native_var_set(L, ngx_http_lua_get_request(L), key, value);
            break;
        }
    }
    lua_getfield(L, 1, "_cache");
    lua_pushvalue(L, 2);
    lua_pushvalue(L, 3);
    lua_rawset(L, -3);
    return 0;
}

static int
ac_var_register(lua_State *L)
{
    int entry;
    luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_settop(L, 3);
    ac_push(L, ac_main()->var_getters_ref);
    lua_pushvalue(L, 1);
    lua_newtable(L);
    entry = lua_gettop(L);
    lua_pushvalue(L, 2); lua_setfield(L, entry, "getter");
    if (lua_istable(L, 3)) {
        lua_getfield(L, 3, "no_cacheable");
        lua_setfield(L, entry, "uncached");
    }
    lua_rawset(L, -3);
    return 0;
}

static int
ac_set_vars(lua_State *L)
{
    int var;
    luaL_checktype(L, 1, LUA_TTABLE);
    ac_require(L, "tablepool");
    lua_getfield(L, -1, "fetch");
    lua_pushliteral(L, "ctx_var");
    lua_pushinteger(L, 0);
    lua_pushinteger(L, 32);
    lua_call(L, 3, 1);
    var = lua_gettop(L);
    lua_getfield(L, var, "_cache");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_setfield(L, var, "_cache");
    } else lua_pop(L, 1);
    lua_pushvalue(L, 1); lua_setfield(L, var, "_ctx");
    ac_require(L, "resty.ngxvar");
    lua_getfield(L, -1, "request");
    lua_call(L, 0, 1);
    lua_setfield(L, var, "_request");
    lua_pop(L, 1);
    ac_push(L, ac_main()->var_meta_ref);
    lua_setmetatable(L, var);
    lua_pushvalue(L, var);
    lua_setfield(L, 1, "var");
    return 0;
}

static int
ac_release_vars(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "var");
    if (lua_isnil(L, -1)) return 0;
    ac_require(L, "table.clear");
    lua_getfield(L, 2, "_cache");
    lua_call(L, 1, 0);
    ac_require(L, "tablepool");
    lua_getfield(L, -1, "release");
    lua_pushliteral(L, "ctx_var");
    lua_pushvalue(L, 2);
    lua_pushboolean(L, 1);
    lua_call(L, 3, 0);
    lua_pushnil(L);
    lua_setfield(L, 1, "var");
    return 0;
}

void
ac_sdk_context(lua_State *L, ac_request_t *ctx)
{
    ac_require(L, "apisix.core.ctx");
    lua_getfield(L, -1, "set_vars_meta");
    ac_push(L, ctx->table_ref);
    lua_call(L, 1, 0);
    lua_pop(L, 1);
}

static int
ac_ctx_open(lua_State *L)
{
    static const luaL_Reg functions[] = {
        {"set_vars_meta", ac_set_vars}, {"release_vars", ac_release_vars}, {"register_var", ac_var_register}, {NULL, NULL}
    };
    ac_main_conf_t *main = ac_main();
    lua_newtable(L);
    main->var_getters_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_newtable(L);
    ac_bind(L, ac_var_index, 0);
    ac_require(L, "apisix.native.runtime"); lua_getfield(L, -1, "cached_index");
    lua_remove(L, -2); lua_insert(L, -2); lua_call(L, 1, 1);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, ac_var_newindex);
    lua_setfield(L, -2, "__newindex");
    main->var_meta_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_newtable(L);
    luaL_register(L, NULL, functions);
    return 1;
}

static int
ac_request_open(lua_State *L)
{
    static const luaL_Reg functions[] = {
        {"headers", ac_headers}, {"header", ac_header},
        {"get_uri_args", ac_uri_args}, {"set_uri_args", ac_set_uri_args}, {NULL, NULL}
    };
    static const char *body_methods[] = {"get_body", "get_post_args", "get_request_body_table", "get_json_request_body_table"};
    static struct { const char *method; const char *name; int number; } vars[] = {
        {"get_ip", "realip_remote_addr", 0}, {"get_remote_client_ip", "remote_addr", 0},
        {"get_remote_client_port", "remote_port", 1}, {"get_scheme", "scheme", 0},
        {"get_host", "host", 0}, {"get_port", "server_port", 1}
    };
    int i;
    lua_newtable(L);
    luaL_register(L, NULL, functions);
    lua_pushboolean(L, 0); lua_pushcclosure(L, ac_modify_header, 1); lua_setfield(L, -2, "set_header");
    lua_pushboolean(L, 1); lua_pushcclosure(L, ac_modify_header, 1); lua_setfield(L, -2, "add_header");
    for (i = 0; i < 4; i++) {
        lua_pushinteger(L, i);
        ac_bind(L, ac_body_start, 1);
        lua_setfield(L, -2, body_methods[i]);
    }
    for (i = 0; i < (int) (sizeof(vars) / sizeof(vars[0])); i++) {
        lua_pushstring(L, vars[i].name);
        lua_pushboolean(L, vars[i].number);
        lua_pushcclosure(L, ac_request_var, 2);
        lua_setfield(L, -2, vars[i].method);
    }
    return 1;
}

ngx_int_t
ac_request_register(ngx_conf_t *cf)
{
    if (ngx_http_lua_add_package_preload(cf, "apisix.core.ctx", ac_ctx_open) != NGX_OK
        || ngx_http_lua_add_package_preload(cf, "apisix.core.request", ac_request_open) != NGX_OK) return NGX_ERROR;
    return ac_merge_register(cf);
}
