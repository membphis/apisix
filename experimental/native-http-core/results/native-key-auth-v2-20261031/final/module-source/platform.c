/* SPDX-License-Identifier: Apache-2.0 */
#include "engine.h"
#include <math.h>
#include <sys/wait.h>
#include <openssl/err.h>

extern char **environ;

static int
ac_os_setenv(lua_State *L)
{
    if (lua_type(L, 1) != LUA_TSTRING || (lua_type(L, 2) != LUA_TSTRING && lua_type(L, 2) != LUA_TNUMBER)) {
        lua_pushboolean(L, 0); lua_pushliteral(L, "invalid argument"); return 2;
    }
    const char *name = lua_tostring(L, 1), *value = lua_tostring(L, 2);
    if (setenv(name, value, 1) != 0) { lua_pushboolean(L, 0); lua_pushstring(L, strerror(errno)); return 2; }
    lua_pushboolean(L, 1); return 1;
}

static int
ac_os_usleep(lua_State *L)
{
    lua_Number n = luaL_checknumber(L, 1);
    if (ceil(n) != floor(n)) return luaL_error(L, "bad microseconds: %f", n);
    usleep((useconds_t) n); return 0;
}

static int
ac_os_waitpid(lua_State *L)
{
    pid_t pid = (pid_t) luaL_checkinteger(L, 1), result;
    lua_Number timeout = luaL_checknumber(L, 2) * 1e6;
    for (lua_Number elapsed = 0; elapsed < timeout; elapsed += 10000) {
        usleep(10000); result = waitpid(pid, NULL, WNOHANG);
        if (result == -1) { lua_pushnil(L); lua_pushstring(L, strerror(errno)); return 2; }
        if (result > 0) { lua_pushboolean(L, 1); return 1; }
    }
    return 0;
}

static int
ac_os_open(lua_State *L)
{
    static const luaL_Reg methods[] = {{"setenv", ac_os_setenv}, {"usleep", ac_os_usleep}, {"waitpid", ac_os_waitpid}, {NULL, NULL}};
    lua_newtable(L); luaL_register(L, NULL, methods); return 1;
}

static int
ac_env_init(lua_State *L)
{
    int cache = lua_upvalueindex(1);
    char **item, *separator;
    for (item = environ; item && *item; item++) {
        separator = strchr(*item, '=');
        if (!separator) continue;
        lua_pushlstring(L, *item, separator - *item); lua_pushstring(L, separator + 1); lua_rawset(L, cache);
    }
    return 0;
}

static int
ac_env_get(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1), *value;
    lua_pushvalue(L, 1); lua_rawget(L, lua_upvalueindex(1));
    if (!lua_isnil(L, -1)) return 1;
    value = getenv(name); if (value) lua_pushstring(L, value); else lua_pushnil(L);
    return 1;
}

static int
ac_env_fetch(lua_State *L)
{
    size_t len, split;
    const char *text, *value;
    luaL_checkany(L, 1);
    if (lua_type(L, 1) != LUA_TSTRING) {
        lua_pushnil(L); lua_pushfstring(L, "error env_uri type: %s", luaL_typename(L, 1)); return 2;
    }
    text = lua_tolstring(L, 1, &len);
    if (len < 7 || ngx_strncasecmp((u_char *) text, (u_char *) "$ENV://", 7) != 0) {
        lua_pushnil(L); lua_pushfstring(L, "error env_uri prefix: %s", text); return 2;
    }
    for (split = 7; split < len && text[split] != '/'; split++) { }
    lua_pushlstring(L, text + 7, split - 7);
    lua_pushvalue(L, -1); lua_rawget(L, lua_upvalueindex(1));
    if (lua_isnil(L, -1)) {
        value = getenv(lua_tostring(L, -2)); lua_pop(L, 1); if (value) lua_pushstring(L, value); else lua_pushnil(L);
    }
    if (split + 1 < len && !lua_isnil(L, -1)) {
        int raw = lua_gettop(L);
        ac_require(L, "cjson.safe"); lua_getfield(L, -1, "decode"); lua_pushvalue(L, raw); lua_call(L, 1, 2);
        if (!lua_toboolean(L, -2)) {
            lua_pushnil(L); lua_pushfstring(L, "decode failed, err: %s, value: %s", lua_tostring(L, -2), lua_tostring(L, raw)); return 2;
        }
        lua_pop(L, 1); lua_pushlstring(L, text + split + 1, len - split - 1); lua_gettable(L, -2);
    }
    return 1;
}

