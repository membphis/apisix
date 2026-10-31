/* SPDX-License-Identifier: Apache-2.0 */
#include "engine.h"
#include <stdio.h>

static void
ac_snapshot_destroy(lua_State *L, ac_generation_t *generation)
{
    ngx_uint_t i;
    int *refs = generation->refs.elts;
    for (i = 0; i < generation->refs.nelts; i++) luaL_unref(L, LUA_REGISTRYINDEX, refs[i]);
    ngx_destroy_pool(generation->main.pool);
}

static ac_generation_t *
ac_snapshot_new(lua_State *L)
{
    ac_main_conf_t *root = ac_main();
    ngx_pool_t *pool = ngx_create_pool(16384, ngx_cycle->log);
    ac_generation_t *generation;
    if (!pool) { luaL_error(L, "configuration generation allocation failed"); return NULL; }
    generation = ngx_pcalloc(pool, sizeof(*generation));
    if (!generation || ngx_array_init(&generation->refs, pool, 256, sizeof(int)) != NGX_OK) {
        ngx_destroy_pool(pool); luaL_error(L, "configuration reference list allocation failed"); return NULL;
    }
    generation->main = *root;
    generation->main.pool = pool;
    if (ngx_array_init(&generation->main.consumer_routes, pool, 4, sizeof(ac_consumer_route_t)) != NGX_OK) {
        ngx_destroy_pool(pool); luaL_error(L, "consumer plan cache allocation failed"); return NULL;
    }
    generation->main.config_version = root->config_version + 1;
    return generation;
}

/* Compilation cannot publish a partial route/global/consumer graph. */
static int
ac_snapshot_compile(lua_State *L)
{
    static const char *kinds[] = {"routes", "services", "upstreams", "consumers", "consumer_groups",
        "plugin_configs", "plugin_metadata", "global_rules", "ssls", "stream_routes", "protos", "secrets", NULL};
    ac_generation_t *generation = lua_touserdata(L, lua_upvalueindex(1));
    ac_main_conf_t *main = &generation->main;
    ngx_uint_t i;
    if (lua_toboolean(L, lua_upvalueindex(2))) {
        for (i = 0; kinds[i]; i++) {
            lua_pushstring(L, kinds[i]); lua_pushliteral(L, "_conf_version"); lua_concat(L, 2);
            lua_pushinteger(L, main->config_version); lua_rawset(L, 1);
        }
        main->resources_ref = ac_ref(L, 1);
        ac_resources_prepare(L, main, 1);
        ac_snapshot_readers(L, main, 0);
    }
    ac_compile_globals(L, main, 1);
    ac_consumer_prepare(L, main);
    if (main->native_key_auth == 1) ac_key_auth_prepare(L, main);
    ac_compile_routes(L, main, 1);
    if (main->native_key_auth == 1) ac_pipeline_compile(L, main);
    return 0;
}

static void
ac_snapshot_publish(lua_State *L, ac_generation_t *generation)
{
    ac_main_conf_t *root = ac_main(), *main = &generation->main;
    ac_generation_t *old = root->generation;
    root->routes = main->routes; root->route_index = main->route_index; root->global = main->global;
    root->resources_ref = main->resources_ref; root->store_ref = main->store_ref; root->local_ref = main->local_ref;
    root->consumer_plugins_ref = main->consumer_plugins_ref; root->consumer_indexes_ref = main->consumer_indexes_ref;
    root->config_crc = main->config_crc; root->config_version = main->config_version;
    root->generation = generation; main->compiled = 1;
    root->host_router = main->host_router; root->parameter_router = main->parameter_router;
    ac_snapshot_readers(L, main, 1);
    if (old) {
        if (!old->users) ac_snapshot_destroy(L, old);
        else { old->next = root->retired; root->retired = old; }
    }
    /* The public plugin module holds this resource object as a visible field. */
    ac_require(L, "apisix.plugin"); ac_resource(L, root, "plugin_metadata");
    lua_setfield(L, -2, "plugin_metadatas"); lua_pop(L, 1);
}

static ngx_flag_t
ac_snapshot_build(lua_State *L, ac_generation_t *generation, int config, ngx_flag_t reload)
{
    ac_main_conf_t *root = ac_main();
    config = ac_abs(L, config);
    lua_pushlightuserdata(L, generation); lua_pushboolean(L, reload);
    lua_pushcclosure(L, ac_snapshot_compile, 2); lua_pushvalue(L, config);
    root->building = generation;
    int result = lua_pcall(L, 1, 0, 0);
    root->building = NULL;
    if (result) {
        ac_snapshot_destroy(L, generation);
        return 0;
    }
    ac_snapshot_publish(L, generation); return 1;
}

void
ac_snapshot_acquire(ac_request_t *ctx)
{
    ac_generation_t *generation = ac_main()->generation;
    if (generation) {
        generation->users++; ctx->generation = generation; ctx->main = &generation->main;
    }
}

void
ac_snapshot_release(lua_State *L, ac_request_t *ctx)
{
    ac_main_conf_t *root = ac_main();
    ac_generation_t *generation = ctx->generation, **link;
    ctx->generation = NULL;
    if (!generation) return;
    if (generation->users) generation->users--;
    /* C-only requests defer retired Lua reference reclamation to the watcher. */
    if (!L || generation->users || root->generation == generation) return;
    for (link = &root->retired; *link && *link != generation; link = &(*link)->next) { }
    if (*link) { *link = generation->next; ac_snapshot_destroy(L, generation); }
}

