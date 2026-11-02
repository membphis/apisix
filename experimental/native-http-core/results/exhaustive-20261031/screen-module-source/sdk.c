/* SPDX-License-Identifier: Apache-2.0 */
#include "engine.h"

void
ac_require(lua_State *L, const char *name)
{
    lua_getfield(L, LUA_REGISTRYINDEX, "_LOADED");
    lua_getfield(L, -1, name);
    if (lua_toboolean(L, -1) && !lua_islightuserdata(L, -1)) { lua_remove(L, -2); return; }
    lua_pop(L, 2);
    lua_getglobal(L, "require");
    lua_pushstring(L, name);
    lua_call(L, 1, 1);
}

void
ac_bind(lua_State *L, lua_CFunction start, int upvalues)
{
    lua_pushcclosure(L, start, upvalues);
    ac_require(L, "apisix.native.runtime");
    lua_getfield(L, -1, "bind");
    lua_remove(L, -2);
    lua_insert(L, -2);
    lua_call(L, 1, 1);
}

void
ac_import(lua_State *L, int target, const char *module, const char *field)
{
    target = ac_abs(L, target);
    ac_require(L, module);
    if (field != NULL) {
        lua_getfield(L, -1, field);
        lua_remove(L, -2);
    }
    lua_setfield(L, target, field ? field : module);
}

static void
ac_global_member(lua_State *L, const char *global, const char *field)
{
    lua_getglobal(L, global);
    if (field) {
        lua_getfield(L, -1, field);
        lua_remove(L, -2);
    }
}

static int
ac_count_keys(lua_State *L, int table)
{
    int count = 0;
    table = ac_abs(L, table);
    lua_pushnil(L);
    while (lua_next(L, table)) {
        count++;
        lua_pop(L, 1);
    }
    return count;
}

static int
ac_table_tail(lua_State *L)
{
    int i, count = lua_gettop(L), offset;
    luaL_checktype(L, 1, LUA_TTABLE);
    offset = (int) lua_objlen(L, 1);
    for (i = 2; i <= count; i++) {
        lua_pushvalue(L, i);
        lua_rawseti(L, 1, ++offset);
    }
    lua_pushinteger(L, offset);
    return 1;
}

static int
ac_table_set(lua_State *L)
{
    int i, count = lua_gettop(L);
    luaL_checktype(L, 1, LUA_TTABLE);
    for (i = 2; i <= count; i++) {
        lua_pushvalue(L, i);
        lua_rawseti(L, 1, i - 1);
    }
    return 0;
}

static int
ac_table_attr(lua_State *L)
{
    int i, count = lua_gettop(L);
    lua_pushvalue(L, 1);
    for (i = 2; i <= count; i++) {
        if (!lua_istable(L, -1)) {
            lua_pushnil(L);
            return 1;
        }
        lua_pushvalue(L, i);
        lua_gettable(L, -2);
        lua_remove(L, -2);
    }
    return 1;
}

static int
ac_table_find(lua_State *L)
{
    size_t i;
    if (!lua_istable(L, 1)) return 0;
    for (i = 1; i <= lua_objlen(L, 1); i++) {
        lua_rawgeti(L, 1, (int) i);
        if (lua_equal(L, -1, 2)) {
            lua_pushinteger(L, (lua_Integer) i);
            return 1;
        }
        lua_pop(L, 1);
    }
    return 0;
}

static int
ac_shallow(lua_State *L, int opts, int path, int parent)
{
    size_t i, length, prefix_length;
    const char *name, *prefix;
    int match;
    if (!opts || !lua_istable(L, opts)) return 0;
    lua_getfield(L, opts, "shallows");
    if (lua_istable(L, -1)) {
        for (i = 1; i <= lua_objlen(L, -1); i++) {
            lua_rawgeti(L, -1, (int) i);
            match = lua_equal(L, -1, path);
            lua_pop(L, 1);
            if (match) {
                lua_pop(L, 1);
                return 1;
            }
        }
    }
    lua_pop(L, 1);
    lua_getfield(L, opts, "shallow_prefix");
    prefix = lua_tolstring(L, -1, &prefix_length);
    name = lua_tolstring(L, parent, &length);
    match = prefix && name && length >= prefix_length
        && ngx_memcmp(name, prefix, prefix_length) == 0
        && (length == prefix_length || name[prefix_length] == '.');
    lua_pop(L, 1);
    return match;
}

