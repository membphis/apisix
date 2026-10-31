/* SPDX-License-Identifier: Apache-2.0 */
#include "engine.h"

static int
ac_forward(lua_State *L)
{
    int count = lua_gettop(L);
    if (!lua_toboolean(L, 1)) { lua_pushvalue(L, 2); return lua_error(L); }
    lua_pushnil(L); lua_replace(L, 1); return count;
}

static int
ac_event_register(lua_State *L)
{
    lua_settop(L, 2); lua_pushvalue(L, 1); lua_pushvalue(L, 2); lua_rawset(L, lua_upvalueindex(1)); return 0;
}

static int
ac_discard(lua_State *L)
{
    if (!lua_toboolean(L, 1)) { lua_pushvalue(L, 2); return lua_error(L); }
    lua_pushnil(L); return 1;
}

static int
ac_event_unregister(lua_State *L)
{
    lua_pushvalue(L, 1); lua_pushnil(L); lua_rawset(L, lua_upvalueindex(1)); return 0;
}

static int
ac_event_start(lua_State *L)
{
    int n = lua_gettop(L), i;
    lua_pushvalue(L, 1); lua_rawget(L, lua_upvalueindex(1));
    if (!lua_toboolean(L, -1)) { lua_pushnil(L); return 1; }
    lua_pushcfunction(L, ac_discard); lua_insert(L, -2);
    lua_createtable(L, n - 1, 1);
    for (i = 2; i <= n; i++) { lua_pushvalue(L, i); lua_rawseti(L, -2, i - 1); }
    lua_pushinteger(L, n - 1); lua_setfield(L, -2, "n"); return 3;
}

static int
ac_event_open(lua_State *L)
{
    int module, handlers;
    lua_newtable(L); module = lua_gettop(L); lua_newtable(L); handlers = lua_gettop(L);
    lua_pushvalue(L, handlers); lua_pushcclosure(L, ac_event_register, 1); lua_setfield(L, module, "register");
    lua_pushvalue(L, handlers); lua_pushcclosure(L, ac_event_unregister, 1); lua_setfield(L, module, "unregister");
    lua_pushvalue(L, handlers); ac_bind(L, ac_event_start, 1); lua_setfield(L, module, "push");
    lua_newtable(L); lua_pushinteger(L, 1); lua_setfield(L, -2, "BUILD_ROUTER"); lua_setfield(L, module, "CONST");
    lua_pop(L, 1); return 1;
}

static lua_Number
ac_timer_now(void)
{
    ngx_time_t *time = ngx_timeofday(); return time->sec + time->msec / 1000.0;
}

static void
ac_timer_error(lua_State *L, int timer, int error)
{
    timer = ac_abs(L, timer); error = ac_abs(L, error);
    ac_ngx(L, NULL, "log"); lua_pushinteger(L, NGX_LOG_ERR);
    lua_pushliteral(L, "failed to run the timer: "); lua_getfield(L, timer, "name");
    lua_pushliteral(L, " err: "); lua_pushvalue(L, error); lua_call(L, 5, 0);
}

static int ac_timer_callback_packet(lua_State *L, int timer);

static int
ac_timer_woke(lua_State *L)
{
    int timer = lua_upvalueindex(1);
    lua_Number ttl, start;
    if (!lua_toboolean(L, 1)) { ac_timer_error(L, timer, 2); goto finish; }
    ngx_time_update();
    lua_getfield(L, timer, "each_ttl"); ttl = lua_tonumber(L, -1); lua_pop(L, 1);
    lua_getfield(L, timer, "start_time"); start = lua_tonumber(L, -1); lua_pop(L, 1);
    if (ttl > 0 && ac_timer_now() < start + ttl) return ac_timer_callback_packet(L, timer);
finish:
    lua_pushboolean(L, 0); lua_setfield(L, timer, "running"); lua_pushnil(L); return 1;
}

static int
ac_timer_called(lua_State *L)
{
    int timer = lua_upvalueindex(1);
    lua_Number sleep;
    if (!lua_toboolean(L, 1)) ac_timer_error(L, timer, 2);
    lua_getfield(L, timer, lua_toboolean(L, 1) ? "sleep_succ" : "sleep_fail");
    sleep = lua_tonumber(L, -1); lua_pop(L, 1);
    if (sleep > 0) {
        lua_pushvalue(L, timer); lua_pushcclosure(L, ac_timer_woke, 1); ac_ngx(L, NULL, "sleep");
        lua_createtable(L, 1, 1); lua_pushnumber(L, sleep); lua_rawseti(L, -2, 1);
        lua_pushinteger(L, 1); lua_setfield(L, -2, "n"); return 3;
    }
    lua_pushboolean(L, 1); lua_replace(L, 1); return ac_timer_woke(L);
}

static int
ac_timer_callback_packet(lua_State *L, int timer)
{
    timer = ac_abs(L, timer);
    lua_pushvalue(L, timer); lua_pushcclosure(L, ac_timer_called, 1);
    lua_getfield(L, timer, "callback_fun"); lua_newtable(L); lua_pushinteger(L, 0); lua_setfield(L, -2, "n"); return 3;
}

static int
ac_timer_start(lua_State *L)
{
    int timer = 2;
    if (lua_toboolean(L, 1) || ac_bool_field(L, timer, "running")) { lua_pushnil(L); return 1; }
    lua_pushboolean(L, 1); lua_setfield(L, timer, "running");
    lua_pushnumber(L, ac_timer_now()); lua_setfield(L, timer, "start_time");
    return ac_timer_callback_packet(L, timer);
}

