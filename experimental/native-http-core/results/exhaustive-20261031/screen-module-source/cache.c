/* SPDX-License-Identifier: Apache-2.0 */
#include "engine.h"

static lua_Number
ac_now(void)
{
    ngx_time_t *now = ngx_timeofday();
    return (lua_Number) now->sec + now->msec / 1000.0;
}

static void
ac_cache_method(lua_State *L, int cache, const char *name)
{
    cache = ac_abs(L, cache);
    lua_getfield(L, cache, "lru");
    lua_getfield(L, -1, name); lua_insert(L, -2);
}

static void
ac_cache_touch(lua_State *L, int cache, int key)
{
    cache = ac_abs(L, cache); key = ac_abs(L, key);
    lua_getfield(L, cache, "access");
    if (lua_istable(L, -1)) {
        lua_pushvalue(L, key); lua_pushnumber(L, ac_now()); lua_rawset(L, -3);
    }
    lua_pop(L, 1);
}

static void
ac_cache_set(lua_State *L, int cache, int key, int entry)
{
    cache = ac_abs(L, cache); key = ac_abs(L, key); entry = ac_abs(L, entry);
    ac_cache_method(L, cache, "set");
    lua_pushvalue(L, key); lua_pushvalue(L, entry); lua_getfield(L, cache, "ttl");
    lua_call(L, 4, 0);
}

static int
ac_cache_hit(lua_State *L, int cache, int key, int version, int factory, int args)
{
    int object, stale, options, pending, entry, count, i;
    cache = ac_abs(L, cache); key = ac_abs(L, key); version = ac_abs(L, version);
    factory = ac_abs(L, factory); args = ac_abs(L, args);
    ac_cache_method(L, cache, "get"); lua_pushvalue(L, key); lua_call(L, 2, 2);
    object = lua_gettop(L) - 1; stale = object + 1;
    if (lua_istable(L, object)) {
        lua_getfield(L, object, "ver");
        int equal = lua_equal(L, -1, version); lua_pop(L, 1);
        if (equal) { lua_getfield(L, object, "val"); ac_cache_touch(L, cache, key); return 1; }
    }
    if (lua_istable(L, stale)) {
        lua_getfield(L, stale, "ver");
        int equal = lua_equal(L, -1, version); lua_pop(L, 1);
        if (equal) {
            lua_getfield(L, cache, "options"); options = lua_gettop(L);
            if (!ac_bool_field(L, options, "invalid_stale")) {
                ac_cache_set(L, cache, key, stale);
                lua_getfield(L, stale, "val"); ac_cache_touch(L, cache, key); return 1;
            }
            if (ac_bool_field(L, options, "refresh_stale")) {
                lua_getfield(L, cache, "pending"); pending = lua_gettop(L);
                lua_pushvalue(L, key); lua_rawget(L, pending);
                if (lua_isnil(L, -1)) { lua_pop(L, 1); lua_newtable(L); }
                entry = lua_gettop(L);
                lua_pushvalue(L, version); lua_setfield(L, entry, "ver");
                lua_pushvalue(L, factory); lua_setfield(L, entry, "fn");
                lua_newtable(L);
                count = object - args;
                for (i = 0; i < count; i++) { lua_pushvalue(L, args + i); lua_rawseti(L, -2, i + 1); }
                lua_pushinteger(L, count); lua_setfield(L, -2, "n");
                lua_setfield(L, entry, "args");
                lua_pushvalue(L, key); lua_pushvalue(L, entry); lua_rawset(L, pending);
                lua_getfield(L, stale, "val"); ac_cache_touch(L, cache, key); return 1;
            }
        }
    }
    lua_settop(L, object - 1);
    return 0;
}

static void
ac_cache_unlock(lua_State *L, int state)
{
    lua_getfield(L, state, "lock");
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "unlock"); lua_pushvalue(L, -2); lua_call(L, 1, 0);
    }
    lua_pop(L, 1);
}