static void
ac_deepcopy(lua_State *L, int source, int copied, int parent, int opts, ngx_uint_t depth)
{
    int dest, path;
    source = ac_abs(L, source);
    if (!lua_istable(L, source)) {
        lua_pushvalue(L, source);
        return;
    }
    if (depth > 256) luaL_error(L, "table nesting exceeds maximum depth");
    lua_pushvalue(L, source);
    lua_rawget(L, copied);
    if (!lua_isnil(L, -1)) return;
    lua_pop(L, 1);
    lua_newtable(L);
    dest = lua_gettop(L);
    lua_pushvalue(L, source);
    lua_pushvalue(L, dest);
    lua_rawset(L, copied);
    lua_pushnil(L);
    while (lua_next(L, source)) {
        lua_pushvalue(L, parent);
        lua_pushliteral(L, ".");
        lua_getglobal(L, "tostring");
        lua_pushvalue(L, -5);
        lua_call(L, 1, 1);
        lua_concat(L, 3);
        path = lua_gettop(L);
        lua_pushvalue(L, -3);
        if (ac_shallow(L, opts, path, parent)) {
            lua_pushvalue(L, -3);
        } else {
            ac_deepcopy(L, path - 1, copied, path, opts, depth + 1);
        }
        lua_rawset(L, dest);
        lua_pop(L, 2);
    }
    if (lua_getmetatable(L, source)) lua_setmetatable(L, dest);
}

static int
ac_table_copy(lua_State *L)
{
    int copied, parent;
    lua_settop(L, 2);
    lua_newtable(L);
    copied = lua_gettop(L);
    lua_pushliteral(L, "self");
    parent = lua_gettop(L);
    ac_deepcopy(L, 1, copied, parent, 2, 0);
    return 1;
}

static void
ac_merge(lua_State *L, int origin, int extend)
{
    int current, source, array;
    origin = ac_abs(L, origin);
    extend = ac_abs(L, extend);
    lua_pushnil(L);
    while (lua_next(L, extend)) {
        source = lua_gettop(L);
        lua_pushvalue(L, -2);
        lua_gettable(L, origin);
        current = lua_gettop(L);
        if (lua_istable(L, source) && lua_istable(L, current)) {
            ac_require(L, "table.isarray");
            lua_pushvalue(L, source);
            lua_call(L, 1, 1);
            array = lua_toboolean(L, -1);
            lua_pop(L, 1);
            ac_require(L, "table.isarray");
            lua_pushvalue(L, current);
            lua_call(L, 1, 1);
            array |= lua_toboolean(L, -1);
            lua_pop(L, 1);
            if (!array) {
                ac_merge(L, current, source);
                lua_pop(L, 2);
                continue;
            }
        }
        lua_pop(L, 1);
        lua_pushvalue(L, -2);
        if (lua_islightuserdata(L, source) && lua_touserdata(L, source) == NULL) {
            lua_pushnil(L);
        } else {
            lua_pushvalue(L, source);
        }
        lua_settable(L, origin);
        lua_pop(L, 1);
    }
}

static int
ac_table_merge(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    luaL_checktype(L, 2, LUA_TTABLE);
    ac_merge(L, 1, 2);
    lua_pushvalue(L, 1);
    return 1;
}

static ngx_flag_t
ac_equal(lua_State *L, int a, int b, ngx_flag_t values, ngx_uint_t depth)
{
    ngx_flag_t equal;
    a = ac_abs(L, a);
    if (depth > 256) luaL_error(L, "table comparison nesting exceeds maximum depth");
    b = ac_abs(L, b);
    if (!lua_istable(L, a) || !lua_istable(L, b)) return lua_equal(L, a, b);
    if (ac_count_keys(L, a) != ac_count_keys(L, b)) return 0;
    lua_pushnil(L);
    while (lua_next(L, a)) {
        lua_pushvalue(L, -2);
        lua_gettable(L, b);
        equal = values ? ac_equal(L, -2, -1, 1, depth + 1) : !lua_isnil(L, -1);
        lua_pop(L, 2);
        if (!equal) {
            lua_pop(L, 1);
            return 0;
        }
    }
    return 1;
}

static int
ac_table_equal(lua_State *L)
{
    lua_pushboolean(L, ac_equal(L, 1, 2, lua_toboolean(L, lua_upvalueindex(1)), 0));
    return 1;
}