static int
ac_timer_new(lua_State *L)
{
    static struct { const char *name; lua_Number value; } fields[] = {
        {"each_ttl", 1}, {"sleep_succ", 1}, {"sleep_fail", 5}, {NULL, 0}
    };
    int timer, i, callback;
    if (!lua_toboolean(L, 1)) { lua_pushnil(L); lua_pushliteral(L, "missing argument: name"); return 2; }
    if (!lua_toboolean(L, 2)) { lua_pushnil(L); lua_pushliteral(L, "missing argument: callback_fun"); return 2; }
    lua_settop(L, 3);
    if (!lua_toboolean(L, 3)) { lua_newtable(L); lua_replace(L, 3); }
    lua_newtable(L); timer = lua_gettop(L);
    lua_pushvalue(L, 1); lua_setfield(L, timer, "name");
    lua_pushvalue(L, 2); lua_setfield(L, timer, "callback_fun");
    for (i = 0; fields[i].name; i++) {
        lua_getfield(L, 3, fields[i].name);
        if (!lua_toboolean(L, -1)) { lua_pop(L, 1); lua_pushnumber(L, fields[i].value); }
        lua_setfield(L, timer, fields[i].name);
    }
    lua_pushinteger(L, 0); lua_setfield(L, timer, "start_time");
    lua_pushboolean(L, 0); lua_setfield(L, timer, "running");
    ac_bind(L, ac_timer_start, 0); callback = lua_gettop(L);
    ac_ngx(L, "timer", "every"); lua_getfield(L, 3, "check_interval");
    if (!lua_toboolean(L, -1)) { lua_pop(L, 1); lua_pushinteger(L, 1); }
    lua_pushvalue(L, callback); lua_pushvalue(L, timer); lua_call(L, 3, 2);
    if (!lua_toboolean(L, -2)) return 2;
    lua_pop(L, 2);
    ac_ngx(L, "timer", "at"); lua_pushinteger(L, 0); lua_pushvalue(L, callback); lua_pushvalue(L, timer); lua_call(L, 3, 2);
    if (!lua_toboolean(L, -2)) return 2;
    lua_pushvalue(L, timer); return 1;
}

static int
ac_timer_open(lua_State *L)
{
    lua_newtable(L); lua_pushcfunction(L, ac_timer_new); lua_setfield(L, -2, "new"); return 1;
}

static int
ac_events_init(lua_State *L)
{
    int module = lua_upvalueindex(1), opts, events;
    if (ac_bool_field(L, module, "inited")) return 0;
    lua_newtable(L); opts = lua_gettop(L);
    lua_pushinteger(L, 5); lua_setfield(L, opts, "unique_timeout"); lua_pushinteger(L, 0); lua_setfield(L, opts, "broker_id");
    lua_pushliteral(L, "unix:"); ac_ngx(L, "config", "prefix"); lua_call(L, 0, 1);
    lua_pushliteral(L, "logs/worker_events.sock"); lua_concat(L, 3); lua_setfield(L, opts, "listening");
    ac_require(L, "resty.events.compat"); events = lua_gettop(L);
    lua_getfield(L, events, "configure"); lua_pushvalue(L, opts); lua_call(L, 1, 2);
    if (!lua_toboolean(L, -2)) return luaL_error(L, "native events initialization failed: %s", lua_tostring(L, -1));
    lua_pop(L, 2); lua_pushvalue(L, events); lua_setfield(L, module, "worker_events");
    lua_pushboolean(L, 1); lua_setfield(L, module, "inited"); return 0;
}

static int
ac_events_start(lua_State *L)
{
    int n = lua_gettop(L), i;
    lua_pushcfunction(L, ac_forward); lua_getfield(L, 1, "worker_events");
    lua_getfield(L, -1, lua_tostring(L, lua_upvalueindex(1))); lua_remove(L, -2);
    lua_createtable(L, n - 1, 1);
    for (i = 2; i <= n; i++) { lua_pushvalue(L, i); lua_rawseti(L, -2, i - 1); }
    lua_pushinteger(L, n - 1); lua_setfield(L, -2, "n"); return 3;
}

static int
ac_event_unknown(lua_State *L)
{
    lua_getglobal(L, "tostring"); lua_pushvalue(L, 2); lua_call(L, 1, 1);
    return luaL_error(L, "event '%s' is an unknown event", lua_tostring(L, -1));
}

static int
ac_event_list(lua_State *L)
{
    int i, n = lua_gettop(L), list;
    lua_newtable(L); list = lua_gettop(L); lua_pushvalue(L, 2); lua_setfield(L, list, "_source");
    for (i = 3; i <= n; i++) { lua_pushvalue(L, i); lua_pushvalue(L, i); lua_rawset(L, list); }
    lua_newtable(L); lua_pushcfunction(L, ac_event_unknown); lua_setfield(L, -2, "__index"); lua_setmetatable(L, list); return 1;
}

static int
ac_events_open(lua_State *L)
{
    lua_newtable(L);
    lua_pushvalue(L, -1); lua_pushcclosure(L, ac_events_init, 1); lua_setfield(L, -2, "init_worker");
    lua_pushliteral(L, "register"); ac_bind(L, ac_events_start, 1); lua_setfield(L, -2, "register");
    lua_pushliteral(L, "post"); ac_bind(L, ac_events_start, 1); lua_setfield(L, -2, "post");
    lua_pushcfunction(L, ac_event_list); lua_setfield(L, -2, "event_list"); return 1;
}

ngx_int_t
ac_background_register(ngx_conf_t *cf)
{
    if (ngx_http_lua_add_package_preload(cf, "apisix.core.event", ac_event_open) != NGX_OK
        || ngx_http_lua_add_package_preload(cf, "apisix.core.timer", ac_timer_open) != NGX_OK
        || ngx_http_lua_add_package_preload(cf, "apisix.events", ac_events_open) != NGX_OK) return NGX_ERROR;
    return ac_platform_register(cf);
}