static int
ac_cache_created(lua_State *L)
{
    int state = lua_upvalueindex(1), cache, entry;
    lua_settop(L, 3);
    ac_cache_unlock(L, state);
    if (!lua_toboolean(L, 1)) { lua_pushvalue(L, 2); return lua_error(L); }
    lua_getfield(L, state, "cache"); cache = lua_gettop(L);
    if (!lua_isnil(L, 2)) {
        lua_newtable(L); entry = lua_gettop(L);
        lua_pushvalue(L, 2); lua_setfield(L, entry, "val");
        lua_getfield(L, state, "version"); lua_setfield(L, entry, "ver");
        lua_getfield(L, state, "key"); ac_cache_set(L, cache, -1, entry); ac_cache_touch(L, cache, -1);
        lua_pop(L, 1);
    } else {
        lua_getfield(L, cache, "negative");
        if (lua_istable(L, -1)) {
            int neg = lua_gettop(L);
            lua_getfield(L, neg, "set"); lua_pushvalue(L, neg); lua_getfield(L, state, "key");
            lua_newtable(L); lua_pushvalue(L, 3); lua_setfield(L, -2, "err");
            lua_getfield(L, state, "version"); lua_setfield(L, -2, "ver");
            lua_getfield(L, cache, "options"); lua_getfield(L, -1, "neg_ttl"); lua_remove(L, -2);
            lua_call(L, 4, 0);
        }
        lua_pop(L, 1);
    }
    lua_pushnil(L); lua_pushvalue(L, 2); lua_pushvalue(L, 3); return 3;
}

static int
ac_cache_create_packet(lua_State *L, int state)
{
    state = ac_abs(L, state);
    lua_pushvalue(L, state); lua_pushcclosure(L, ac_cache_created, 1);
    lua_getfield(L, state, "factory"); lua_getfield(L, state, "args"); return 3;
}

static int
ac_cache_locked(lua_State *L)
{
    int state = lua_upvalueindex(1), cache, key, version, factory, first, count, i;
    if (!lua_toboolean(L, 1)) { lua_pushvalue(L, 2); return lua_error(L); }
    if (lua_isnil(L, 2)) {
        lua_pushnil(L); lua_pushnil(L);
        lua_pushfstring(L, "failed to acquire the lock: %s", lua_tostring(L, 3)); return 3;
    }
    lua_getfield(L, state, "cache"); cache = lua_gettop(L);
    lua_getfield(L, state, "key"); key = lua_gettop(L);
    lua_getfield(L, state, "version"); version = lua_gettop(L);
    lua_getfield(L, state, "factory"); factory = lua_gettop(L);
    lua_getfield(L, state, "args");
    lua_getfield(L, -1, "n"); count = (int) lua_tointeger(L, -1); lua_pop(L, 1);
    first = lua_gettop(L) + 1;
    for (i = 1; i <= count; i++) lua_rawgeti(L, first - 1, i);
    if (ac_cache_hit(L, cache, key, version, factory, first)) {
        ac_cache_unlock(L, state); lua_pushnil(L); lua_insert(L, -2); return 2;
    }
    return ac_cache_create_packet(L, state);
}