static int
ac_env_open(lua_State *L)
{
    int module, cache;
    lua_newtable(L); module = lua_gettop(L); lua_newtable(L); cache = lua_gettop(L);
    lua_pushvalue(L, cache); lua_pushcclosure(L, ac_env_init, 1); lua_setfield(L, module, "init");
    lua_pushvalue(L, cache); lua_pushcclosure(L, ac_env_get, 1); lua_setfield(L, module, "get");
    lua_pushvalue(L, cache); lua_pushcclosure(L, ac_env_fetch, 1); lua_setfield(L, module, "fetch_by_uri");
    lua_pushliteral(L, "$ENV://"); lua_setfield(L, module, "PREFIX"); lua_pop(L, 1); return 1;
}

static int
ac_io_file(lua_State *L)
{
    int file, result;
    lua_getglobal(L, "io"); lua_getfield(L, -1, "open"); lua_pushvalue(L, 1); lua_pushliteral(L, "r"); lua_call(L, 2, 2);
    if (lua_isnil(L, -2)) return 2;
    lua_pop(L, 1); file = lua_gettop(L); lua_getfield(L, file, "read"); lua_pushvalue(L, file); lua_pushliteral(L, "*all"); lua_call(L, 2, 1);
    result = lua_gettop(L); lua_getfield(L, file, "close"); lua_pushvalue(L, file); lua_call(L, 1, 0); lua_pushvalue(L, result); return 1;
}

static int
ac_io_open(lua_State *L)
{
    lua_newtable(L); lua_pushcfunction(L, ac_io_file); lua_setfield(L, -2, "get_file"); return 1;
}

static int
ac_ip_create(lua_State *L)
{
    ac_require(L, "resty.ipmatcher"); lua_getfield(L, -1, "new"); lua_pushvalue(L, 1); lua_call(L, 1, 2);
    if (!lua_toboolean(L, -2)) { lua_pushnil(L); return 1; }
    lua_pop(L, 1); return 1;
}

static int
ac_ip_validate(lua_State *L)
{
    const char *text;
    size_t len, split;
    lua_Number mask = 0;
    text = luaL_checklstring(L, 1, &len);
    for (split = 0; split < len && text[split] != '/'; split++) { }
    if (split < len) {
        lua_pushlstring(L, text + split + 1, len - split - 1);
        if (!lua_isnumber(L, -1)) return luaL_error(L, "attempt to compare nil with number");
        mask = lua_tonumber(L, -1); lua_pop(L, 1);
        if (mask < 0 || mask > 128) { lua_pushboolean(L, 0); return 1; }
    }
    ac_require(L, "resty.ipmatcher"); int module = lua_gettop(L);
    lua_getfield(L, module, "parse_ipv4"); lua_pushlstring(L, text, split); lua_call(L, 1, 1);
    if (lua_toboolean(L, -1)) { lua_pushboolean(L, mask <= 32); return 1; }
    lua_pop(L, 1); lua_getfield(L, module, "parse_ipv6"); lua_pushlstring(L, text, split);
    int before = lua_gettop(L) - 2; lua_call(L, 1, LUA_MULTRET); return lua_gettop(L) - before;
}

static int
ac_ip_open(lua_State *L)
{
    lua_newtable(L); lua_pushcfunction(L, ac_ip_create); lua_setfield(L, -2, "create_ip_matcher");
    lua_pushcfunction(L, ac_ip_validate); lua_setfield(L, -2, "validate_cidr_or_ip"); return 1;
}

static int
ac_ws_frame(lua_State *L)
{
    int op = lua_tointeger(L, lua_upvalueindex(2));
    ac_ngx(L, "ctx", NULL); lua_pushvalue(L, lua_upvalueindex(1));
    if (op == 0) { lua_pushvalue(L, 1); lua_settable(L, -3); return 0; }
    lua_gettable(L, -2);
    if (op == 1) return 1;
    lua_pushvalue(L, 1); lua_setfield(L, -2, op == 2 ? "payload" : "code"); return 0;
}

