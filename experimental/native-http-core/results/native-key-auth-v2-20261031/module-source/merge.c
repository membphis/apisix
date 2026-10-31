/* SPDX-License-Identifier: Apache-2.0 */
#include "engine.h"

static void
ac_field(lua_State *L, int dest, int source, const char *name)
{
    dest = ac_abs(L, dest); source = ac_abs(L, source);
    lua_getfield(L, source, name);
    lua_setfield(L, dest, name);
}

static void
ac_copy_table(lua_State *L, int source, ngx_flag_t deep)
{
    source = ac_abs(L, source);
    if (deep) {
        ac_require(L, "apisix.core.table");
        lua_getfield(L, -1, "deepcopy");
        lua_remove(L, -2);
    } else ac_require(L, "table.clone");
    lua_pushvalue(L, source);
    lua_call(L, 1, 1);
}

static void
ac_overlay(lua_State *L, int dest, int source, ngx_flag_t only_missing, ngx_flag_t consumer)
{
    int exists;
    dest = ac_abs(L, dest); source = ac_abs(L, source);
    if (!lua_istable(L, source)) return;
    lua_pushnil(L);
    while (lua_next(L, source)) {
        lua_pushvalue(L, -2);
        lua_rawget(L, dest);
        exists = !lua_isnil(L, -1);
        lua_pop(L, 1);
        if (!exists && consumer) {
            lua_pushboolean(L, 1);
            lua_setfield(L, -2, "_from_consumer");
        }
        if (!only_missing || !exists) {
            lua_pushvalue(L, -2);
            lua_pushvalue(L, -2);
            lua_rawset(L, dest);
        }
        lua_pop(L, 1);
    }
}

static int
ac_plugins(lua_State *L, int value)
{
    value = ac_abs(L, value);
    lua_getfield(L, value, "plugins");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, value, "plugins");
    }
    return lua_gettop(L);
}

