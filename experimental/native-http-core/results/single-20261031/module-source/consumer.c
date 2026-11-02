/* SPDX-License-Identifier: Apache-2.0 */
#include "engine.h"

static void
ac_consumer_clone(lua_State *L, int wrapper, int conf)
{
    int value, result;
    wrapper = ac_abs(L, wrapper); conf = ac_abs(L, conf);
    lua_getfield(L, wrapper, "value"); value = lua_gettop(L);
    ac_require(L, "table.clone"); lua_pushvalue(L, value); lua_call(L, 1, 1);
    result = lua_gettop(L);
    lua_getfield(L, wrapper, "modifiedIndex"); lua_setfield(L, result, "modifiedIndex");
    lua_getfield(L, value, "id"); lua_setfield(L, result, "consumer_name");
    lua_pushvalue(L, conf); lua_setfield(L, result, "auth_conf");
    lua_getfield(L, value, "labels");
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "custom_id"); lua_setfield(L, result, "custom_id");
    }
    lua_pop(L, 1);
    lua_remove(L, value);
}

void
ac_consumer_prepare(lua_State *L, ac_main_conf_t *main)
{
    int consumers, values, i, wrapper, value, configs, group, nodes, config, all;
    ngx_uint_t j;
    ac_plugin_t *plugins = main->plugins.elts;
    ac_resource(L, main, "consumers"); consumers = lua_gettop(L);
    lua_getfield(L, consumers, "values"); values = lua_gettop(L);
    lua_newtable(L); all = lua_gettop(L);
    main->consumer_plugins_ref = ac_ref(L, all);
    lua_newtable(L); main->consumer_indexes_ref = ac_ref(L, -1); lua_pop(L, 1);
    for (j = 0; j < main->plugins.nelts; j++) {
        if (!plugins[j].auth) continue;
        lua_newtable(L); group = lua_gettop(L);
        lua_newtable(L); nodes = lua_gettop(L);
        for (i = 1; i <= (int) lua_objlen(L, values); i++) {
            lua_rawgeti(L, values, i); wrapper = lua_gettop(L);
            lua_getfield(L, wrapper, "value"); value = lua_gettop(L);
            lua_getfield(L, value, "plugins"); configs = lua_gettop(L);
            if (lua_istable(L, configs)) {
                lua_pushlstring(L, (char *) plugins[j].name.data, plugins[j].name.len); lua_rawget(L, configs);
                config = lua_gettop(L);
                if (lua_istable(L, config)) {
                    ac_push(L, plugins[j].module_ref); lua_getfield(L, -1, "check_schema");
                    if (lua_isfunction(L, -1)) {
                        lua_pushvalue(L, config); lua_pushinteger(L, 1); lua_call(L, 2, 2);
                        if (!lua_toboolean(L, -2)) luaL_error(L, "invalid consumer plugin schema: %s", lua_tostring(L, -1));
                        lua_pop(L, 2);
                    } else lua_pop(L, 1);
                    lua_pop(L, 1);
                    ac_consumer_clone(L, wrapper, config);
                    lua_rawseti(L, nodes, (int) lua_objlen(L, nodes) + 1);
                }
                lua_pop(L, 1);
            }
            lua_pop(L, 3);
        }
        if (lua_objlen(L, nodes)) {
            lua_pushvalue(L, nodes); lua_setfield(L, group, "nodes");
            lua_pushinteger(L, (lua_Integer) lua_objlen(L, nodes)); lua_setfield(L, group, "len");
            lua_getfield(L, consumers, "conf_version"); lua_setfield(L, group, "conf_version");
            lua_pushlstring(L, (char *) plugins[j].name.data, plugins[j].name.len);
            lua_pushvalue(L, group); lua_rawset(L, all);
        }
        lua_pop(L, 2);
    }
    lua_pop(L, 3);
}

static int
ac_consumer_plugin(lua_State *L)
{
    ac_push(L, ac_main()->consumer_plugins_ref);
    lua_pushvalue(L, 1); lua_rawget(L, -2);
    return 1;
}

