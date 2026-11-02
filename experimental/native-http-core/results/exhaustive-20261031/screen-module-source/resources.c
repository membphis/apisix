/* SPDX-License-Identifier: Apache-2.0 */
#include "engine.h"

void
ac_resource(lua_State *L, ac_main_conf_t *main, const char *name)
{
    ac_push(L, main->store_ref);
    lua_getfield(L, -1, name);
    lua_remove(L, -2);
}

void
ac_resource_get(lua_State *L, ac_main_conf_t *main, const char *name, int key)
{
    int object;
    key = ac_abs(L, key);
    ac_resource(L, main, name);
    object = lua_gettop(L);
    if (lua_isnil(L, object)) return;
    lua_getfield(L, object, "values_hash");
    lua_pushvalue(L, key);
    lua_tolstring(L, -1, NULL);
    lua_rawget(L, -2);
    if (lua_isnil(L, -1)) {
        lua_replace(L, object);
        lua_settop(L, object);
        return;
    }
    lua_getfield(L, object, "values");
    lua_rawgeti(L, -1, (int) lua_tointeger(L, -2));
    lua_replace(L, object);
    lua_settop(L, object);
}

void
ac_plugin_parent(lua_State *L, int plugins, int parent)
{
    int conf, meta;
    plugins = ac_abs(L, plugins);
    parent = ac_abs(L, parent);
    if (lua_isnil(L, plugins)) return;
    lua_pushnil(L);
    while (lua_next(L, plugins)) {
        conf = lua_gettop(L);
        if (!lua_istable(L, conf)) luaL_error(L, "plugin configuration must be a table");
        lua_getfield(L, conf, "_meta");
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            lua_newtable(L);
            lua_pushvalue(L, -1);
            lua_setfield(L, conf, "_meta");
        }
        meta = lua_gettop(L);
        lua_getfield(L, meta, "parent");
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            lua_newtable(L);
            lua_getfield(L, parent, "key");
            lua_setfield(L, -2, "resource_key");
            lua_getglobal(L, "tostring");
            lua_getfield(L, parent, "modifiedIndex");
            lua_call(L, 1, 1);
            lua_setfield(L, -2, "resource_version");
            if (lua_getmetatable(L, meta)) {
                lua_pushvalue(L, -2);
                lua_setfield(L, -2, "parent");
                lua_pop(L, 1);
            } else {
                lua_newtable(L);
                lua_newtable(L);
                lua_pushvalue(L, -3);
                lua_setfield(L, -2, "parent");
                lua_setfield(L, -2, "__index");
                lua_setmetatable(L, meta);
            }
        }
        lua_pop(L, 3);
    }
}

static int
ac_object_get(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_settop(L, 2);
    lua_getfield(L, 1, "values_hash");
    lua_pushvalue(L, 2);
    lua_tolstring(L, -1, NULL);
    lua_rawget(L, -2);
    if (lua_isnil(L, -1)) return 1;
    lua_getfield(L, 1, "values");
    lua_rawgeti(L, -1, (int) lua_tointeger(L, -2));
    return 1;
}

static int
ac_object_close(lua_State *L)
{
    lua_pushboolean(L, 0);
    lua_setfield(L, 1, "running");
    return 0;
}

static int
ac_object_server(lua_State *L)
{
    lua_pushnil(L);
    return 1;
}

static void
ac_object_methods(lua_State *L, int object)
{
    object = ac_abs(L, object);
    lua_pushcfunction(L, ac_object_get);
    lua_setfield(L, object, "get");
    lua_pushcfunction(L, ac_object_close);
    lua_setfield(L, object, "close");
    lua_pushcfunction(L, ac_object_server);
    lua_setfield(L, object, "server_version");
    lua_pushboolean(L, 1);
    lua_setfield(L, object, "running");
}