static int
ac_ws_role(lua_State *L)
{
    const char *role = lua_type(L, 1) == LUA_TSTRING ? lua_tostring(L, 1) : NULL;
    if (role && (ngx_strcmp(role, "client") == 0 || ngx_strcmp(role, "upstream") == 0)) {
        lua_getfield(L, lua_upvalueindex(1), role); return 1;
    }
    lua_getglobal(L, "tostring"); lua_pushvalue(L, 1); lua_call(L, 1, 1);
    lua_pushnil(L); lua_pushfstring(L, "invalid role: %s", lua_tostring(L, -2)); return 2;
}

static int
ac_ws_open(lua_State *L)
{
    static const char *roles[] = {"client", "upstream"};
    static const char *methods[] = {"stash_frame", "get_frame", "set_frame_data", "set_status"};
    int i, j, module;
    lua_newtable(L); module = lua_gettop(L);
    for (i = 0; i < 2; i++) {
        lua_newtable(L);
        for (j = 0; j < 4; j++) {
            lua_pushfstring(L, "websocket_%s", roles[i]); lua_pushinteger(L, j); lua_pushcclosure(L, ac_ws_frame, 2);
            lua_setfield(L, -2, methods[j]);
        }
        lua_setfield(L, module, roles[i]);
    }
    lua_pushliteral(L, "client"); lua_setfield(L, module, "ROLE_CLIENT");
    lua_pushliteral(L, "upstream"); lua_setfield(L, module, "ROLE_UPSTREAM");
    lua_pushvalue(L, module); lua_pushcclosure(L, ac_ws_role, 1); lua_setfield(L, module, "get_role"); return 1;
}

static int
ac_iv_table(lua_State *L)
{
    int result, keys, i, key, aes;
    size_t length;
    lua_settop(L, 1);
    if (lua_type(L, 1) == LUA_TSTRING) { lua_newtable(L); lua_pushvalue(L, 1); lua_rawseti(L, -2, 1); keys = lua_gettop(L); }
    else keys = 1;
    lua_newtable(L); result = lua_gettop(L);
    if (!lua_istable(L, keys)) return 1;
    for (i = 1; i <= (int) lua_objlen(L, keys); i++) {
        lua_rawgeti(L, keys, i); key = lua_gettop(L); luaL_checklstring(L, key, &length);
        if (length != 16 && length != 32) {
            ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "ignored data_encryption keyring entry with invalid byte length");
            lua_pop(L, 1); continue;
        }
        ac_require(L, "resty.aes"); aes = lua_gettop(L); lua_getfield(L, aes, "new");
        lua_pushvalue(L, aes); lua_pushvalue(L, key); lua_pushnil(L);
        lua_getfield(L, aes, "cipher"); lua_pushinteger(L, length == 32 ? 256 : 128); lua_pushliteral(L, "cbc"); lua_call(L, 2, 1);
        lua_newtable(L); lua_pushvalue(L, key); lua_setfield(L, -2, "iv"); lua_call(L, 5, 2);
        if (!lua_toboolean(L, -2)) return luaL_error(L, "cipher initialization failed: %s", lua_tostring(L, -1));
        lua_pop(L, 1); lua_rawseti(L, result, (int) lua_objlen(L, result) + 1); lua_pop(L, 2);
    }
    lua_pushvalue(L, result); return 1;
}

static int
ac_encrypt(lua_State *L)
{
    lua_rawgeti(L, 1, 1);
    if (lua_isnil(L, -1)) { lua_pushnil(L); lua_pushliteral(L, "no keyring configured"); return 2; }
    lua_getfield(L, -1, "encrypt"); lua_pushvalue(L, -2); lua_pushvalue(L, 2); lua_call(L, 2, 1);
    if (lua_isnil(L, -1)) { lua_pushnil(L); lua_pushliteral(L, "failed to encrypt"); return 2; }
    ac_ngx(L, NULL, "encode_base64"); lua_insert(L, -2); lua_call(L, 1, 1); return 1;
}