static int
ac_cache_start(lua_State *L)
{
    int cache = lua_upvalueindex(1), n = lua_gettop(L), state, i, options;
    lua_settop(L, n < 3 ? 3 : n);
    luaL_checktype(L, 3, LUA_TFUNCTION);
    lua_getfield(L, cache, "negative");
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "get"); lua_pushvalue(L, -2); lua_pushvalue(L, 1); lua_call(L, 2, 1);
        if (lua_istable(L, -1)) {
            lua_getfield(L, -1, "ver");
            if (lua_equal(L, -1, 2)) {
                lua_pop(L, 1); lua_getfield(L, -1, "err");
                lua_pushnil(L); lua_pushnil(L); lua_pushvalue(L, -3); return 3;
            }
            lua_pop(L, 1);
        }
    }
    lua_settop(L, n);
    if (ac_cache_hit(L, cache, 1, 2, 3, 4)) {
        lua_pushnil(L); lua_insert(L, -2); return 2;
    }
    lua_newtable(L); state = lua_gettop(L);
    lua_pushvalue(L, cache); lua_setfield(L, state, "cache");
    lua_pushvalue(L, 1); lua_setfield(L, state, "key");
    lua_pushvalue(L, 2); lua_setfield(L, state, "version");
    lua_pushvalue(L, 3); lua_setfield(L, state, "factory");
    lua_createtable(L, n > 3 ? n - 3 : 0, 1);
    for (i = 4; i <= n; i++) { lua_pushvalue(L, i); lua_rawseti(L, -2, i - 3); }
    lua_pushinteger(L, n > 3 ? n - 3 : 0); lua_setfield(L, -2, "n"); lua_setfield(L, state, "args");
    lua_getfield(L, cache, "options"); options = lua_gettop(L);
    if (ac_bool_field(L, options, "serial_creating")) {
        ac_ngx(L, NULL, "get_phase"); lua_call(L, 0, 1);
        const char *phase = lua_tostring(L, -1);
        int yieldable = ngx_strcmp(phase, "rewrite") == 0 || ngx_strcmp(phase, "access") == 0
            || ngx_strcmp(phase, "content") == 0 || ngx_strcmp(phase, "timer") == 0;
        lua_pop(L, 1);
        if (yieldable) {
            ac_require(L, "resty.lock"); lua_getfield(L, -1, "new"); lua_pushvalue(L, -2);
            lua_pushliteral(L, "lrucache-lock"); lua_call(L, 2, 2);
            if (lua_isnil(L, -2)) {
                lua_pushnil(L); lua_pushnil(L);
                lua_pushfstring(L, "failed to create lock: %s", lua_tostring(L, -3)); return 3;
            }
            lua_pop(L, 1);
            lua_pushvalue(L, -1); lua_setfield(L, state, "lock");
            int lock = lua_gettop(L);
            lua_pushvalue(L, state); lua_pushcclosure(L, ac_cache_locked, 1);
            lua_getfield(L, lock, "lock");
            lua_createtable(L, 2, 1); lua_pushvalue(L, lock); lua_rawseti(L, -2, 1);
            lua_getglobal(L, "tostring"); lua_pushvalue(L, 1); lua_call(L, 1, 1); lua_rawseti(L, -2, 2);
            lua_pushinteger(L, 2); lua_setfield(L, -2, "n"); return 3;
        }
    }
    return ac_cache_create_packet(L, state);
}

static int
ac_cache_new(lua_State *L)
{
    int options, cache, count, plugin, pool, index;
    lua_settop(L, 1);
    if (!lua_istable(L, 1)) { lua_newtable(L); lua_replace(L, 1); }
    options = 1;
    lua_getfield(L, options, "type"); plugin = lua_isstring(L, -1) && ngx_strcmp(lua_tostring(L, -1), "plugin") == 0; lua_pop(L, 1);
    lua_getfield(L, options, "count"); count = lua_isnil(L, -1) ? (plugin ? 8 : 1024) : luaL_checkinteger(L, -1); lua_pop(L, 1);
    lua_newtable(L); cache = lua_gettop(L);
    lua_pushvalue(L, options); lua_setfield(L, cache, "options");
    lua_getfield(L, options, "ttl");
    if (lua_isnil(L, -1)) { lua_pop(L, 1); lua_pushinteger(L, plugin ? 300 : 3600); }
    lua_setfield(L, cache, "ttl");
    ac_require(L, "resty.lrucache"); lua_getfield(L, -1, "new"); lua_pushinteger(L, count); lua_call(L, 1, 1);
    lua_setfield(L, cache, "lru"); lua_pop(L, 1);
    lua_newtable(L); lua_setfield(L, cache, "pending");
    lua_getfield(L, options, "inactive");
    if (!lua_isnil(L, -1)) { lua_newtable(L); lua_setfield(L, cache, "access"); }
    lua_pop(L, 1);
    lua_getfield(L, options, "neg_ttl"); lua_getfield(L, options, "neg_count");
    if (lua_toboolean(L, -1) && lua_toboolean(L, -2)) {
        ac_require(L, "resty.lrucache"); lua_getfield(L, -1, "new");
        lua_pushvalue(L, -3); lua_call(L, 1, 1); lua_setfield(L, cache, "negative"); lua_pop(L, 1);
    }
    lua_pop(L, 2);
    lua_pushvalue(L, lua_upvalueindex(1)); pool = lua_gettop(L);
    lua_getfield(L, pool, "next"); index = (int) lua_tointeger(L, -1) + 1; lua_pop(L, 1);
    lua_pushinteger(L, index); lua_setfield(L, pool, "next");
    lua_pushvalue(L, cache); lua_rawseti(L, pool, index); lua_pop(L, 1);
    lua_pushvalue(L, cache); ac_bind(L, ac_cache_start, 1); return 1;
}