void
ac_resources_prepare(lua_State *L, ac_main_conf_t *main, int config)
{
    static const char *kinds[] = {
        "routes", "services", "upstreams", "consumers", "consumer_groups",
        "plugin_configs", "plugin_metadata", "global_rules", "ssls", "stream_routes",
        "protos", "secrets", NULL
    };
    ngx_uint_t i, j;
    int store, object, items, values, hash, value, wrapper, position, replace;
    config = ac_abs(L, config);
    lua_getfield(L, config, "local_conf");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_newtable(L);
        lua_pushliteral(L, "data_plane");
        lua_setfield(L, -2, "role");
        lua_pushliteral(L, "json");
        lua_setfield(L, -2, "config_provider");
        lua_setfield(L, -2, "deployment");
        lua_newtable(L);
        lua_pushboolean(L, 0);
        lua_setfield(L, -2, "enable_admin");
        lua_setfield(L, -2, "apisix");
    }
    main->local_ref = ac_ref(L, -1);
    lua_pop(L, 1);
    lua_newtable(L);
    store = lua_gettop(L);
    main->store_ref = ac_ref(L, store);
    for (i = 0; kinds[i]; i++) {
        lua_newtable(L);
        object = lua_gettop(L);
        ac_object_methods(L, object);
        lua_pushstring(L, kinds[i]);
        lua_setfield(L, object, "key");
        lua_pushstring(L, kinds[i]);
        lua_pushliteral(L, "_conf_version");
        lua_concat(L, 2);
        lua_rawget(L, config);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            lua_pushinteger(L, 1);
        }
        lua_setfield(L, object, "conf_version");
        lua_getfield(L, config, kinds[i]);
        items = lua_gettop(L);
        lua_newtable(L);
        values = lua_gettop(L);
        lua_newtable(L);
        hash = lua_gettop(L);
        if (lua_istable(L, items)) {
            for (j = 1; j <= lua_objlen(L, items); j++) {
                lua_rawgeti(L, items, (int) j);
                value = lua_gettop(L);
                luaL_checktype(L, value, LUA_TTABLE);
                lua_getfield(L, value, "id");
                if (lua_isnil(L, -1)) {
                    lua_pop(L, 1);
                    lua_getfield(L, value, "username");
                    if (lua_isnil(L, -1)) { lua_pop(L, 1); lua_pushfstring(L, "arr_%d", (int) j); }
                }
                lua_tolstring(L, -1, NULL);
                lua_pushvalue(L, -1); lua_setfield(L, value, "id");
                lua_pushvalue(L, -1);
                lua_rawget(L, hash);
                position = lua_isnil(L, -1) ? (int) lua_objlen(L, values) + 1 : (int) lua_tointeger(L, -1);
                replace = 1;
                if (!lua_isnil(L, -1)) {
                    lua_rawgeti(L, values, position); lua_getfield(L, -1, "modifiedIndex");
                    lua_getfield(L, value, "modifiedIndex");
                    replace = lua_isnil(L, -1) || !lua_equal(L, -1, -2);
                    lua_pop(L, 3);
                }
                lua_pop(L, 1);
                lua_pushinteger(L, position);
                lua_rawset(L, hash);
                lua_newtable(L);
                wrapper = lua_gettop(L);
                lua_pushvalue(L, value);
                lua_setfield(L, wrapper, "value");
                lua_getfield(L, value, "modifiedIndex");
                if (lua_isnil(L, -1)) {
                    lua_pop(L, 1);
                    lua_getfield(L, object, "conf_version");
                }
                lua_pushvalue(L, -1);
                lua_setfield(L, wrapper, "orig_modifiedIndex");
                lua_setfield(L, wrapper, "modifiedIndex");
                lua_pushliteral(L, "/");
                lua_pushstring(L, kinds[i]);
                lua_pushliteral(L, "/");
                lua_getfield(L, value, "id");
                lua_concat(L, 4);
                lua_setfield(L, wrapper, "key");
                lua_newtable(L);
                lua_setfield(L, wrapper, "clean_handlers");
                lua_getfield(L, value, "plugins");
                ac_plugin_parent(L, -1, wrapper);
                lua_pop(L, 1);
                if (i == 0 || i == 1 || i == 2) {
                    lua_pushcfunction(L, ac_filter_upstream);
                    if (i == 2) lua_pushvalue(L, value);
                    else lua_getfield(L, value, "upstream");
                    lua_pushvalue(L, wrapper); lua_call(L, 2, 0);
                }
                if (replace) lua_rawseti(L, values, position); else lua_pop(L, 1);
                lua_pop(L, 1);
            }
        }
        lua_pushvalue(L, values);
        lua_setfield(L, object, "values");
        lua_pushvalue(L, hash);
        lua_setfield(L, object, "values_hash");
        lua_pushvalue(L, object);
        lua_setfield(L, store, kinds[i]);
        lua_pop(L, 4);
    }
    lua_pop(L, 1);
}