static int
ac_merge_service(lua_State *L)
{
    int wrapper, service, route, plugins, i;
    static const char *optional[] = {"upstream_id", "script", "timeout", "hosts", "labels", NULL};
    lua_settop(L, 2);
    ac_copy_table(L, 1, 1);
    wrapper = lua_gettop(L);
    lua_getfield(L, wrapper, "value");
    service = lua_gettop(L);
    lua_getfield(L, 2, "value");
    route = lua_gettop(L);
    lua_getfield(L, service, "id"); lua_setfield(L, service, "service_id");
    ac_field(L, service, route, "id");
    ac_field(L, wrapper, 2, "modifiedIndex");
    lua_getfield(L, route, "plugins");
    if (lua_istable(L, -1)) {
        plugins = ac_plugins(L, service);
        ac_overlay(L, plugins, plugins - 1, 0, 0);
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    lua_getfield(L, route, "upstream");
    if (!lua_isnil(L, -1)) {
        lua_pushvalue(L, -1); lua_setfield(L, service, "upstream");
        lua_pushnil(L); lua_setfield(L, service, "upstream_id");
        ac_field(L, wrapper, 2, "has_domain");
    }
    lua_pop(L, 1);
    for (i = 0; optional[i]; i++) {
        lua_getfield(L, route, optional[i]);
        if (!lua_isnil(L, -1)) {
            lua_pushvalue(L, -1);
            lua_setfield(L, service, optional[i]);
        }
        lua_pop(L, 1);
    }
    ac_field(L, service, route, "name");
    lua_getfield(L, service, "hosts");
    if (lua_isnil(L, -1)) {
        lua_getfield(L, route, "host");
        if (!lua_isnil(L, -1)) {
            lua_pushvalue(L, -1); lua_setfield(L, service, "host");
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    lua_pushvalue(L, wrapper);
    return 1;
}

int
ac_merge_plugin_config(lua_State *L)
{
    int route, plugins;
    lua_settop(L, 2);
    lua_getfield(L, 1, "prev_plugin_config_ver");
    lua_getfield(L, 2, "modifiedIndex");
    if (lua_equal(L, -1, -2)) { lua_pushvalue(L, 1); return 1; }
    lua_pop(L, 2);
    lua_getfield(L, 1, "value");
    route = lua_gettop(L);
    lua_getfield(L, 1, "orig_plugins");
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        ac_plugins(L, route);
        lua_pushvalue(L, -1); lua_setfield(L, 1, "orig_plugins");
    }
    ac_copy_table(L, -1, 0);
    plugins = lua_gettop(L);
    lua_pushvalue(L, plugins); lua_setfield(L, route, "plugins");
    lua_getfield(L, 2, "value"); lua_getfield(L, -1, "plugins");
    ac_overlay(L, plugins, -1, 1, 0);
    lua_getfield(L, 1, "orig_modifiedIndex");
    lua_pushliteral(L, "#");
    lua_getfield(L, 2, "modifiedIndex");
    lua_concat(L, 3); lua_setfield(L, 1, "modifiedIndex");
    lua_getfield(L, 2, "modifiedIndex"); lua_setfield(L, 1, "prev_plugin_config_ver");
    lua_pushvalue(L, 1);
    return 1;
}

static int
ac_plugin_get(lua_State *L)
{
    ac_main_conf_t *main = ac_main();
    ac_plugin_t *plugins = main->plugins.elts;
    ngx_uint_t i;
    size_t len;
    const char *name = luaL_checklstring(L, 1, &len);
    for (i = 0; i < main->plugins.nelts; i++) {
        if (plugins[i].module_ref != LUA_NOREF && plugins[i].name.len == len && ngx_memcmp(plugins[i].name.data, name, len) == 0) {
            ac_push(L, plugins[i].module_ref);
            return 1;
        }
    }
    return 0;
}

static int
ac_plugin_attr(lua_State *L)
{
    luaL_checkstring(L, 1);
    ac_push(L, ac_main()->local_ref);
    lua_getfield(L, -1, "plugin_attr");
    if (lua_istable(L, -1)) {
        lua_pushvalue(L, 1); lua_rawget(L, -2);
        return 1;
    }
    return 0;
}

static int
ac_plugin_metadata(lua_State *L)
{
    ac_resource_get(L, ac_main(), "plugin_metadata", 1);
    return 1;
}

static int
ac_conf_version(lua_State *L)
{
    int conf;
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "_version");
    if (!lua_isnil(L, -1)) return 1;
    lua_pop(L, 1);
    conf = 1;
    ac_require(L, "dkjson");
    lua_getfield(L, -1, "encode");
    lua_pushvalue(L, conf);
    lua_call(L, 1, 1);
    ac_ngx(L, NULL, "crc32_short");
    lua_insert(L, -2);
    lua_call(L, 1, 1);
    lua_getglobal(L, "tostring"); lua_insert(L, -2); lua_call(L, 1, 1);
    lua_pushvalue(L, -1); lua_setfield(L, conf, "_version");
    return 1;
}

static int
ac_parent_api(lua_State *L)
{
    ac_plugin_parent(L, 1, 2);
    return 0;
}

static int
ac_skip_plugin(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "_skip_plugins");
    if (!lua_toboolean(L, -1)) {
        lua_pop(L, 1); lua_newtable(L); lua_pushvalue(L, -1); lua_setfield(L, 1, "_skip_plugins");
    }
    lua_pushvalue(L, 2); lua_pushboolean(L, 1); lua_rawset(L, -3); return 0;
}