static int
ac_consumer_kv(lua_State *L)
{
    int cache, nodes, i, consumer, conf, key, values;
    const char *attribute = luaL_checkstring(L, 3);
    ac_push(L, ac_main()->consumer_indexes_ref);
    lua_pushvalue(L, 2); lua_rawget(L, -2);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1); lua_newtable(L);
        lua_pushvalue(L, 2); lua_pushvalue(L, -2); lua_rawset(L, -4);
    }
    lua_remove(L, -2);
    cache = lua_gettop(L);
    lua_getfield(L, cache, attribute);
    if (!lua_isnil(L, -1)) return 1;
    lua_pop(L, 1); lua_newtable(L); values = lua_gettop(L);
    lua_getfield(L, 2, "nodes"); nodes = lua_gettop(L);
    for (i = 1; i <= (int) lua_objlen(L, nodes); i++) {
        lua_rawgeti(L, nodes, i); consumer = lua_gettop(L);
        lua_getfield(L, consumer, "auth_conf"); conf = lua_gettop(L);
        lua_getfield(L, conf, attribute); key = lua_gettop(L);
        if (!lua_isnil(L, key)) {
            const char *secret = lua_type(L, key) == LUA_TSTRING ? lua_tostring(L, key) : NULL;
            if (!secret || (ngx_strncmp(secret, "$secret://", 10) != 0
                           && ngx_strncasecmp((u_char *) secret, (u_char *) "$ENV://", 7) != 0)) {
                lua_pushvalue(L, key); lua_pushvalue(L, consumer); lua_rawset(L, values);
            }
        }
        lua_pop(L, 3);
    }
    lua_pushvalue(L, values); lua_setfield(L, cache, attribute);
    lua_pushvalue(L, values);
    return 1;
}

static int
ac_consumer_find(lua_State *L)
{
    int conf, values;
    lua_settop(L, 3);
    lua_pushcfunction(L, ac_consumer_plugin); lua_pushvalue(L, 1); lua_call(L, 1, 1);
    conf = lua_gettop(L);
    if (lua_isnil(L, conf)) {
        lua_pushnil(L); lua_pushnil(L); lua_pushliteral(L, "Missing related consumer"); return 3;
    }
    lua_pushcfunction(L, ac_consumer_kv);
    lua_pushvalue(L, 1); lua_pushvalue(L, conf); lua_pushvalue(L, 2); lua_call(L, 3, 1);
    values = lua_gettop(L);
    lua_pushvalue(L, 3); lua_rawget(L, values);
    lua_pushvalue(L, conf);
    return 2;
}

static int
ac_consumer_attach(lua_State *L)
{
    static struct { const char *field; const char *source; } fields[] = {
        {"consumer_name", "consumer_name"}, {"consumer_group_id", "group_id"}, {NULL, NULL}
    };
    static struct { const char *header; const char *field; } headers[] = {
        {"X-Consumer-Username", "username"}, {"X-Credential-Identifier", "credential_id"},
        {"X-Consumer-Custom-ID", "custom_id"}, {NULL, NULL}
    };
    int i;
    lua_settop(L, 3);
    lua_pushvalue(L, 2); lua_setfield(L, 1, "consumer");
    for (i = 0; fields[i].field; i++) { lua_getfield(L, 2, fields[i].source); lua_setfield(L, 1, fields[i].field); }
    lua_getfield(L, 3, "conf_version"); lua_setfield(L, 1, "consumer_ver");
    for (i = 0; headers[i].header; i++) {
        ac_require(L, "apisix.core.request"); lua_getfield(L, -1, "set_header");
        lua_pushvalue(L, 1); lua_pushstring(L, headers[i].header); lua_getfield(L, 2, headers[i].field);
        lua_call(L, 3, 0); lua_pop(L, 1);
    }
    return 0;
}

static int
ac_consumer_anonymous(lua_State *L)
{
    int wrapper, consumer;
    ac_resource_get(L, ac_main(), "consumers", 1); wrapper = lua_gettop(L);
    if (lua_isnil(L, wrapper)) {
        lua_pushnil(L); lua_pushnil(L);
        lua_pushfstring(L, "failed to get anonymous consumer %s", lua_tostring(L, 1)); return 3;
    }
    lua_getfield(L, wrapper, "value"); consumer = lua_gettop(L);
    lua_getfield(L, consumer, "id"); lua_setfield(L, consumer, "consumer_name");
    lua_getfield(L, wrapper, "modifiedIndex"); lua_setfield(L, consumer, "modifiedIndex");
    lua_newtable(L); lua_getfield(L, wrapper, "modifiedIndex"); lua_setfield(L, -2, "conf_version");
    return 2;
}

static int
ac_consumers(lua_State *L)
{
    ac_resource(L, ac_main(), "consumers");
    lua_getfield(L, -1, "values"); lua_getfield(L, -2, "conf_version"); return 2;
}

static int
ac_multi_auth(lua_State *L)
{
    lua_getfield(L, 1, "_plugin_name");
    lua_pushboolean(L, lua_isstring(L, -1) && ngx_strcmp(lua_tostring(L, -1), "multi-auth") == 0);
    return 1;
}

static int
ac_auth_open(lua_State *L)
{
    lua_newtable(L); lua_pushcfunction(L, ac_multi_auth); lua_setfield(L, -2, "is_running_under_multi_auth"); return 1;
}

