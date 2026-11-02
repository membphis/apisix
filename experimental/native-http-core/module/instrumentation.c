/* SPDX-License-Identifier: Apache-2.0 */
#include "engine.h"
#include <math.h>

static void
ac_array(lua_State *L, int table)
{
    table = ac_abs(L, table);
    ac_require(L, "cjson.safe"); lua_getfield(L, -1, "array_mt"); lua_remove(L, -2); lua_setmetatable(L, table);
}

static void
ac_checker_nodes(lua_State *L, int key)
{
    key = ac_abs(L, key);
    ac_require(L, "resty.healthcheck"); lua_getfield(L, -1, "get_target_list");
    lua_pushliteral(L, "upstream#"); lua_pushvalue(L, key); lua_concat(L, 2);
    lua_pushliteral(L, "upstream-healthcheck"); lua_call(L, 2, 2);
    lua_pop(L, 1);
    if (lua_istable(L, -1)) ac_array(L, -1);
}

static void
ac_checker_type(lua_State *L, int info, int checks)
{
    info = ac_abs(L, info); checks = ac_abs(L, checks);
    lua_getfield(L, checks, "active");
    if (lua_istable(L, -1)) lua_getfield(L, -1, "type"); else lua_pushnil(L);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 2); lua_getfield(L, checks, "passive");
        if (lua_istable(L, -1)) lua_getfield(L, -1, "type"); else lua_pushnil(L);
    }
    lua_setfield(L, info, "type"); lua_pop(L, 1);
}

static int
ac_health_checks(lua_State *L)
{
    static const char *kinds[] = {"routes", "stream_routes", "services", "upstreams", NULL};
    int infos, values, wrapper, value, checks, info, configs, module, targets, target, i, j, n;
    lua_newtable(L); infos = lua_gettop(L); ac_array(L, infos);
    for (i = 0; kinds[i]; i++) {
        ac_resource(L, ac_main(), kinds[i]); lua_getfield(L, -1, "values"); values = lua_gettop(L);
        for (j = 1; j <= (int) lua_objlen(L, values); j++) {
            lua_rawgeti(L, values, j); wrapper = lua_gettop(L);
            lua_getfield(L, wrapper, "value"); value = lua_gettop(L);
            lua_getfield(L, value, "checks");
            if (lua_isnil(L, -1)) {
                lua_pop(L, 1); lua_getfield(L, value, "upstream");
                if (lua_istable(L, -1)) lua_getfield(L, -1, "checks"); else lua_pushnil(L);
                lua_remove(L, -2);
            }
            checks = lua_gettop(L);
            if (lua_istable(L, checks)) {
                lua_newtable(L); info = lua_gettop(L);
                lua_getfield(L, wrapper, "key"); lua_pushvalue(L, -1); lua_setfield(L, info, "name");
                ac_checker_nodes(L, -1); lua_setfield(L, info, "nodes"); lua_pop(L, 1);
                ac_checker_type(L, info, checks);
                lua_rawseti(L, infos, (int) lua_objlen(L, infos) + 1);
            }
            lua_pop(L, 1);
            lua_getfield(L, value, "plugins"); configs = lua_gettop(L);
            if (lua_istable(L, configs)) {
                lua_pushnil(L);
                while (lua_next(L, configs)) {
                    lua_getfield(L, -1, "_meta");
                    int disabled = lua_istable(L, -1) && ac_bool_field(L, -1, "disable"); lua_pop(L, 1);
                    if (!disabled) {
                        ac_require(L, "apisix.plugin"); lua_getfield(L, -1, "get");
                        lua_pushvalue(L, -4); lua_call(L, 1, 1); module = lua_gettop(L);
                        if (lua_istable(L, module)) {
                            lua_getfield(L, module, "list_healthcheck_targets");
                            if (lua_isfunction(L, -1)) {
                                lua_pushvalue(L, module - 2); lua_getfield(L, wrapper, "key"); lua_call(L, 2, 1);
                                targets = lua_gettop(L);
                                if (lua_istable(L, targets)) {
                                    for (n = 1; n <= (int) lua_objlen(L, targets); n++) {
                                        lua_rawgeti(L, targets, n); target = lua_gettop(L);
                                        lua_newtable(L); info = lua_gettop(L);
                                        lua_getfield(L, target, "resource_path"); lua_pushvalue(L, -1); lua_setfield(L, info, "name");
                                        ac_checker_nodes(L, -1); lua_setfield(L, info, "nodes"); lua_pop(L, 1);
                                        lua_getfield(L, target, "meta"); lua_setfield(L, info, "meta");
                                        lua_pushvalue(L, module - 3); lua_setfield(L, info, "plugin");
                                        lua_getfield(L, target, "checks"); ac_checker_type(L, info, -1); lua_pop(L, 1);
                                        lua_rawseti(L, infos, (int) lua_objlen(L, infos) + 1); lua_pop(L, 1);
                                    }
                                }
                            }
                            lua_pop(L, 1);
                        }
                        lua_pop(L, 2);
                    }
                    lua_pop(L, 1);
                }
            }
            lua_pop(L, 3);
        }
        lua_pop(L, 2);
    }
    return 1;
}