void
ac_resolve_route(lua_State *L, ac_main_conf_t *main, ac_route_t *route, int wrapper)
{
    int value, merged;
    wrapper = ac_abs(L, wrapper);
    lua_pushvalue(L, wrapper);
    merged = lua_gettop(L);
    lua_getfield(L, merged, "value");
    value = lua_gettop(L);
    lua_getfield(L, value, "plugin_config_id");
    if (!lua_isnil(L, -1)) {
        ac_resource_get(L, main, "plugin_configs", -1);
        if (lua_isnil(L, -1)) route->resolve_status = 503;
        else {
            lua_pushcfunction(L, ac_merge_plugin_config);
            lua_pushvalue(L, merged); lua_pushvalue(L, -3);
            lua_call(L, 2, 1); lua_replace(L, merged);
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    lua_getfield(L, value, "service_id");
    if (!lua_isnil(L, -1)) {
        ac_resource_get(L, main, "services", -1);
        if (lua_isnil(L, -1)) route->resolve_status = 404;
        else {
            route->service_ref = ac_ref(L, -1);
            lua_pushcfunction(L, ac_merge_service);
            lua_pushvalue(L, -2); lua_pushvalue(L, merged);
            lua_call(L, 2, 1); lua_replace(L, merged);
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 2);
    route->wrapper_ref = ac_ref(L, merged);
    lua_getfield(L, merged, "value");
    route->value_ref = ac_ref(L, -1);
    lua_pop(L, 2);
}

void
ac_route_context_at(lua_State *L, ac_request_t *ctx, ngx_flag_t global, int table)
{
    static const char *names[] = {"conf_type", "conf_id", "conf_version", "route_name", "service_id", "service_name"};
    ngx_uint_t i;
    int route, service;
    table = ac_abs(L, table);
    if (!global && ctx->metadata) {
        for (i = 0; i < ctx->metadata->count; i++) {
            ac_push(L, ctx->metadata->fields[i]); lua_setfield(L, table, names[i]);
        }
        return;
    }
    if (global) {
        lua_pushliteral(L, "global_rule"); lua_setfield(L, table, "conf_type");
        ac_resource(L, ctx->main, "global_rules");
        lua_getfield(L, -1, "conf_version"); lua_setfield(L, table, "conf_version");
        lua_pop(L, 1);
        lua_pushinteger(L, 1); lua_setfield(L, table, "conf_id");
    } else if (ctx->route) {
        ac_push(L, ctx->route->wrapper_ref);
        route = lua_gettop(L);
        lua_getfield(L, route, "value");
        lua_getfield(L, -1, "id"); lua_setfield(L, table, "conf_id");
        lua_getfield(L, -1, "name"); lua_setfield(L, table, "route_name");
        lua_pop(L, 1);
        if (ctx->route->service_ref != LUA_NOREF) {
            ac_push(L, ctx->route->service_ref);
            service = lua_gettop(L);
            lua_getfield(L, route, "modifiedIndex"); lua_pushliteral(L, "&");
            lua_getfield(L, service, "modifiedIndex"); lua_concat(L, 3);
            lua_setfield(L, table, "conf_version");
            lua_getfield(L, table, "conf_id"); lua_pushliteral(L, "&");
            lua_getfield(L, service, "value"); lua_getfield(L, -1, "id"); lua_remove(L, -2);
            lua_pushvalue(L, -1); lua_setfield(L, table, "service_id");
            lua_concat(L, 3); lua_setfield(L, table, "conf_id");
            lua_getfield(L, service, "value"); lua_getfield(L, -1, "name");
            lua_setfield(L, table, "service_name"); lua_pop(L, 1);
            lua_pushliteral(L, "route&service"); lua_setfield(L, table, "conf_type");
            lua_pop(L, 1);
        } else {
            lua_pushliteral(L, "route"); lua_setfield(L, table, "conf_type");
            lua_getfield(L, route, "modifiedIndex"); lua_setfield(L, table, "conf_version");
        }
        lua_pop(L, 1);
        if (ctx->consumer_processed) {
            lua_getfield(L, table, "consumer");
            if (lua_istable(L, -1)) {
                int consumer = lua_gettop(L);
                lua_getfield(L, table, "conf_type"); lua_pushliteral(L, "&consumer"); lua_concat(L, 2);
                lua_setfield(L, table, "conf_type");
                lua_getfield(L, table, "conf_version"); lua_pushliteral(L, "&");
                lua_getfield(L, table, "consumer_ver"); lua_concat(L, 3); lua_setfield(L, table, "conf_version");
                lua_getfield(L, table, "conf_id"); lua_pushliteral(L, "&");
                lua_getfield(L, table, "consumer_name"); lua_concat(L, 3); lua_setfield(L, table, "conf_id");
                lua_getfield(L, consumer, "group_id");
                if (!lua_isnil(L, -1)) {
                    ac_resource_get(L, ctx->main, "consumer_groups", -1);
                    if (lua_istable(L, -1)) {
                        int group = lua_gettop(L);
                        lua_getfield(L, table, "conf_type"); lua_pushliteral(L, "&consumer_group"); lua_concat(L, 2);
                        lua_setfield(L, table, "conf_type");
                        lua_getfield(L, table, "conf_version"); lua_pushliteral(L, "&");
                        lua_getfield(L, group, "modifiedIndex"); lua_concat(L, 3); lua_setfield(L, table, "conf_version");
                        lua_getfield(L, table, "conf_id"); lua_pushliteral(L, "&");
                        lua_getfield(L, consumer, "group_id"); lua_concat(L, 3); lua_setfield(L, table, "conf_id");
                    }
                    lua_pop(L, 1);
                }
                lua_pop(L, 1);
            }
            lua_pop(L, 1);
        }
    } else {
        lua_pushnil(L); lua_setfield(L, table, "conf_type");
        lua_pushnil(L); lua_setfield(L, table, "conf_version");
        lua_pushnil(L); lua_setfield(L, table, "conf_id");
    }
    if (!global && ctx->route && ac_main()->building) {
        ac_metadata_t *metadata = ngx_palloc(ctx->main->pool, sizeof(*metadata));
        if (!metadata) luaL_error(L, "native context metadata allocation failed");
        metadata->count = ctx->route->service_ref == LUA_NOREF ? 4 : 6;
        for (i = 0; i < metadata->count; i++) {
            lua_getfield(L, table, names[i]); metadata->fields[i] = ac_ref(L, -1); lua_pop(L, 1);
        }
        ctx->metadata = metadata;
    }
}

void
ac_route_context(lua_State *L, ac_request_t *ctx, ngx_flag_t global)
{
    ac_push(L, ctx->table_ref);
    ac_route_context_at(L, ctx, global, -1);
    lua_pop(L, 1);
}

void
ac_prepare_route_metadata(lua_State *L, ac_main_conf_t *main, ac_route_t *route)
{
    ac_request_t context;
    ngx_memzero(&context, sizeof(context));
    context.main = main; context.route = route;
    lua_createtable(L, 0, 6); context.table_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    ac_route_context(L, &context, 0); route->metadata = context.metadata;
    luaL_unref(L, LUA_REGISTRYINDEX, context.table_ref);
}

static int
ac_plugin_open(lua_State *L)
{
    static const luaL_Reg functions[] = {
        {"get", ac_plugin_get}, {"plugin_attr", ac_plugin_attr}, {"plugin_metadata", ac_plugin_metadata},
        {"conf_version", ac_conf_version}, {"merge_service_route", ac_merge_service},
        {"skip_plugin", ac_skip_plugin},
        {"set_plugins_meta_parent", ac_parent_api}, {NULL, NULL}
    };
    lua_newtable(L);
    luaL_register(L, NULL, functions);
    ac_resource(L, ac_main(), "plugin_metadata"); lua_setfield(L, -2, "plugin_metadatas");
    return 1;
}

ngx_int_t
ac_merge_register(ngx_conf_t *cf)
{
    if (ngx_http_lua_add_package_preload(cf, "apisix.plugin", ac_plugin_open) != NGX_OK) return NGX_ERROR;
    return ac_response_register(cf);
}
