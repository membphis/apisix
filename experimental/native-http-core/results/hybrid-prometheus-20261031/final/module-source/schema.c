/* SPDX-License-Identifier: Apache-2.0 */
#include "engine.h"
#include "schema_data.h"
#include "redis_data.h"

static void
ac_schema_value(lua_State *L, int value, int tables)
{
    value = ac_abs(L, value);
    lua_rawgeti(L, value, 1);
    if (ngx_strcmp(lua_tostring(L, -1), "ref") == 0) {
        lua_pop(L, 1);
        lua_rawgeti(L, value, 2);
        lua_rawgeti(L, tables, (int) lua_tointeger(L, -1));
        lua_remove(L, -2);
    } else {
        lua_pop(L, 1);
        lua_rawgeti(L, value, 2);
    }
}

static int
ac_realm_schema(lua_State *L)
{
    lua_settop(L, 1);
    lua_createtable(L, 0, 5);
    lua_pushliteral(L, "string");
    lua_setfield(L, -2, "type");
    lua_pushliteral(L, "^[\x20-\x21\x23-\x5B\\]-\x7E]+$");
    lua_setfield(L, -2, "pattern");
    lua_pushvalue(L, 1);
    lua_setfield(L, -2, "default");
    lua_pushinteger(L, 1);
    lua_setfield(L, -2, "minLength");
    lua_pushinteger(L, 128);
    lua_setfield(L, -2, "maxLength");
    return 1;
}

static void
ac_data_open(lua_State *L, const char *data, size_t length)
{
    int graph, nodes, tables, i, j, count, entries, dest, pair;
    ac_require(L, "cjson.safe");
    lua_getfield(L, -1, "decode");
    lua_pushlstring(L, data, length);
    lua_call(L, 1, 1);
    graph = lua_gettop(L);
    lua_getfield(L, graph, "nodes");
    nodes = lua_gettop(L);
    count = (int) lua_objlen(L, nodes);
    lua_createtable(L, count, 0);
    tables = lua_gettop(L);
    for (i = 1; i <= count; i++) {
        lua_newtable(L);
        lua_rawseti(L, tables, i);
    }
    for (i = 1; i <= count; i++) {
        lua_rawgeti(L, nodes, i);
        entries = lua_gettop(L);
        lua_rawgeti(L, tables, i);
        dest = lua_gettop(L);
        for (j = 1; j <= (int) lua_objlen(L, entries); j++) {
            lua_rawgeti(L, entries, j);
            pair = lua_gettop(L);
            lua_rawgeti(L, pair, 1);
            ac_schema_value(L, -1, tables);
            lua_remove(L, -2);
            lua_rawgeti(L, pair, 2);
            ac_schema_value(L, -1, tables);
            lua_remove(L, -2);
            lua_rawset(L, dest);
            lua_pop(L, 1);
        }
        lua_pop(L, 2);
    }
    lua_getfield(L, graph, "root");
    lua_rawgeti(L, tables, (int) lua_tointeger(L, -1));
}

static int
ac_schema_def_open(lua_State *L)
{
    ac_data_open(L, ac_schema_graph, sizeof(ac_schema_graph) - 1);
    lua_pushcfunction(L, ac_realm_schema);
    lua_setfield(L, -2, "get_realm_schema");
    return 1;
}

static int
ac_redis_data_open(lua_State *L)
{
    ac_data_open(L, ac_redis_graph, sizeof(ac_redis_graph) - 1);
    return 1;
}

static int
ac_secret_ref(lua_State *L)
{
    size_t len;
    const u_char *value;
    int is_ref;
    if (lua_type(L, 1) != LUA_TSTRING) {
        lua_pushboolean(L, 0);
        return 1;
    }
    if (lua_istable(L, 2)) {
        lua_getfield(L, 2, "type");
        if (!lua_isnil(L, -1) && (!lua_isstring(L, -1)
                || ngx_strcmp(lua_tostring(L, -1), "string") != 0)) {
            lua_pushboolean(L, 0);
            return 1;
        }
        lua_pop(L, 1);
    }
    value = (u_char *) lua_tolstring(L, 1, &len);
    is_ref = (len >= 10 && ngx_memcmp(value, "$secret://", 10) == 0)
        || (len >= 7 && ngx_strncasecmp((u_char *) value, (u_char *) "$ENV://", 7) == 0);
    lua_pushboolean(L, is_ref);
    return 1;
}

static int
ac_schema_validator(lua_State *L)
{
    int options, cached;
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, lua_upvalueindex(1), "get");
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_pushvalue(L, 1);
    lua_call(L, 2, 1);
    if (!lua_isnil(L, -1)) {
        lua_pushnil(L);
        return 2;
    }
    lua_pop(L, 1);
    ac_require(L, "jsonschema");
    lua_getfield(L, -1, "generate_validator");
    lua_pushvalue(L, 1);
    lua_newtable(L);
    options = lua_gettop(L);
    lua_pushcfunction(L, ac_secret_ref);
    lua_setfield(L, options, "skip_validation");
    if (lua_pcall(L, 2, 1, 0) != 0) {
        lua_pushnil(L);
        lua_insert(L, -2);
        return 2;
    }
    cached = lua_gettop(L);
    lua_getfield(L, lua_upvalueindex(1), "set");
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_pushvalue(L, 1);
    lua_pushvalue(L, cached);
    lua_pushinteger(L, 0);
    lua_call(L, 4, 0);
    lua_pushnil(L);
    return 2;
}

static int
ac_schema_check(lua_State *L)
{
    int count;
    lua_settop(L, 2);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_pushvalue(L, 1);
    lua_call(L, 1, 2);
    if (!lua_isfunction(L, -2)) {
        lua_pushboolean(L, 0);
        lua_replace(L, -3);
        return 2;
    }
    lua_pop(L, 1);
    lua_pushvalue(L, 2);
    lua_call(L, 1, LUA_MULTRET);
    count = lua_gettop(L) - 2;
    return count;
}

static int
ac_schema_readonly(lua_State *L)
{
    return luaL_error(L, "no modification allowed");
}

static int
ac_schema_open(lua_State *L)
{
    int module;
    lua_newtable(L);
    module = lua_gettop(L);
    lua_pushinteger(L, 1);
    lua_setfield(L, module, "TYPE_CONSUMER");
    lua_pushinteger(L, 2);
    lua_setfield(L, module, "TYPE_METADATA");
    ac_require(L, "resty.lrucache");
    lua_getfield(L, -1, "new");
    lua_pushinteger(L, 1000);
    lua_call(L, 1, 1);
    lua_remove(L, -2);
    lua_pushcclosure(L, ac_schema_validator, 1);
    lua_pushvalue(L, -1);
    lua_setfield(L, module, "valid");
    lua_pushcclosure(L, ac_schema_check, 1);
    lua_setfield(L, module, "check");
    lua_newtable(L);
    ac_require(L, "apisix.schema_def");
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, ac_schema_readonly);
    lua_setfield(L, -2, "__newindex");
    lua_setmetatable(L, module);
    return 1;
}

ngx_int_t
ac_schema_register(ngx_conf_t *cf)
{
    if (ngx_http_lua_add_package_preload(cf, "apisix.schema_def", ac_schema_def_open) != NGX_OK
        || ngx_http_lua_add_package_preload(cf, "apisix.core.schema", ac_schema_open) != NGX_OK
        || ngx_http_lua_add_package_preload(cf, "apisix.utils.redis-schema", ac_redis_data_open) != NGX_OK) {
        return NGX_ERROR;
    }
    return NGX_OK;
}