static int
ac_consumer_open(lua_State *L)
{
    static const luaL_Reg functions[] = {
        {"plugin", ac_consumer_plugin}, {"consumers_conf", ac_consumer_plugin}, {"consumers_kv", ac_consumer_kv},
        {"find_consumer", ac_consumer_find}, {"attach_consumer", ac_consumer_attach},
        {"get_anonymous_consumer", ac_consumer_anonymous}, {"consumers", ac_consumers}, {NULL, NULL}
    };
    lua_newtable(L); luaL_register(L, NULL, functions); return 1;
}

static int
ac_plugin_count(lua_State *L, int plugins)
{
    int count = 0;
    plugins = ac_abs(L, plugins);
    if (!lua_istable(L, plugins)) return 0;
    lua_pushnil(L);
    while (lua_next(L, plugins)) { count++; lua_pop(L, 1); }
    return count;
}

static int
ac_consumer_build(lua_State *L, ac_request_t *ctx, ngx_pool_t *pool, ngx_flag_t cached)
{
    int table, consumer, group = 0, wrapper, value, plugins, source, i;
    ac_route_t *route;
    ac_call_t *calls;
    ac_push(L, ctx->table_ref); table = lua_gettop(L);
    lua_getfield(L, table, "consumer"); consumer = lua_gettop(L);
    if (!lua_istable(L, consumer)) { lua_pop(L, 2); return 0; }
    lua_getfield(L, consumer, "group_id");
    if (!lua_isnil(L, -1)) {
        ac_resource_get(L, ctx->main, "consumer_groups", -1);
        lua_remove(L, -2);
        if (lua_isnil(L, -1)) { ctx->early_status = 503; lua_pop(L, 3); return 0; }
        group = lua_gettop(L);
    } else lua_pop(L, 1);
    lua_getfield(L, consumer, "plugins"); i = ac_plugin_count(L, -1); lua_pop(L, 1);
    if (group) {
        lua_getfield(L, group, "value"); lua_getfield(L, -1, "plugins");
        i += ac_plugin_count(L, -1); lua_pop(L, 2);
    }
    if (!i) { lua_settop(L, table - 1); ac_route_context(L, ctx, 0); return 0; }
    ac_require(L, "apisix.core.table"); lua_getfield(L, -1, "deepcopy");
    ac_push(L, ctx->route->wrapper_ref);
    lua_newtable(L); lua_pushliteral(L, "self.value.plugins"); lua_setfield(L, -2, "shallow_prefix");
    lua_call(L, 2, 1); wrapper = lua_gettop(L);
    lua_getfield(L, wrapper, "value"); value = lua_gettop(L);
    lua_getfield(L, value, "plugins");
    if (lua_isnil(L, -1)) { lua_pop(L, 1); lua_newtable(L); lua_pushvalue(L, -1); lua_setfield(L, value, "plugins"); }
    plugins = lua_gettop(L);
    for (i = 0; i < 2; i++) {
        if (i == 0) {
            if (!group) continue;
            lua_getfield(L, group, "value"); lua_getfield(L, -1, "plugins"); lua_remove(L, -2);
        } else lua_getfield(L, consumer, "plugins");
        source = lua_gettop(L);
        if (lua_istable(L, source)) {
            lua_pushnil(L);
            while (lua_next(L, source)) {
                lua_pushvalue(L, -2); lua_rawget(L, plugins);
                if (lua_isnil(L, -1)) { lua_pushboolean(L, 1); lua_setfield(L, -3, "_from_consumer"); }
                lua_pop(L, 1);
                lua_pushvalue(L, -2); lua_pushvalue(L, -2); lua_rawset(L, plugins);
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
    }
    route = ngx_palloc(pool, sizeof(*route));
    if (!route) luaL_error(L, "consumer route allocation failed");
    *route = *ctx->route;
    route->wrapper_ref = ac_ref(L, wrapper);
    route->value_ref = ac_ref(L, value);
    ac_compile_plan(L, ctx->main, &route->plan, pool, plugins, 0);
    calls = route->plan.calls.elts;
    for (i = 0; i < (int) route->plan.calls.nelts; i++) {
        ac_push(L, calls[i].conf_ref);
        calls[i].consumer_only = ac_bool_field(L, -1, "_from_consumer");
        if (!calls[i].consumer_only || calls[i].plugin->auth) {
            lua_pushboolean(L, 1); lua_setfield(L, -2, "_skip_rewrite_in_consumer");
        }
        lua_pop(L, 1);
    }
    ctx->route = route; ctx->temporary_route = !cached;
    lua_pushvalue(L, wrapper); lua_setfield(L, table, "matched_route");
    lua_settop(L, table - 1);
    ac_route_context(L, ctx, 0);
    return 1;
}

static int
ac_consumer_build_call(lua_State *L)
{
    ac_request_t *ctx = lua_touserdata(L, lua_upvalueindex(1));
    ngx_pool_t *pool = lua_touserdata(L, lua_upvalueindex(2));
    ngx_flag_t cached = lua_toboolean(L, lua_upvalueindex(3));
    if (cached) {
        ac_ref(L, 1);
        if (lua_istable(L, 2)) ac_ref(L, 2);
    }
    int changed = ac_consumer_build(L, ctx, pool, cached);
    lua_pushboolean(L, changed); return 1;
}

int
ac_consumer_merge(lua_State *L, ac_request_t *ctx)
{
    ac_main_conf_t *root = ac_main();
    ac_consumer_route_t *entries = ctx->main->consumer_routes.elts, *entry = NULL;
    ngx_array_t *cache = &ctx->main->consumer_routes;
    ac_route_t *base = ctx->route;
    ngx_pool_t *pool = ctx->request->pool;
    ngx_uint_t i, references = 0;
    int top = lua_gettop(L), table, consumer, group, status, changed;
    const void *consumer_key, *group_key;
    ac_push(L, ctx->table_ref); table = lua_gettop(L);
    lua_getfield(L, table, "consumer"); consumer = lua_gettop(L);
    if (!lua_istable(L, consumer)) { lua_settop(L, top); return 0; }
    ctx->metadata = NULL;
    consumer_key = lua_topointer(L, consumer);
    lua_getfield(L, consumer, "group_id");
    if (!lua_isnil(L, -1)) { ac_resource_get(L, ctx->main, "consumer_groups", -1); lua_remove(L, -2); }
    group = lua_gettop(L); group_key = lua_istable(L, group) ? lua_topointer(L, group) : NULL;
    for (i = 0; i < cache->nelts; i++) {
        if (entries[i].base != base || entries[i].consumer != consumer_key || entries[i].group != group_key) continue;
        if (entries[i].error_ref != LUA_NOREF) { ac_push(L, entries[i].error_ref); return lua_error(L); }
        ctx->route = entries[i].route; ctx->temporary_route = 0;
        ctx->metadata = entries[i].metadata;
        ctx->early_status = entries[i].status;
        ac_push(L, ctx->route->wrapper_ref); lua_setfield(L, table, "matched_route");
        lua_settop(L, top); ac_route_context(L, ctx, 0); return entries[i].changed;
    }
    /* Bound retained compiled combinations. Beyond the cap, use request-owned
     * plans as before. A configuration generation owns every cached reference. */
    if (ctx->generation && cache->nelts < 256) {
        entry = ngx_array_push(cache);
        if (!entry) return luaL_error(L, "consumer plan cache entry allocation failed");
        ngx_memzero(entry, sizeof(*entry));
        entry->base = base; entry->consumer = consumer_key; entry->group = group_key; entry->error_ref = LUA_NOREF;
        pool = ctx->main->pool;
        references = ctx->generation->refs.nelts;
    }
    lua_pushlightuserdata(L, ctx); lua_pushlightuserdata(L, pool); lua_pushboolean(L, entry != NULL);
    lua_pushcclosure(L, ac_consumer_build_call, 3); lua_pushvalue(L, consumer); lua_pushvalue(L, group);
    root->building = entry ? ctx->generation : NULL;
    status = lua_pcall(L, 2, 1, 0);
    root->building = NULL;
    if (status) {
        if (entry) {
            int *refs = ctx->generation->refs.elts;
            for (i = references; i < ctx->generation->refs.nelts; i++) luaL_unref(L, LUA_REGISTRYINDEX, refs[i]);
            ctx->generation->refs.nelts = references;
            root->building = ctx->generation;
            entry->error_ref = ac_ref(L, -1);
            root->building = NULL;
        }
        return lua_error(L);
    }
    changed = lua_toboolean(L, -1);
    if (entry) {
        entry->route = ctx->route; entry->changed = changed; entry->status = ctx->early_status;
        entry->metadata = ctx->metadata;
    }
    lua_settop(L, top); return changed;
}

ngx_int_t
ac_consumer_register(ngx_conf_t *cf)
{
    if (ngx_http_lua_add_package_preload(cf, "apisix.consumer", ac_consumer_open) != NGX_OK
        || ngx_http_lua_add_package_preload(cf, "apisix.utils.auth", ac_auth_open) != NGX_OK) return NGX_ERROR;
    return ac_cache_register(cf);
}