static int
ac_table_pick(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    luaL_checktype(L, 2, LUA_TTABLE);
    lua_newtable(L);
    lua_pushnil(L);
    while (lua_next(L, 1)) {
        lua_pushvalue(L, -2);
        lua_gettable(L, 2);
        if (!lua_isnil(L, -1)) {
            lua_pushvalue(L, -3);
            lua_pushvalue(L, -3);
            lua_settable(L, 3);
        }
        lua_pop(L, 2);
    }
    return 1;
}

static int
ac_table_open(lua_State *L)
{
    static const luaL_Reg functions[] = {
        {"insert_tail", ac_table_tail}, {"set", ac_table_set},
        {"try_read_attr", ac_table_attr}, {"array_find", ac_table_find},
        {"deepcopy", ac_table_copy}, {"merge", ac_table_merge},
        {"pick", ac_table_pick}, {NULL, NULL}
    };
    static const char *builtins[] = {
        "new", "clear", "nkeys", "clone", "isarray", "isempty", NULL
    };
    int table;
    ngx_uint_t i;
    lua_newtable(L);
    table = lua_gettop(L);
    luaL_register(L, NULL, functions);
    for (i = 0; builtins[i]; i++) {
        lua_pushliteral(L, "table.");
        lua_pushstring(L, builtins[i]);
        lua_concat(L, 2);
        lua_getglobal(L, "require");
        lua_insert(L, -2);
        lua_call(L, 1, 1);
        lua_setfield(L, table, builtins[i]);
    }
    lua_pushboolean(L, 1);
    lua_pushcclosure(L, ac_table_equal, 1);
    lua_setfield(L, table, "deep_eq");
    lua_pushboolean(L, 0);
    lua_pushcclosure(L, ac_table_equal, 1);
    lua_setfield(L, table, "set_eq");
    lua_newtable(L);
    lua_getglobal(L, "table");
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, table);
    return 1;
}

static int
ac_string_affix(lua_State *L)
{
    size_t a, b;
    const char *s = luaL_checklstring(L, 1, &a);
    const char *prefix = luaL_checklstring(L, 2, &b);
    if (lua_type(L, 1) != LUA_TSTRING || lua_type(L, 2) != LUA_TSTRING) {
        return luaL_error(L, "unexpected type in string prefix/suffix");
    }
    lua_pushboolean(L, a >= b && ngx_memcmp(s +
        (lua_toboolean(L, lua_upvalueindex(1)) ? a - b : 0), prefix, b) == 0);
    return 1;
}

static int
ac_string_find(lua_State *L)
{
    lua_settop(L, 3);
    if (lua_isnil(L, 3)) {
        lua_pushinteger(L, 1);
        lua_replace(L, 3);
    }
    ac_global_member(L, "string", "find");
    lua_insert(L, 1);
    lua_pushboolean(L, 1);
    lua_call(L, 4, LUA_MULTRET);
    return lua_gettop(L);
}

static int
ac_string_rfind(lua_State *L)
{
    size_t len, chlen;
    const u_char *s = (u_char *) luaL_checklstring(L, 1, &len);
    const u_char *ch = (u_char *) luaL_checklstring(L, 2, &chlen);
    lua_Integer i = luaL_optinteger(L, 3, (lua_Integer) len);
    if (!chlen) return 0;
    for (; i >= 1; i--) {
        if ((size_t) i <= len && s[i - 1] == ch[0]) {
            lua_pushinteger(L, i);
            return 1;
        }
    }
    return 0;
}

static int
ac_string_decode(lua_State *L)
{
    lua_settop(L, 1);
    ac_global_member(L, "ngx", "decode_args");
    lua_pushvalue(L, 1);
    lua_pushinteger(L, 0);
    lua_call(L, 2, LUA_MULTRET);
    return lua_gettop(L) - 1;
}

static int
ac_string_compress(lua_State *L)
{
    int i;
    static const char *patterns[] = {"^\\s+", ",\\s+"};
    lua_settop(L, 1);
    for (i = 0; i < 2; i++) {
        ac_global_member(L, "ngx", "re");
        lua_getfield(L, -1, "gsub");
        lua_remove(L, -2);
        lua_pushvalue(L, 1);
        lua_pushstring(L, patterns[i]);
        lua_pushstring(L, i ? "," : "");
        lua_pushliteral(L, "mjo");
        lua_call(L, 4, 1);
        lua_replace(L, 1);
    }
    return 1;
}