static int
ac_decrypt(lua_State *L)
{
    int decoded, i;
    const char *what;
    lua_settop(L, 3);
    what = lua_toboolean(L, 3) ? luaL_checkstring(L, 3) : NULL;
    ac_ngx(L, NULL, "decode_base64"); lua_pushvalue(L, 2); lua_call(L, 1, 1); decoded = lua_gettop(L);
    if (lua_toboolean(L, decoded)) {
        for (i = 1; i <= (int) lua_objlen(L, 1); i++) {
            lua_rawgeti(L, 1, i); lua_getfield(L, -1, "decrypt"); lua_pushvalue(L, -2); lua_pushvalue(L, decoded); lua_call(L, 2, 1);
            if (lua_toboolean(L, -1)) return 1;
            ERR_clear_error(); lua_pop(L, 2);
        }
    }
    lua_pushnil(L);
    lua_pushfstring(L, "%s%s%sfailed", lua_toboolean(L, decoded) ? "decrypt " : "base64 decode ", what ? what : "", what ? " " : ""); return 2;
}

static int
ac_keyring_operation(lua_State *L)
{
    int state = lua_upvalueindex(1), decrypt = lua_toboolean(L, lua_upvalueindex(2)), keys;
    lua_settop(L, 2); lua_getfield(L, state, "keyring");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1); ac_push(L, ac_main()->local_ref); lua_getfield(L, -1, "apisix");
        lua_getfield(L, -1, "data_encryption");
        if (lua_istable(L, -1)) lua_getfield(L, -1, "keyring"); else lua_pushnil(L);
        lua_pushcfunction(L, ac_iv_table); lua_pushvalue(L, -2); lua_call(L, 1, 1);
        lua_pushvalue(L, -1); lua_setfield(L, state, "keyring");
    }
    keys = lua_gettop(L);
    if (!lua_objlen(L, keys)) { lua_pushvalue(L, 1); return 1; }
    lua_pushcfunction(L, decrypt ? ac_decrypt : ac_encrypt);
    lua_pushvalue(L, keys); lua_pushvalue(L, 1); lua_pushvalue(L, 2); lua_call(L, 3, 2);
    if (!decrypt && lua_isnil(L, -2)) { lua_pushvalue(L, 1); return 1; }
    if (lua_isnil(L, -1)) { lua_pop(L, 1); return 1; }
    return 2;
}

static int
ac_encryption_open(lua_State *L)
{
    int module, state;
    lua_newtable(L); module = lua_gettop(L); lua_newtable(L); state = lua_gettop(L);
    lua_pushvalue(L, state); lua_pushboolean(L, 0); lua_pushcclosure(L, ac_keyring_operation, 2); lua_setfield(L, module, "encrypt");
    lua_pushvalue(L, state); lua_pushboolean(L, 1); lua_pushcclosure(L, ac_keyring_operation, 2); lua_setfield(L, module, "decrypt");
    lua_pushcfunction(L, ac_iv_table); lua_setfield(L, module, "init_iv_tbl");
    lua_pushcfunction(L, ac_encrypt); lua_setfield(L, module, "aes_cbc_encrypt");
    lua_pushcfunction(L, ac_decrypt); lua_setfield(L, module, "aes_cbc_decrypt"); lua_pop(L, 1); return 1;
}

ngx_int_t
ac_platform_register(ngx_conf_t *cf)
{
    static struct { const char *name; lua_CFunction open; } modules[] = {
        {"apisix.core.os", ac_os_open}, {"apisix.core.env", ac_env_open}, {"apisix.core.io", ac_io_open},
        {"apisix.core.ip", ac_ip_open}, {"apisix.core.websocket", ac_ws_open},
        {"apisix.core.data_encryption", ac_encryption_open}, {NULL, NULL}
    };
    ngx_uint_t i;
    for (i = 0; modules[i].name; i++) if (ngx_http_lua_add_package_preload(cf, modules[i].name, modules[i].open) != NGX_OK) return NGX_ERROR;
    return ac_tracing_register(cf);
}