static ngx_flag_t
ac_same_setting(lua_State *L, int next, int previous, const char *name)
{
    ngx_flag_t same;
    next = ac_abs(L, next); previous = ac_abs(L, previous);
    ac_require(L, "apisix.core.table"); lua_getfield(L, -1, "deep_eq"); lua_remove(L, -2);
    lua_getfield(L, next, name);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1); lua_getfield(L, previous, name);
        lua_pushvalue(L, -1); lua_setfield(L, next, name);
    }
    lua_getfield(L, previous, name); lua_call(L, 2, 1);
    same = lua_toboolean(L, -1); lua_pop(L, 1); return same;
}

static int
ac_snapshot_reload(lua_State *L)
{
    ac_main_conf_t *root = ac_main();
    FILE *file;
    long length;
    u_char *data;
    uint32_t crc;
    int config, previous;
    ac_generation_t *generation;
    file = fopen((char *) root->config_path.data, "rb");
    if (!file) { lua_pushboolean(L, 0); lua_pushliteral(L, "cannot open standalone configuration"); return 2; }
    if (fseek(file, 0, SEEK_END) || (length = ftell(file)) < 0 || length > 64 * 1024 * 1024
        || fseek(file, 0, SEEK_SET)) {
        fclose(file); lua_pushboolean(L, 0); lua_pushliteral(L, "invalid standalone configuration length"); return 2;
    }
    data = ngx_alloc((size_t) length + 1, ngx_cycle->log);
    if (!data) { fclose(file); return luaL_error(L, "standalone reload buffer allocation failed"); }
    if (fread(data, 1, (size_t) length, file) != (size_t) length) {
        fclose(file); ngx_free(data); lua_pushboolean(L, 0); lua_pushliteral(L, "standalone configuration read failed"); return 2;
    }
    fclose(file); crc = ngx_crc32_long(data, (size_t) length);
    if (crc == root->config_crc) { ngx_free(data); goto unchanged; }
    /* Copy to a Lua string before decoding so errors cannot leak the file buffer. */
    lua_pushlstring(L, (char *) data, (size_t) length); ngx_free(data);
    ac_require(L, "cjson.safe"); lua_getfield(L, -1, "decode"); lua_pushvalue(L, -3);
    lua_call(L, 1, 2);
    if (!lua_istable(L, -2)) { lua_pushboolean(L, 0); lua_pushvalue(L, -2); return 2; }
    lua_pop(L, 1); config = lua_gettop(L);
    ac_push(L, root->resources_ref); previous = lua_gettop(L);
    if (!ac_same_setting(L, config, previous, "plugins") || !ac_same_setting(L, config, previous, "local_conf")
        || !ac_same_setting(L, config, previous, "router")) {
        lua_pushboolean(L, 0); lua_pushliteral(L, "local settings and enabled plugins require an NGINX reload"); return 2;
    }
    generation = ac_snapshot_new(L); generation->main.config_crc = crc;
    if (!ac_snapshot_build(L, generation, config, 1)) {
        lua_pushboolean(L, 0); lua_insert(L, -2); return 2;
    }
unchanged:
    lua_pushboolean(L, 1); lua_pushinteger(L, root->config_version); return 2;
}

static int
ac_snapshot_stats(lua_State *L)
{
    ac_main_conf_t *root = ac_main();
    ac_generation_t *item;
    ngx_uint_t live = root->generation ? 1 : 0, readers = root->generation ? root->generation->users : 0;
    for (item = root->retired; item; item = item->next) { live++; readers += item->users; }
    lua_newtable(L);
    lua_pushinteger(L, root->config_version); lua_setfield(L, -2, "version");
    lua_pushinteger(L, live); lua_setfield(L, -2, "generations");
    lua_pushinteger(L, readers); lua_setfield(L, -2, "readers"); return 1;
}

static int
ac_snapshot_tick(lua_State *L)
{
    if (lua_toboolean(L, 1)) return 0;
    ac_generation_t **link = &ac_main()->retired, *item;
    while ((item = *link) != NULL) {
        if (!item->users) { *link = item->next; ac_snapshot_destroy(L, item); }
        else link = &item->next;
    }
    lua_pushcfunction(L, ac_snapshot_reload);
    if (lua_pcall(L, 0, 2, 0)) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "standalone reload failed: %s", lua_tostring(L, -1));
    } else if (!lua_toboolean(L, -2)) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "standalone configuration rejected: %s", lua_tostring(L, -1));
    }
    return 0;
}

void
ac_snapshot_worker(lua_State *L)
{
    ac_generation_t *generation = ac_snapshot_new(L);
    ac_push(L, ac_main()->resources_ref);
    if (!ac_snapshot_build(L, generation, -1, 0)) lua_error(L);
    lua_pop(L, 1);
    ac_ngx(L, "timer", "every"); lua_pushinteger(L, 1); ac_bind(L, ac_snapshot_tick, 0);
    lua_call(L, 2, 2);
    if (!lua_toboolean(L, -2)) luaL_error(L, "standalone file watcher could not start: %s", lua_tostring(L, -1));
    lua_pop(L, 2);
}

static int
ac_snapshot_open(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, ac_snapshot_reload); lua_setfield(L, -2, "reload");
    lua_pushcfunction(L, ac_snapshot_stats); lua_setfield(L, -2, "stats"); return 1;
}

ngx_int_t
ac_snapshot_register(ngx_conf_t *cf)
{
    return ngx_http_lua_add_package_preload(cf, "apisix.native.config", ac_snapshot_open);
}