static int
ac_string_open(lua_State *L)
{
    static const luaL_Reg functions[] = {
        {"find", ac_string_find}, {"rfind_char", ac_string_rfind},
        {"decode_args", ac_string_decode}, {"compress_script", ac_string_compress}, {NULL, NULL}
    };
    lua_newtable(L);
    luaL_register(L, NULL, functions);
    lua_pushboolean(L, 0);
    lua_pushcclosure(L, ac_string_affix, 1);
    lua_setfield(L, -2, "has_prefix");
    lua_pushboolean(L, 1);
    lua_pushcclosure(L, ac_string_affix, 1);
    lua_setfield(L, -2, "has_suffix");
    ac_global_member(L, "ngx", "encode_args");
    lua_setfield(L, -2, "encode_args");
    lua_newtable(L);
    lua_getglobal(L, "string");
    lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2);
    return 1;
}

static int
ac_log(lua_State *L)
{
    int n = lua_gettop(L);
    ac_global_member(L, "ngx", "log");
    lua_insert(L, 1);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 2);
    if (!lua_isnil(L, lua_upvalueindex(2))) {
        lua_pushvalue(L, lua_upvalueindex(2));
        lua_insert(L, 3);
        n++;
    }
    lua_call(L, n + 1, 0);
    return 0;
}

static int
ac_delay_tostring(lua_State *L)
{
    int n, i;
    lua_getfield(L, 1, "func");
    lua_getfield(L, 1, "args");
    n = (int) lua_objlen(L, -1);
    for (i = 1; i <= n; i++) lua_rawgeti(L, 3, i);
    lua_remove(L, 3);
    lua_call(L, n, 1);
    lua_getglobal(L, "tostring");
    lua_insert(L, -2);
    lua_call(L, 1, 1);
    return 1;
}

static int
ac_delay_exec(lua_State *L)
{
    int i, n = lua_gettop(L), result;
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_newtable(L);
    result = lua_gettop(L);
    lua_pushvalue(L, 1);
    lua_setfield(L, result, "func");
    lua_createtable(L, n - 1, 0);
    for (i = 2; i <= n; i++) {
        lua_pushvalue(L, i);
        lua_rawseti(L, -2, i - 1);
    }
    lua_setfield(L, result, "args");
    lua_newtable(L);
    lua_pushcfunction(L, ac_delay_tostring);
    lua_setfield(L, -2, "__tostring");
    lua_setmetatable(L, result);
    return 1;
}

static void ac_log_table(lua_State *L, int prefix);

static int
ac_log_new(lua_State *L)
{
    lua_settop(L, 1);
    ac_log_table(L, 1);
    return 1;
}

static void
ac_log_table(lua_State *L, int prefix)
{
    static struct { const char *name; int level; } levels[] = {
        {"stderr", 0}, {"emerg", 1}, {"alert", 2}, {"crit", 3},
        {"error", 4}, {"warn", 5}, {"notice", 6}, {"info", 7}, {"debug", 8}
    };
    ngx_uint_t i;
    lua_newtable(L);
    for (i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
        lua_pushinteger(L, levels[i].level);
        if (prefix) lua_pushvalue(L, prefix); else lua_pushnil(L);
        lua_pushcclosure(L, ac_log, 2);
        lua_setfield(L, -2, levels[i].name);
    }
    lua_pushcfunction(L, ac_log_new);
    lua_setfield(L, -2, "new");
    lua_pushcfunction(L, ac_delay_exec);
    lua_setfield(L, -2, "delay_exec");
}

static int
ac_log_open(lua_State *L)
{
    ac_log_table(L, 0);
    return 1;
}

static void
ac_json_strip(lua_State *L, int value)
{
    value = ac_abs(L, value);
    lua_pushnil(L);
    while (lua_next(L, value)) {
        if (lua_islightuserdata(L, -1) && lua_touserdata(L, -1) == NULL) {
            lua_pushvalue(L, -2);
            lua_pushnil(L);
            lua_rawset(L, value);
        } else if (lua_istable(L, -1)) {
            ac_json_strip(L, -1);
        }
        lua_pop(L, 1);
    }
}

static int
ac_json_strip_api(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    ac_json_strip(L, 1);
    lua_pushvalue(L, 1);
    return 1;
}