static int
ac_control_open(lua_State *L)
{
    lua_newtable(L); lua_pushcfunction(L, ac_health_checks); lua_setfield(L, -2, "get_health_checkers"); return 1;
}

static int
ac_latency(lua_State *L)
{
    lua_Number latency, upstream, apisix;
    ngx_http_request_t *r = ngx_http_lua_get_request(L);
    ngx_time_t *now = ngx_timeofday();
    latency = ((lua_Number) now->sec - r->start_sec) * 1000 + now->msec - r->start_msec;
    lua_getfield(L, 1, "var"); lua_getfield(L, -1, "upstream_response_time");
    if (lua_isnil(L, -1)) { lua_pushnumber(L, latency); lua_pushnil(L); lua_pushnumber(L, latency); return 3; }
    upstream = luaL_checknumber(L, -1) * 1000;
    apisix = latency - upstream;
    lua_pushnumber(L, latency); lua_pushnumber(L, upstream); lua_pushnumber(L, apisix < 0 ? 0 : apisix); return 3;
}

static int
ac_log_util_open(lua_State *L)
{
    lua_newtable(L); lua_pushcfunction(L, ac_latency); lua_setfield(L, -2, "latency_details_in_ms"); return 1;
}

static int
ac_xrpc_metrics(lua_State *L)
{
    int config, protocols, i, protocol, metric, definition, hub;
    lua_settop(L, 1);
    ac_push(L, ac_main()->local_ref); config = lua_gettop(L);
    lua_getfield(L, config, "xrpc");
    if (!lua_istable(L, -1)) return 0;
    lua_getfield(L, -1, "protocols"); protocols = lua_gettop(L);
    if (!lua_istable(L, protocols)) return 0;
    for (i = 1; i <= (int) lua_objlen(L, protocols); i++) {
        lua_rawgeti(L, protocols, i); protocol = lua_gettop(L);
        lua_getglobal(L, "require"); lua_pushliteral(L, "apisix.stream.xrpc.protocols.");
        lua_getfield(L, protocol, "name"); lua_pushliteral(L, ".metrics"); lua_concat(L, 3);
        if (lua_pcall(L, 1, 1, 0) != 0) { lua_pop(L, 2); continue; }
        definition = lua_gettop(L); lua_newtable(L); hub = lua_gettop(L);
        lua_pushnil(L);
        while (lua_next(L, definition)) {
            metric = lua_gettop(L);
            lua_getfield(L, metric, "type"); lua_gettable(L, 1); lua_pushvalue(L, 1);
            lua_getfield(L, protocol, "name"); lua_pushliteral(L, "_"); lua_pushvalue(L, metric - 1); lua_concat(L, 3);
            lua_getfield(L, metric, "help"); lua_getfield(L, metric, "labels"); lua_getfield(L, metric, "buckets");
            lua_call(L, 5, 1); lua_pushvalue(L, metric - 1); lua_pushvalue(L, -2); lua_rawset(L, hub);
            lua_pop(L, 2);
        }
        lua_getfield(L, protocol, "name"); lua_pushvalue(L, hub); lua_rawset(L, lua_upvalueindex(1));
        lua_pop(L, 3);
    }
    return 0;
}

static int
ac_xrpc_open(lua_State *L)
{
    lua_newtable(L); lua_newtable(L); lua_pushcclosure(L, ac_xrpc_metrics, 1);
    lua_setfield(L, -2, "init_metrics"); return 1;
}

ngx_int_t
ac_instrumentation_register(ngx_conf_t *cf)
{
    if (ngx_http_lua_add_package_preload(cf, "apisix.control.v1", ac_control_open) != NGX_OK
        || ngx_http_lua_add_package_preload(cf, "apisix.utils.log-util", ac_log_util_open) != NGX_OK
        || ngx_http_lua_add_package_preload(cf, "apisix.stream.xrpc", ac_xrpc_open) != NGX_OK) return NGX_ERROR;
    return ac_resolver_register(cf);
}
