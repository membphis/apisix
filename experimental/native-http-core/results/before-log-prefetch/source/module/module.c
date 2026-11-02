/*
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements. See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "engine.h"
#include <stdio.h>

const char *ac_phase_names[AC_PHASES] = {
    "rewrite", "access", "before_proxy", "header_filter",
    "body_filter", "delayed_body_filter", "log"
};

static int ac_open(lua_State *L);
static ngx_int_t ac_register(ngx_conf_t *cf);
static void *ac_create_main(ngx_conf_t *cf);
static void *ac_create_location(ngx_conf_t *cf);
static char *ac_merge_location(ngx_conf_t *cf, void *parent, void *child);

static ngx_command_t ac_commands[] = {
    {ngx_string("apisix_core_native_filters"), NGX_HTTP_MAIN_CONF | NGX_CONF_FLAG,
     ngx_conf_set_flag_slot, NGX_HTTP_MAIN_CONF_OFFSET, offsetof(ac_main_conf_t, native_filters), NULL},
    {ngx_string("apisix_core_proxy"), NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_FLAG,
     ngx_conf_set_flag_slot, NGX_HTTP_LOC_CONF_OFFSET, offsetof(ac_loc_conf_t, proxy), NULL},
    ngx_null_command
};

static ngx_http_module_t ac_module_context = {
    ac_upstream_variables, ac_register, ac_create_main, NULL, NULL, NULL, ac_create_location, ac_merge_location
};

ngx_module_t ngx_http_apisix_core_module = {
    NGX_MODULE_V1, &ac_module_context, ac_commands, NGX_HTTP_MODULE,
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NGX_MODULE_V1_PADDING
};

int
ac_abs(lua_State *L, int index)
{
    return index > 0 || index <= LUA_REGISTRYINDEX
           ? index : lua_gettop(L) + index + 1;
}

int
ac_ref(lua_State *L, int index)
{
    int ref, *slot;
    ac_generation_t *building = ac_main()->building;
    lua_pushvalue(L, index);
    ref = luaL_ref(L, LUA_REGISTRYINDEX);
    if (building && ref >= 0) {
        slot = ngx_array_push(&building->refs);
        if (!slot) {
            luaL_unref(L, LUA_REGISTRYINDEX, ref);
            luaL_error(L, "configuration reference tracking allocation failed");
        }
        *slot = ref;
    }
    return ref;
}

void
ac_push(lua_State *L, int ref)
{
    if (ref == LUA_NOREF || ref == LUA_REFNIL) {
        lua_pushnil(L);
    } else {
        lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
    }
}

ngx_str_t
ac_string(lua_State *L, ngx_pool_t *pool, int index)
{
    ngx_str_t value;
    const char *data = luaL_checklstring(L, index, &value.len);
    value.data = ngx_pnalloc(pool, value.len + 1);
    if (value.data == NULL) {
        luaL_error(L, "native core: allocation failed");
    }
    ngx_memcpy(value.data, data, value.len);
    value.data[value.len] = '\0';
    return value;
}

ngx_flag_t
ac_bool_field(lua_State *L, int index, const char *name)
{
    ngx_flag_t value;
    lua_getfield(L, index, name);
    value = lua_toboolean(L, -1);
    lua_pop(L, 1);
    return value;
}

void
ac_set_string(lua_State *L, int index, const char *name, ngx_str_t value)
{
    index = ac_abs(L, index);
    lua_pushlstring(L, (const char *) value.data, value.len);
    lua_setfield(L, index, name);
}

void
ac_copy(lua_State *L, int index, ngx_uint_t depth)
{
    int copy;
    index = ac_abs(L, index);
    if (!lua_istable(L, index)) {
        lua_pushvalue(L, index);
        return;
    }
    if (depth > 128) {
        luaL_error(L, "native configuration exceeds maximum nesting");
    }
    lua_newtable(L);
    copy = lua_gettop(L);
    lua_pushnil(L);
    while (lua_next(L, index)) {
        lua_pushvalue(L, -2);
        ac_copy(L, -2, depth + 1);
        lua_rawset(L, copy);
        lua_pop(L, 1);
    }
    if (lua_getmetatable(L, index)) {
        lua_setmetatable(L, copy);
    }
}

ac_main_conf_t *
ac_main(void)
{
    return ngx_http_cycle_get_module_main_conf(ngx_cycle,
                                               ngx_http_apisix_core_module);
}

static void
ac_cleanup(void *data)
{
    ac_request_t *ctx = data;
    ac_call_t *calls;
    ngx_uint_t i;
    if (!ctx->released && ctx->main->vm != NULL && !ngx_exiting) {
        ac_upstream_cleanup(ctx->main->vm, ctx);
        if (ctx->temporary_route) {
            calls = ctx->route->plan.calls.elts;
            for (i = 0; i < ctx->route->plan.calls.nelts; i++) {
                luaL_unref(ctx->main->vm, LUA_REGISTRYINDEX, calls[i].conf_ref);
                luaL_unref(ctx->main->vm, LUA_REGISTRYINDEX, calls[i].pre_ref);
                ac_expr_release(ctx->main->vm, calls[i].expression);
            }
            luaL_unref(ctx->main->vm, LUA_REGISTRYINDEX, ctx->route->value_ref);
            luaL_unref(ctx->main->vm, LUA_REGISTRYINDEX, ctx->route->wrapper_ref);
        }
        luaL_unref(ctx->main->vm, LUA_REGISTRYINDEX, ctx->table_ref);
        luaL_unref(ctx->main->vm, LUA_REGISTRYINDEX, ctx->match_opts_ref);
        luaL_unref(ctx->main->vm, LUA_REGISTRYINDEX, ctx->span_ref);
        ac_snapshot_release(ctx->main->vm, ctx);
    }
    ctx->released = 1;
    ctx->request = NULL;
}

ac_request_t *
ac_request(lua_State *L)
{
    ngx_http_request_t *request = ngx_http_lua_get_request(L);
    ngx_pool_cleanup_t *cleanup;
    ac_request_t *ctx;
    if (request == NULL) {
        luaL_error(L, "native core API requires an HTTP request");
    }
    ctx = ngx_http_get_module_ctx(request, ngx_http_apisix_core_module);
    if (ctx != NULL) {
        return ctx;
    }
    ctx = ngx_pcalloc(request->pool, sizeof(*ctx));
    cleanup = ngx_pool_cleanup_add(request->pool, 0);
    if (ctx == NULL || cleanup == NULL) {
        luaL_error(L, "native core request allocation failed");
    }
    ctx->request = request;
    if (ngx_array_init(&ctx->meta_matches, request->pool, 4, sizeof(ac_meta_match_t)) != NGX_OK) {
        luaL_error(L, "native plugin filter cache allocation failed");
    }
    ctx->main = ac_main();
    if (!ctx->main->compiled) {
        luaL_error(L, "native core has not been initialized");
    }
    ac_snapshot_acquire(ctx);
    ctx->phase = AC_PHASES;
    ctx->match_opts_ref = LUA_NOREF;
    ctx->span_ref = LUA_NOREF;
    lua_createtable(L, 0, 32);
    ctx->table_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_getglobal(L, "ngx");
    lua_getfield(L, -1, "ctx");
    ac_push(L, ctx->table_ref);
    lua_pushvalue(L, -2);
    lua_setfield(L, -2, "ngx_ctx");
    lua_setfield(L, -2, "api_ctx");
    lua_pop(L, 2);
    cleanup->data = ctx;
    cleanup->handler = ac_cleanup;
    ngx_http_set_ctx(request, ctx, ngx_http_apisix_core_module);
    ac_sdk_context(L, ctx);
    return ctx;
}

int
ac_context(lua_State *L)
{
    ac_request_t *ctx = ac_request(L);
    ac_push(L, ctx->table_ref);
    return 1;
}

int
ac_init(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    ac_main_conf_t *main = ac_main();
    FILE *file;
    long length;
    char *data;
    int config;
    if (main->initialized) {
        return luaL_error(L, "native core is already initialized");
    }
    if (!lua_pushthread(L)) {
        return luaL_error(L, "native core must initialize on the main Lua VM");
    }
    lua_pop(L, 1);
    main->vm = L;
    main->config_path = ac_string(L, main->pool, 1);
    file = fopen(path, "rb");
    if (file == NULL) {
        return luaL_error(L, "cannot open native standalone configuration");
    }
    if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0
        || length > 64 * 1024 * 1024 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return luaL_error(L, "invalid native standalone configuration length");
    }
    data = ngx_pnalloc(main->pool, (size_t) length + 1);
    if (data == NULL) {
        fclose(file);
        return luaL_error(L, "native configuration allocation failed");
    }
    if (fread(data, 1, (size_t) length, file) != (size_t) length) {
        fclose(file);
        return luaL_error(L, "native configuration read failed");
    }
    fclose(file);
    main->config_crc = ngx_crc32_long((u_char *) data, (size_t) length);
    lua_getglobal(L, "require");
    lua_pushliteral(L, "cjson.safe");
    lua_call(L, 1, 1);
    lua_getfield(L, -1, "decode");
    lua_pushlstring(L, data, (size_t) length);
    lua_call(L, 1, 2);
    if (!lua_istable(L, -2)) {
        return luaL_error(L, "invalid standalone JSON: %s", lua_tostring(L, -1));
    }
    lua_pop(L, 1);
    config = lua_gettop(L);
    main->resources_ref = ac_ref(L, config);
    ac_resources_prepare(L, main, config);
    ac_push(L, main->local_ref); lua_getfield(L, -1, "apisix");
    main->tracing = ac_bool_field(L, -1, "tracing"); lua_pop(L, 2);
    ac_require(L, "apisix.core.env"); lua_getfield(L, -1, "init"); lua_call(L, 0, 0); lua_pop(L, 1);
    ac_require(L, "ngx.process");
    lua_getfield(L, -1, "enable_privileged_agent");
    lua_call(L, 0, 2);
    if (!lua_toboolean(L, -2)) {
        return luaL_error(L, "native privileged worker setup failed: %s", lua_tostring(L, -1));
    }
    lua_pop(L, 3);
    main->initialized = 1;
    lua_pushboolean(L, 1);
    return 1;
}

static int
ac_open(lua_State *L)
{
    static const luaL_Reg functions[] = {
        {"init", ac_init}, {"init_worker", ac_init_worker},
        {"shutdown", ac_shutdown}, {"context", ac_context},
        {"enter", ac_enter}, {"next", ac_next},
        {"complete", ac_complete}, {"leave", ac_leave},
        {"start_phase", ac_phase_start}, {"step", ac_phase_step},
        {"encode", ac_encode}, {"render", ac_render},
        {"balance_enter", ac_balance_enter}, {"balance_finish", ac_balance_finish}, {NULL, NULL}
    };
    lua_newtable(L);
    luaL_register(L, NULL, functions);
    return 1;
}

int
ac_encode(lua_State *L)
{
    lua_settop(L, 1);
    lua_getglobal(L, "require");
    lua_pushliteral(L, "cjson.safe");
    lua_call(L, 1, 1);
    lua_getfield(L, -1, "encode");
    lua_pushvalue(L, 1);
    lua_call(L, 1, LUA_MULTRET);
    return lua_gettop(L) - 2;
}

int
ac_render(lua_State *L)
{
    lua_Integer status = luaL_checkinteger(L, 1);
    ac_request_t *ctx = ac_request(L);
    lua_settop(L, 2);
    ac_push(L, ctx->table_ref);
    lua_getfield(L, -1, "_resp_source");
    if (!lua_toboolean(L, -1)) { lua_pushliteral(L, "apisix"); lua_setfield(L, -3, "_resp_source"); }
    lua_pop(L, 1);
    lua_pop(L, 1);
    if (status >= 400) ac_trace_finish_all(L, ctx, (ngx_uint_t) status);
    if (lua_istable(L, 2)) {
        lua_getglobal(L, "require");
        lua_pushliteral(L, "cjson.safe");
        lua_call(L, 1, 1);
        lua_getfield(L, -1, "encode");
        lua_pushvalue(L, 2);
        lua_call(L, 1, 2);
        if (lua_isnil(L, -2)) {
            return luaL_error(L, "native response JSON encoding failed: %s", lua_tostring(L, -1));
        }
        lua_pop(L, 1);
        lua_pushliteral(L, "\n"); lua_concat(L, 2);
        lua_replace(L, 2);
        lua_settop(L, 2);
        lua_getglobal(L, "ngx");
        lua_getfield(L, -1, "header");
        lua_pushliteral(L, "application/json");
        lua_setfield(L, -2, "Content-Type");
        lua_pop(L, 2);
    }
    lua_pushinteger(L, status);
    lua_pushvalue(L, 2);
    return 2;
}

static void *
ac_create_main(ngx_conf_t *cf)
{
    ac_main_conf_t *main = ngx_pcalloc(cf->pool, sizeof(*main));
    if (main != NULL) {
        main->pool = cf->pool;
        main->native_filters = NGX_CONF_UNSET;
        main->resources_ref = LUA_NOREF;
        main->store_ref = LUA_NOREF;
        main->local_ref = LUA_NOREF;
        main->var_getters_ref = LUA_NOREF;
        main->var_meta_ref = LUA_NOREF;
        main->consumer_plugins_ref = LUA_NOREF;
        main->resolver_names_ref = LUA_NOREF;
        main->dns_client_ref = LUA_NOREF;
        main->hosts_ref = LUA_NOREF;
        main->trusted_ref = LUA_NOREF;
        main->readers_ref = LUA_NOREF;
    }
    return main;
}

static ngx_int_t
ac_register(ngx_conf_t *cf)
{
    if (ngx_http_lua_add_package_preload(cf, "apisix.native.engine", ac_open) != NGX_OK) {
        return NGX_ERROR;
    }
    if (ac_http_register(cf) != NGX_OK) return NGX_ERROR;
    if (ac_helpers_register(cf) != NGX_OK) return NGX_ERROR;
    if (ac_script_register(cf) != NGX_OK) return NGX_ERROR;
    if (ac_snapshot_register(cf) != NGX_OK) return NGX_ERROR;
    if (ac_sdk_register(cf) != NGX_OK) return NGX_ERROR;
    return ac_native_filters_register(cf);
}

static void *
ac_create_location(ngx_conf_t *cf)
{
    ac_loc_conf_t *conf = ngx_pcalloc(cf->pool, sizeof(*conf));
    if (!conf) return NULL;
    conf->proxy = NGX_CONF_UNSET;
    return conf;
}

static char *
ac_merge_location(ngx_conf_t *cf, void *parent, void *child)
{
    ac_loc_conf_t *prev = parent, *conf = child;
    ngx_conf_merge_value(conf->proxy, prev->proxy, 0);
    return NGX_CONF_OK;
}