static int
ac_forward_results(lua_State *L)
{
    int n = lua_gettop(L);
    if (!lua_toboolean(L, 1)) { lua_pushvalue(L, 2); return lua_error(L); }
    lua_pushnil(L); lua_replace(L, 1); return n;
}

static void
ac_plugin_key(lua_State *L, int ctx, int extra)
{
    ctx = ac_abs(L, ctx); extra = ac_abs(L, extra);
    lua_getfield(L, ctx, "conf_type"); lua_pushliteral(L, "#"); lua_getfield(L, ctx, "conf_id"); lua_concat(L, 3);
    if (!lua_isnil(L, extra)) { lua_pushliteral(L, "#"); lua_pushvalue(L, extra); lua_concat(L, 3); }
}

static int
ac_cache_plugin_start(lua_State *L)
{
    int n = lua_gettop(L), args, i;
    lua_pushcfunction(L, ac_forward_results); lua_pushvalue(L, 1);
    lua_createtable(L, n - 1, 1); args = lua_gettop(L);
    ac_plugin_key(L, 2, 3); lua_rawseti(L, args, 1);
    lua_getfield(L, 2, "conf_version"); lua_rawseti(L, args, 2);
    for (i = 4; i <= n; i++) { lua_pushvalue(L, i); lua_rawseti(L, args, i - 1); }
    lua_pushinteger(L, n - 1); lua_setfield(L, args, "n"); return 3;
}

static int
ac_cache_plugin_id(lua_State *L)
{
    lua_settop(L, 2); ac_plugin_key(L, 1, 2); lua_pushliteral(L, "#");
    lua_getfield(L, 1, "conf_version"); lua_concat(L, 3); return 1;
}

static int ac_background_next(lua_State *L, int state);

static int
ac_background_resume(lua_State *L)
{
    int state = lua_upvalueindex(1), item, pending, key;
    lua_getfield(L, state, "current"); item = lua_gettop(L);
    lua_getfield(L, item, "cache"); lua_getfield(L, -1, "pending"); pending = lua_gettop(L);
    lua_getfield(L, item, "key"); key = lua_gettop(L);
    lua_pushvalue(L, key); lua_rawget(L, pending); lua_getfield(L, item, "entry");
    if (lua_rawequal(L, -1, -2) && lua_toboolean(L, 1) && !lua_isnil(L, 2)) {
        int entry = lua_gettop(L);
        lua_newtable(L); lua_pushvalue(L, 2); lua_setfield(L, -2, "val");
        lua_getfield(L, entry, "ver"); lua_setfield(L, -2, "ver");
        lua_getfield(L, item, "cache"); ac_cache_set(L, -1, key, -2); lua_pop(L, 1);
        lua_pushvalue(L, key); lua_pushnil(L); lua_rawset(L, pending);
    }
    return ac_background_next(L, state);
}

static int
ac_background_next(lua_State *L, int state)
{
    int index, snapshot, item, entry;
    lua_getfield(L, state, "index"); index = (int) lua_tointeger(L, -1) + 1; lua_pop(L, 1);
    lua_pushinteger(L, index); lua_setfield(L, state, "index");
    lua_getfield(L, state, "snapshot"); snapshot = lua_gettop(L);
    lua_rawgeti(L, snapshot, index);
    if (lua_isnil(L, -1)) { lua_pushnil(L); return 1; }
    item = lua_gettop(L); lua_pushvalue(L, item); lua_setfield(L, state, "current");
    lua_getfield(L, item, "entry"); entry = lua_gettop(L);
    lua_pushvalue(L, state); lua_pushcclosure(L, ac_background_resume, 1);
    lua_getfield(L, entry, "fn"); lua_getfield(L, entry, "args"); return 3;
}