static int
ac_json_decode(lua_State *L)
{
    lua_settop(L, 2);
    ac_require(L, "cjson.safe");
    lua_getfield(L, -1, "decode");
    lua_pushvalue(L, 1);
    lua_call(L, 1, 2);
    if (lua_istable(L, 2) && ac_bool_field(L, 2, "null_as_nil")) {
        if (lua_islightuserdata(L, -2) && lua_touserdata(L, -2) == NULL) {
            lua_pushnil(L);
            lua_replace(L, -3);
        } else if (lua_istable(L, -2)) ac_json_strip(L, -2);
    }
    return 2;
}

static void
ac_json_serialise(lua_State *L, int source, int seen)
{
    int dest, value;
    source = ac_abs(L, source);
    if (lua_istable(L, source)) {
        lua_newtable(L);
        dest = lua_gettop(L);
        lua_pushnil(L);
        while (lua_next(L, source)) {
            value = lua_gettop(L);
            ac_json_serialise(L, value - 1, seen);
            if (lua_istable(L, value)) {
                lua_pushvalue(L, value);
                lua_rawget(L, seen);
                if (lua_toboolean(L, -1)) {
                    lua_pop(L, 1);
                    lua_getglobal(L, "tostring");
                    lua_pushvalue(L, value);
                    lua_call(L, 1, 1);
                } else {
                    lua_pop(L, 1);
                    lua_pushvalue(L, value);
                    lua_pushboolean(L, 1);
                    lua_rawset(L, seen);
                    ac_json_serialise(L, value, seen);
                }
            } else ac_json_serialise(L, value, seen);
            lua_rawset(L, dest);
            lua_pop(L, 1);
        }
    } else if (lua_type(L, source) == LUA_TFUNCTION || lua_type(L, source) == LUA_TUSERDATA
               || lua_type(L, source) == 10) {
        lua_getglobal(L, "tostring");
        lua_pushvalue(L, source);
        lua_call(L, 1, 1);
    } else lua_pushvalue(L, source);
}

static int
ac_json_encode(lua_State *L)
{
    int start;
    lua_settop(L, 2);
    if (lua_toboolean(L, 2)) {
        lua_newtable(L);
        ac_json_serialise(L, 1, 3);
        lua_replace(L, 1);
        lua_pop(L, 1);
    }
    ac_require(L, "cjson.safe");
    lua_getfield(L, -1, "encode");
    lua_pushvalue(L, 1);
    start = lua_gettop(L) - 2;
    lua_call(L, 1, LUA_MULTRET);
    return lua_gettop(L) - start;
}

static int
ac_json_delay_tostring(lua_State *L)
{
    lua_getfield(L, 1, "data");
    lua_getfield(L, 1, "force");
    lua_remove(L, 1);
    return ac_json_encode(L);
}

static int
ac_json_delay(lua_State *L)
{
    lua_settop(L, 2);
    lua_newtable(L);
    lua_pushvalue(L, 1);
    lua_setfield(L, 3, "data");
    lua_pushvalue(L, 2);
    lua_setfield(L, 3, "force");
    lua_newtable(L);
    lua_pushcfunction(L, ac_json_delay_tostring);
    lua_setfield(L, -2, "__tostring");
    lua_setmetatable(L, 3);
    return 1;
}

static int
ac_json_open(lua_State *L)
{
    static const luaL_Reg functions[] = {
        {"encode", ac_json_encode}, {"decode", ac_json_decode},
        {"strip_nulls", ac_json_strip_api}, {"delay_encode", ac_json_delay}, {NULL, NULL}
    };
    int table;
    lua_newtable(L);
    table = lua_gettop(L);
    luaL_register(L, NULL, functions);
    ac_import(L, table, "cjson.safe", "null");
    ac_import(L, table, "cjson.safe", "array_mt");
    ac_require(L, "dkjson");
    lua_getfield(L, -1, "encode");
    lua_setfield(L, table, "stably_encode");
    lua_pop(L, 1);
    return 1;
}

ngx_int_t
ac_sdk_register(ngx_conf_t *cf)
{
    static struct { const char *name; lua_CFunction open; } modules[] = {
        {"apisix.core.table", ac_table_open}, {"apisix.core.string", ac_string_open},
        {"apisix.core.log", ac_log_open}, {"apisix.core.json", ac_json_open}, {NULL, NULL}
    };
    ngx_uint_t i;
    for (i = 0; modules[i].name; i++) {
        if (ngx_http_lua_add_package_preload(cf, modules[i].name, modules[i].open) != NGX_OK) {
            return NGX_ERROR;
        }
    }
    if (ac_schema_register(cf) != NGX_OK) return NGX_ERROR;
    return ac_resources_register(cf);
}
