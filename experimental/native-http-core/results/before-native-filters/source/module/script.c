/* SPDX-License-Identifier: Apache-2.0 */
#include "engine.h"

static int
ac_script_loaded(lua_State *L)
{
    if (!lua_toboolean(L, 1)) { lua_pushvalue(L, 2); return lua_error(L); }
    lua_pushvalue(L, 2); lua_setfield(L, lua_upvalueindex(1), "script_obj");
    lua_pushnil(L); return 1;
}

static int
ac_script_load(lua_State *L)
{
    size_t length;
    const char *script, *name;
    lua_settop(L, 2);
    lua_getfield(L, 1, "value"); lua_getfield(L, -1, "script");
    script = lua_tolstring(L, -1, &length);
    if (!script || !length) return luaL_error(L, "missing valid script");
    lua_pushliteral(L, "route#"); lua_getfield(L, -3, "id"); lua_concat(L, 2);
    name = lua_tostring(L, -1);
    if (luaL_loadbuffer(L, script, length, name) != 0) {
        return luaL_error(L, "failed to load script: %s script: %s", lua_tostring(L, -1), script);
    }
    lua_pushvalue(L, 2); lua_pushcclosure(L, ac_script_loaded, 1); lua_insert(L, -2);
    lua_newtable(L); lua_pushinteger(L, 0); lua_setfield(L, -2, "n"); return 3;
}

static int
ac_script_ran(lua_State *L)
{
    if (!lua_toboolean(L, 1)) { lua_pushvalue(L, 2); return lua_error(L); }
    lua_pushnil(L); lua_pushvalue(L, lua_upvalueindex(1)); return 2;
}

static int
ac_script_run(lua_State *L)
{
    lua_settop(L, 2);
    if (lua_istable(L, 2)) lua_getfield(L, 2, "script_obj"); else lua_pushnil(L);
    if (!lua_toboolean(L, -1)) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "missing loaded script object");
        lua_pushnil(L); lua_pushvalue(L, 2); return 2;
    }
    lua_pushvalue(L, 1); lua_gettable(L, -2);
    if (!lua_toboolean(L, -1)) { lua_pushnil(L); lua_pushvalue(L, 2); return 2; }
    lua_pushvalue(L, 2); lua_pushcclosure(L, ac_script_ran, 1); lua_insert(L, -2);
    lua_newtable(L); lua_pushvalue(L, 2); lua_rawseti(L, -2, 1);
    lua_pushinteger(L, 1); lua_setfield(L, -2, "n"); return 3;
}

int
ac_script_next(lua_State *L, ac_request_t *ctx)
{
    ac_push(L, ctx->route->value_ref);
    ctx->script_route = ac_bool_field(L, -1, "script"); lua_pop(L, 1);
    if (!ctx->script_route) return -1;
    if (ctx->script_dispatched) return 0;
    ctx->script_dispatched = 1;
    if (ctx->phase == AC_REWRITE && ctx->script_loaded) return 0;
    ac_route_context(L, ctx, 0);
    ac_require(L, "apisix.script");
    lua_getfield(L, -1, ctx->phase == AC_REWRITE ? "load" : "run"); lua_remove(L, -2);
    if (ctx->phase == AC_REWRITE) ac_push(L, ctx->route->wrapper_ref);
    else lua_pushstring(L, ac_phase_names[ctx->phase]);
    ac_push(L, ctx->table_ref);
    ctx->callback_pending = 1; ctx->script_pending = 1; return 3;
}

static int
ac_script_open(lua_State *L)
{
    lua_newtable(L);
    ac_bind(L, ac_script_load, 0); lua_setfield(L, -2, "load");
    ac_bind(L, ac_script_run, 0); lua_setfield(L, -2, "run"); return 1;
}

ngx_int_t
ac_script_register(ngx_conf_t *cf)
{
    return ngx_http_lua_add_package_preload(cf, "apisix.script", ac_script_open);
}