static int
ac_background_start(lua_State *L)
{
    int pool = lua_upvalueindex(1), cache, pending, snapshot, item, state, count = 0;
    if (lua_toboolean(L, 1)) { lua_pushnil(L); return 1; }
    lua_newtable(L); state = lua_gettop(L);
    lua_newtable(L); snapshot = lua_gettop(L);
    lua_pushnil(L);
    while (lua_next(L, pool)) {
        if (!lua_istable(L, -1)) { lua_pop(L, 1); continue; }
        cache = lua_gettop(L);
        lua_getfield(L, cache, "access");
        if (lua_istable(L, -1)) {
            int access = lua_gettop(L);
            lua_getfield(L, cache, "options"); lua_getfield(L, -1, "inactive");
            lua_Number inactive = lua_tonumber(L, -1); lua_pop(L, 2);
            lua_pushnil(L);
            while (lua_next(L, access)) {
                if (ac_now() - lua_tonumber(L, -1) > inactive) {
                    ac_cache_method(L, cache, "delete"); lua_pushvalue(L, -4); lua_call(L, 2, 0);
                    lua_pushvalue(L, -2); lua_pushnil(L); lua_rawset(L, access);
                    lua_getfield(L, cache, "pending"); lua_pushvalue(L, -3); lua_pushnil(L); lua_rawset(L, -3); lua_pop(L, 1);
                }
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
        lua_getfield(L, cache, "pending"); pending = lua_gettop(L);
        lua_pushnil(L);
        while (lua_next(L, pending)) {
            lua_newtable(L); item = lua_gettop(L);
            lua_pushvalue(L, cache); lua_setfield(L, item, "cache");
            lua_pushvalue(L, -3); lua_setfield(L, item, "key");
            lua_pushvalue(L, -2); lua_setfield(L, item, "entry");
            lua_rawseti(L, snapshot, ++count); lua_pop(L, 1);
        }
        lua_pop(L, 2);
    }
    lua_pushvalue(L, snapshot); lua_setfield(L, state, "snapshot");
    return ac_background_next(L, state);
}

static int
ac_cache_worker(lua_State *L)
{
    lua_getfield(L, lua_upvalueindex(1), "started");
    if (lua_toboolean(L, -1)) return 0;
    lua_pop(L, 1);
    ac_ngx(L, "timer", "every"); lua_pushinteger(L, 1);
    lua_pushvalue(L, lua_upvalueindex(1)); ac_bind(L, ac_background_start, 1);
    lua_call(L, 2, 2);
    if (!lua_toboolean(L, -2)) return luaL_error(L, "native cache timer failed: %s", lua_tostring(L, -1));
    lua_pushboolean(L, 1); lua_setfield(L, lua_upvalueindex(1), "started"); return 0;
}

static int
ac_cache_open(lua_State *L)
{
    int module, pool;
    lua_newtable(L); module = lua_gettop(L);
    lua_newtable(L); pool = lua_gettop(L);
    lua_newtable(L); lua_pushliteral(L, "v"); lua_setfield(L, -2, "__mode"); lua_setmetatable(L, pool);
    lua_pushvalue(L, pool); lua_pushcclosure(L, ac_cache_new, 1); lua_setfield(L, module, "new");
    lua_pushvalue(L, pool); lua_pushcclosure(L, ac_cache_new, 1); lua_pushnil(L); lua_call(L, 1, 1); lua_setfield(L, module, "global");
    lua_pushvalue(L, pool); lua_pushcclosure(L, ac_cache_worker, 1); lua_setfield(L, module, "init_worker");
    ac_bind(L, ac_cache_plugin_start, 0); lua_setfield(L, module, "plugin_ctx");
    lua_pushcfunction(L, ac_cache_plugin_id); lua_setfield(L, module, "plugin_ctx_id");
    lua_pop(L, 1); return 1;
}

ngx_int_t
ac_cache_register(ngx_conf_t *cf)
{
    if (ngx_http_lua_add_package_preload(cf, "apisix.core.lrucache", ac_cache_open) != NGX_OK) return NGX_ERROR;
    return ac_utils_register(cf);
}