static int
ac_local_conf(lua_State *L)
{
    ac_push(L, ac_main()->local_ref);
    return 1;
}

static int
ac_config_fetch(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    ac_resource(L, ac_main(), name[0] == '/' ? name + 1 : name);
    return 1;
}

static int
ac_config_new(lua_State *L)
{
    int object, source, filter, values, i;
    const char *name;
    lua_settop(L, 2);
    if (lua_isnil(L, 1)) {
        lua_newtable(L);
        ac_object_methods(L, -1);
        return 1;
    }
    name = luaL_checkstring(L, 1);
    ac_resource(L, ac_main(), name[0] == '/' ? name + 1 : name);
    source = lua_gettop(L);
    if (!lua_istable(L, source)) {
        lua_newtable(L);
        ac_object_methods(L, -1);
        lua_newtable(L);
        lua_setfield(L, -2, "values");
        lua_newtable(L);
        lua_setfield(L, -2, "values_hash");
        lua_pushinteger(L, 0);
        lua_setfield(L, -2, "conf_version");
        lua_pushstring(L, name[0] == '/' ? name + 1 : name);
        lua_setfield(L, -2, "key");
        lua_replace(L, source);
    }
    ac_require(L, "table.clone");
    lua_pushvalue(L, source);
    lua_call(L, 1, 1);
    object = lua_gettop(L);
    if (lua_istable(L, 2)) {
        lua_pushnil(L);
        while (lua_next(L, 2)) {
            lua_pushvalue(L, -2);
            lua_pushvalue(L, -2);
            lua_rawset(L, object);
            lua_pop(L, 1);
        }
        lua_getfield(L, 2, "filter");
        filter = lua_gettop(L);
        if (lua_isfunction(L, filter)) {
            lua_getfield(L, object, "values");
            values = lua_gettop(L);
            for (i = 1; i <= (int) lua_objlen(L, values); i++) {
                lua_pushvalue(L, filter);
                lua_rawgeti(L, values, i);
                lua_call(L, 1, 0);
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
    if (lua_istable(L, 2) && ac_bool_field(L, 2, "automatic")) {
        ac_main_conf_t *main = ac_main();
        if (main->readers_ref == LUA_NOREF) {
            lua_newtable(L); lua_newtable(L); lua_pushliteral(L, "k"); lua_setfield(L, -2, "__mode");
            lua_setmetatable(L, -2); main->readers_ref = luaL_ref(L, LUA_REGISTRYINDEX);
        }
        ac_push(L, main->readers_ref); lua_pushvalue(L, object);
        lua_pushstring(L, name[0] == '/' ? name + 1 : name); lua_rawset(L, -3); lua_pop(L, 1);
    }
    lua_pushvalue(L, object);
    return 1;
}

void
ac_snapshot_readers(lua_State *L, ac_main_conf_t *main, ngx_flag_t publish)
{
    static const char *fields[] = {"values", "values_hash", "conf_version", NULL};
    int readers, reader, object, filter, values, i, field;
    if (ac_main()->readers_ref == LUA_NOREF) return;
    ac_push(L, ac_main()->readers_ref); readers = lua_gettop(L);
    lua_pushnil(L);
    while (lua_next(L, readers)) {
        reader = lua_gettop(L) - 1;
        if (!ac_bool_field(L, reader, "running")) { lua_pop(L, 1); continue; }
        ac_resource(L, main, lua_tostring(L, -1)); object = lua_gettop(L);
        if (lua_istable(L, object)) {
            if (publish) {
                for (field = 0; fields[field]; field++) {
                    lua_getfield(L, object, fields[field]); lua_setfield(L, reader, fields[field]);
                }
            } else {
                lua_getfield(L, reader, "filter"); filter = lua_gettop(L);
                if (lua_isfunction(L, filter)) {
                    lua_getfield(L, object, "values"); values = lua_gettop(L);
                    for (i = 1; i <= (int) lua_objlen(L, values); i++) {
                        lua_pushvalue(L, filter); lua_rawgeti(L, values, i); lua_call(L, 1, 0);
                    }
                    lua_pop(L, 1);
                }
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 2);
    }
    lua_pop(L, 1);
}

static int
ac_config_open(lua_State *L)
{
    static const luaL_Reg functions[] = {
        {"local_conf", ac_local_conf}, {"new", ac_config_new},
        {"fetch_created_obj", ac_config_fetch}, {"get", ac_object_get},
        {"close", ac_object_close}, {"server_version", ac_object_server}, {NULL, NULL}
    };
    lua_newtable(L);
    luaL_register(L, NULL, functions);
    lua_pushliteral(L, "json");
    lua_setfield(L, -2, "type");
    return 1;
}

static int
ac_values(lua_State *L)
{
    ac_resource(L, ac_main(), lua_tostring(L, lua_upvalueindex(1)));
    lua_getfield(L, -1, "values");
    lua_getfield(L, -2, "conf_version");
    return 2;
}

static int
ac_get(lua_State *L)
{
    ac_resource_get(L, ac_main(), lua_tostring(L, lua_upvalueindex(1)), 1);
    return 1;
}

static int
ac_pre_index(lua_State *L)
{
    lua_pushinteger(L, 0);
    return 1;
}

static int
ac_resource_open(lua_State *L)
{
    const char *module = luaL_checkstring(L, 1), *kind, *method;
    if (ngx_strcmp(module, "apisix.http.service") == 0) { kind = method = "services"; }
    else if (ngx_strcmp(module, "apisix.http.route") == 0) { kind = method = "routes"; }
    else if (ngx_strcmp(module, "apisix.global_rules") == 0) { kind = method = "global_rules"; }
    else if (ngx_strcmp(module, "apisix.plugin_config") == 0) { kind = method = "plugin_configs"; }
    else if (ngx_strcmp(module, "apisix.consumer_group") == 0) { kind = method = "consumer_groups"; }
    else if (ngx_strcmp(module, "apisix.upstream") == 0) { kind = method = "upstreams"; }
    else { return luaL_error(L, "unknown native resource module"); }
    lua_newtable(L);
    lua_pushstring(L, kind);
    lua_pushcclosure(L, ac_values, 1);
    lua_setfield(L, -2, method);
    lua_pushstring(L, kind);
    lua_pushcclosure(L, ac_get, 1);
    lua_setfield(L, -2, "get");
    if (ngx_strcmp(kind, "global_rules") == 0) {
        lua_pushcfunction(L, ac_pre_index);
        lua_setfield(L, -2, "get_pre_index");
    }
    if (ngx_strcmp(kind, "upstreams") == 0) ac_upstream_api(L, -1);
    if (ngx_strcmp(kind, "plugin_configs") == 0) {
        lua_pushcfunction(L, ac_merge_plugin_config); lua_setfield(L, -2, "merge");
    }
    return 1;
}

static int
ac_router_open(lua_State *L)
{
    static const char *kinds[] = {"routes", "ssls", "stream_routes", NULL};
    static const char *methods[] = {"http_routes", "ssls", "stream_routes"};
    ngx_uint_t i;
    lua_newtable(L);
    for (i = 0; kinds[i]; i++) {
        lua_pushstring(L, kinds[i]);
        lua_pushcclosure(L, ac_values, 1);
        lua_setfield(L, -2, methods[i]);
    }
    return 1;
}

ngx_int_t
ac_resources_register(ngx_conf_t *cf)
{
    static const char *modules[] = {
        "apisix.http.service", "apisix.http.route", "apisix.global_rules",
        "apisix.plugin_config", "apisix.consumer_group", "apisix.upstream", NULL
    };
    ngx_uint_t i;
    for (i = 0; modules[i]; i++) {
        if (ngx_http_lua_add_package_preload(cf, modules[i], ac_resource_open) != NGX_OK) return NGX_ERROR;
    }
    if (ngx_http_lua_add_package_preload(cf, "apisix.core.config", ac_config_open) != NGX_OK
        || ngx_http_lua_add_package_preload(cf, "apisix.core.config_local", ac_config_open) != NGX_OK
        || ngx_http_lua_add_package_preload(cf, "apisix.core.config_json", ac_config_open) != NGX_OK
        || ngx_http_lua_add_package_preload(cf, "apisix.core.config_yaml", ac_config_open) != NGX_OK
        || ngx_http_lua_add_package_preload(cf, "apisix.router", ac_router_open) != NGX_OK) return NGX_ERROR;
    return ac_request_register(cf);
}
