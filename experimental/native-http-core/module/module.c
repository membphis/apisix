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

/* Cycle-owned; generations share this pointer, never a copy of count/refs. */
struct ac_table_pool_s {
    lua_State *vm;
    ngx_uint_t count;
    ngx_uint_t releases;
    int refs[200];
};

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
    {ngx_string("apisix_core_lua_log_bridge"), NGX_HTTP_MAIN_CONF | NGX_CONF_FLAG,
     ngx_conf_set_flag_slot, NGX_HTTP_MAIN_CONF_OFFSET, offsetof(ac_main_conf_t, lua_log_bridge), NULL},
    {ngx_string("apisix_core_native_key_auth"), NGX_HTTP_MAIN_CONF | NGX_CONF_FLAG,
     ngx_conf_set_flag_slot, NGX_HTTP_MAIN_CONF_OFFSET, offsetof(ac_main_conf_t, native_key_auth), NULL},
    {ngx_string("apisix_core_native_pipeline"), NGX_HTTP_MAIN_CONF | NGX_CONF_FLAG,
     ngx_conf_set_flag_slot, NGX_HTTP_MAIN_CONF_OFFSET, offsetof(ac_main_conf_t, native_pipeline), NULL},
    {ngx_string("apisix_core_native_balancer"), NGX_HTTP_UPS_CONF | NGX_CONF_NOARGS,
     ac_upstream_native_directive, 0, 0, NULL},
    {ngx_string("apisix_core_balancer_peer_plan"), NGX_HTTP_MAIN_CONF | NGX_CONF_FLAG,
     ngx_conf_set_flag_slot, NGX_HTTP_MAIN_CONF_OFFSET, offsetof(ac_main_conf_t, balancer_peer_plan), NULL},
    {ngx_string("apisix_core_sdk_cache_replay"), NGX_HTTP_MAIN_CONF | NGX_CONF_FLAG,
     ngx_conf_set_flag_slot, NGX_HTTP_MAIN_CONF_OFFSET, offsetof(ac_main_conf_t, sdk_cache_replay), NULL},
    {ngx_string("apisix_core_native_var_writes"), NGX_HTTP_MAIN_CONF | NGX_CONF_FLAG,
     ngx_conf_set_flag_slot, NGX_HTTP_MAIN_CONF_OFFSET, offsetof(ac_main_conf_t, native_var_writes), NULL},
    {ngx_string("apisix_core_batch_vars"), NGX_HTTP_MAIN_CONF | NGX_CONF_FLAG,
     ngx_conf_set_flag_slot, NGX_HTTP_MAIN_CONF_OFFSET, offsetof(ac_main_conf_t, batch_vars), NULL},
    {ngx_string("apisix_core_http_var_plan"), NGX_HTTP_MAIN_CONF | NGX_CONF_FLAG,
     ngx_conf_set_flag_slot, NGX_HTTP_MAIN_CONF_OFFSET, offsetof(ac_main_conf_t, http_var_plan), NULL},
    {ngx_string("apisix_core_lazy_route_opts"), NGX_HTTP_MAIN_CONF | NGX_CONF_FLAG,
     ngx_conf_set_flag_slot, NGX_HTTP_MAIN_CONF_OFFSET, offsetof(ac_main_conf_t, lazy_route_opts), NULL},
    {ngx_string("apisix_core_route_candidates"), NGX_HTTP_MAIN_CONF | NGX_CONF_FLAG,
     ngx_conf_set_flag_slot, NGX_HTTP_MAIN_CONF_OFFSET, offsetof(ac_main_conf_t, route_candidates), NULL},
    {ngx_string("apisix_core_matched_pool"), NGX_HTTP_MAIN_CONF | NGX_CONF_FLAG,
     ngx_conf_set_flag_slot, NGX_HTTP_MAIN_CONF_OFFSET, offsetof(ac_main_conf_t, matched_pool), NULL},
    {ngx_string("apisix_core_request_pool_native"), NGX_HTTP_MAIN_CONF | NGX_CONF_FLAG,
     ngx_conf_set_flag_slot, NGX_HTTP_MAIN_CONF_OFFSET, offsetof(ac_main_conf_t, request_pool_native), NULL},
    {ngx_string("apisix_core_request_pool"), NGX_HTTP_MAIN_CONF | NGX_CONF_FLAG,
     ngx_conf_set_flag_slot, NGX_HTTP_MAIN_CONF_OFFSET, offsetof(ac_main_conf_t, request_pool), NULL},
    {ngx_string("apisix_core_lazy_meta_matches"), NGX_HTTP_MAIN_CONF | NGX_CONF_FLAG,
     ngx_conf_set_flag_slot, NGX_HTTP_MAIN_CONF_OFFSET, offsetof(ac_main_conf_t, lazy_meta_matches), NULL},
    {ngx_string("apisix_core_phase_index"), NGX_HTTP_MAIN_CONF | NGX_CONF_FLAG,
     ngx_conf_set_flag_slot, NGX_HTTP_MAIN_CONF_OFFSET, offsetof(ac_main_conf_t, phase_index), NULL},
    {ngx_string("apisix_core_phase_context"), NGX_HTTP_MAIN_CONF | NGX_CONF_FLAG,
     ngx_conf_set_flag_slot, NGX_HTTP_MAIN_CONF_OFFSET, offsetof(ac_main_conf_t, phase_context), NULL},
    {ngx_string("apisix_core_request_header_flags"), NGX_HTTP_MAIN_CONF | NGX_CONF_FLAG,
     ngx_conf_set_flag_slot, NGX_HTTP_MAIN_CONF_OFFSET, offsetof(ac_main_conf_t, request_header_flags), NULL},
    {ngx_string("apisix_core_header_scratch"), NGX_HTTP_MAIN_CONF | NGX_CONF_FLAG,
     ngx_conf_set_flag_slot, NGX_HTTP_MAIN_CONF_OFFSET, offsetof(ac_main_conf_t, header_scratch), NULL},
    {ngx_string("apisix_core_plain_uri"), NGX_HTTP_MAIN_CONF | NGX_CONF_FLAG,
     ngx_conf_set_flag_slot, NGX_HTTP_MAIN_CONF_OFFSET, offsetof(ac_main_conf_t, plain_uri), NULL},
    {ngx_string("apisix_core_fused_balancer"), NGX_HTTP_MAIN_CONF | NGX_CONF_FLAG,
     ngx_conf_set_flag_slot, NGX_HTTP_MAIN_CONF_OFFSET, offsetof(ac_main_conf_t, fused_balancer), NULL},
    {ngx_string("apisix_core_lazy_rate_json"), NGX_HTTP_MAIN_CONF | NGX_CONF_FLAG,
     ngx_conf_set_flag_slot, NGX_HTTP_MAIN_CONF_OFFSET, offsetof(ac_main_conf_t, lazy_rate_json), NULL},
    {ngx_string("apisix_core_log_var_plan"), NGX_HTTP_MAIN_CONF | NGX_CONF_FLAG,
     ngx_conf_set_flag_slot, NGX_HTTP_MAIN_CONF_OFFSET, offsetof(ac_main_conf_t, log_var_plan), NULL},
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

int
ac_table_pool_fetch(lua_State *L, ac_main_conf_t *main, ac_table_pool_t *pool, int nrec)
{
    if (pool) {
        if (!pool->vm) pool->vm = main->vm;
        if (pool->vm == main->vm && pool->count)
            return pool->refs[--pool->count];
    }
    lua_createtable(L, 0, nrec);
    return luaL_ref(L, LUA_REGISTRYINDEX);
}

/* Reuse registry ownership as well as the table. The existing table.clear
 * primitive remains a protected call: no private LuaJIT ABI or slow Lua-next
 * clear loop. Read the current loaded function instead of caching a potentially
 * replaced function forever. A missing helper simply falls back to GC. */
static ngx_flag_t
ac_table_pool_return(ac_request_t *ctx, ac_table_pool_t *pool, int ref, const char *name)
{
    lua_State *L = ctx->main->vm;
    ngx_uint_t i;
    ngx_flag_t retained = 0;
    int top = lua_gettop(L);
    if (!pool || pool->vm != L || ref < 0) return 0;
    ac_push(L, ref);
    if (lua_getmetatable(L, -1)) goto done;
    lua_pushliteral(L, "_LOADED"); lua_rawget(L, LUA_REGISTRYINDEX);
    if (!lua_istable(L, -1)) goto done;
    lua_pushliteral(L, "table.clear"); lua_rawget(L, -2); lua_remove(L, -2);
    if (!lua_isfunction(L, -1)) goto done;
    /* Match installed tablepool: every 20000 releases discard the old pool
     * and this object without clearing it; full pools also discard unchanged. */
    if (++pool->releases >= 20000) {
        for (i = 0; i < pool->count; i++)
            luaL_unref(L, LUA_REGISTRYINDEX, pool->refs[i]);
        pool->count = 0;
        pool->releases = 0;
        goto done;
    }
    if (pool->count == 200) goto done;
    lua_insert(L, top + 1);  /* clear, table */
    if (lua_pcall(L, 1, 0, 0) != 0) {
        const char *error = lua_tostring(L, -1);
        ngx_log_error(NGX_LOG_ERR, ctx->request->connection->log, 0,
                      "native registry %s pool clear failed: %s", name,
                      error ? error : "non-string Lua error");
        goto done;
    }
    pool->refs[pool->count++] = ref;
    retained = 1;
done:
    lua_settop(L, top);
    return retained;
}

void
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
        /* Match stable Lua's nested-record-before-api_ctx release order. If a
         * plugin replaced curr_req_matched, its old alias remains GC-owned. */
        if (ctx->main->matched_pool == 1 && ctx->log_done && ctx->matched_ref >= 0) {
            lua_State *L = ctx->main->vm;
            ngx_flag_t same;
            ac_push(L, ctx->table_ref);
            lua_pushliteral(L, "curr_req_matched"); lua_rawget(L, -2);
            ac_push(L, ctx->matched_ref);
            same = lua_rawequal(L, -1, -2);
            lua_pop(L, 3);
            if (same && ac_table_pool_return(ctx, ctx->main->matched_pool_state,
                                            ctx->matched_ref, "matched"))
                ctx->matched_ref = LUA_NOREF;
        }
        /* Recycle only the Lua table, after all request-owned native cleanup.
         * A suspended/in-flight request cannot enter this pool. Abnormal exits
         * without completed log cleanup retain the existing GC-only path. */
        if (ctx->main->request_pool_native == 1 && ctx->log_done) {
            if (ac_table_pool_return(ctx, ctx->main->request_pool_state,
                                     ctx->table_ref, "api_ctx"))
                ctx->table_ref = LUA_NOREF;
        } else if (ctx->main->request_pool == 1 && ctx->log_done) {
            lua_State *L = ctx->main->vm;
            int top = lua_gettop(L);
            /* fetch loaded this dependency during construction. Do not invoke
             * require from an unprotected NGINX pool cleanup handler. */
            lua_getfield(L, LUA_REGISTRYINDEX, "_LOADED");
            lua_getfield(L, -1, "tablepool");
            if (lua_istable(L, -1)) {
                lua_getfield(L, -1, "release");
                lua_pushliteral(L, "native_api_ctx");
                ac_push(L, ctx->table_ref);
                /* Plugin-supplied metatables are not our reusable shape. */
                if (!lua_getmetatable(L, -1)) {
                    if (lua_pcall(L, 2, 0, 0) != 0) {
                        const char *error = lua_tostring(L, -1);
                        ngx_log_error(NGX_LOG_ERR, ctx->request->connection->log, 0,
                                      "native api_ctx pool release failed: %s",
                                      error ? error : "non-string Lua error");
                    }
                }
            }
            lua_settop(L, top);
        }
        luaL_unref(ctx->main->vm, LUA_REGISTRYINDEX, ctx->table_ref);
        luaL_unref(ctx->main->vm, LUA_REGISTRYINDEX, ctx->match_opts_ref);
        luaL_unref(ctx->main->vm, LUA_REGISTRYINDEX, ctx->matched_ref);
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
        if (ctx->c_pipeline && !ctx->lua_log_active)
            luaL_error(L, "C-only request cannot enter the Lua engine outside the explicit log bridge");
        return ctx;
    }
    ctx = ngx_pcalloc(request->pool, sizeof(*ctx));
    cleanup = ngx_pool_cleanup_add(request->pool, 0);
    if (ctx == NULL || cleanup == NULL) {
        luaL_error(L, "native core request allocation failed");
    }
    ctx->request = request;
    ctx->main = ac_main();
    if (ctx->main->lazy_meta_matches != 1
        && ngx_array_init(&ctx->meta_matches, request->pool, 4, sizeof(ac_meta_match_t)) != NGX_OK) {
        luaL_error(L, "native plugin filter cache allocation failed");
    }
    if (!ctx->main->compiled) {
        luaL_error(L, "native core has not been initialized");
    }
    ac_snapshot_acquire(ctx);
    ctx->phase = AC_PHASES;
    ctx->match_opts_ref = LUA_NOREF;
    ctx->matched_ref = LUA_NOREF;
    ctx->span_ref = LUA_NOREF;
    ac_attach_lua_context(L, ctx);
    cleanup->data = ctx;
    cleanup->handler = ac_cleanup;
    ngx_http_set_ctx(request, ctx, ngx_http_apisix_core_module);
    return ctx;
}

void
ac_attach_lua_context(lua_State *L, ac_request_t *ctx)
{
    if (ctx->main->request_pool_native == 1) {
        ctx->table_ref = ac_table_pool_fetch(L, ctx->main,
                                            ctx->main->request_pool_state, 32);
    } else {
        if (ctx->main->request_pool == 1) {
            ac_require(L, "tablepool");
            lua_getfield(L, -1, "fetch");
            lua_pushliteral(L, "native_api_ctx");
            lua_pushinteger(L, 0);
            lua_pushinteger(L, 32);
            lua_call(L, 3, 1);
            lua_remove(L, -2);
        } else {
            lua_createtable(L, 0, 32);
        }
        ctx->table_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    lua_getglobal(L, "ngx");
    lua_getfield(L, -1, "ctx");
    ac_push(L, ctx->table_ref);
    lua_pushvalue(L, -2);
    lua_setfield(L, -2, "ngx_ctx");
    lua_setfield(L, -2, "api_ctx");
    lua_pop(L, 2);
    ac_sdk_context(L, ctx);
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
        {"native_log_start", ac_pipeline_log_start},
        {"shutdown", ac_shutdown}, {"context", ac_context},
        {"enter", ac_enter}, {"next", ac_next},
        {"complete", ac_complete}, {"leave", ac_leave},
        {"start_phase", ac_phase_start}, {"step", ac_phase_step},
        {"encode", ac_encode}, {"render", ac_render},
        {"balance_enter", ac_balance_enter}, {"balance_finish", ac_balance_finish},
        {"balance_start", ac_balance_start}, {NULL, NULL}
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
        main->request_pool_state = ngx_pcalloc(cf->pool, sizeof(ac_table_pool_t));
        main->matched_pool_state = ngx_pcalloc(cf->pool, sizeof(ac_table_pool_t));
        if (!main->request_pool_state || !main->matched_pool_state) return NULL;
        main->pool = cf->pool;
        main->native_filters = NGX_CONF_UNSET;
        main->native_key_auth = NGX_CONF_UNSET;
        main->native_pipeline = NGX_CONF_UNSET;
        main->lua_log_bridge = NGX_CONF_UNSET;
        main->lazy_rate_json = NGX_CONF_UNSET;
        main->rate_variable_index = NGX_ERROR;
        main->log_var_plan = NGX_CONF_UNSET;
        main->fused_balancer = NGX_CONF_UNSET;
        main->batch_vars = NGX_CONF_UNSET;
        main->http_var_plan = NGX_CONF_UNSET;
        main->lazy_route_opts = NGX_CONF_UNSET;
        main->route_candidates = NGX_CONF_UNSET;
        main->request_pool = NGX_CONF_UNSET;
        main->request_pool_native = NGX_CONF_UNSET;
        main->matched_pool = NGX_CONF_UNSET;
        main->balancer_peer_plan = NGX_CONF_UNSET;
        main->lazy_meta_matches = NGX_CONF_UNSET;
        main->phase_index = NGX_CONF_UNSET;
        main->phase_context = NGX_CONF_UNSET;
        main->request_header_flags = NGX_CONF_UNSET;
        main->header_scratch = NGX_CONF_UNSET;
        main->plain_uri = NGX_CONF_UNSET;
        main->sdk_cache_replay = NGX_CONF_UNSET;
        main->native_var_writes = NGX_CONF_UNSET;
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
    ac_main_conf_t *main = ngx_http_conf_get_module_main_conf(cf, ngx_http_apisix_core_module);
    if (main->native_pipeline == NGX_CONF_UNSET) main->native_pipeline = 0;
    if (main->lua_log_bridge == NGX_CONF_UNSET) main->lua_log_bridge = 0;
    if (main->lua_log_bridge && !main->native_pipeline) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "Lua log bridge requires native_pipeline");
        return NGX_ERROR;
    }
    if (main->native_key_auth == NGX_CONF_UNSET) main->native_key_auth = 0;
    if (main->native_pipeline) main->native_key_auth = 1;
    if (main->fused_balancer == NGX_CONF_UNSET) main->fused_balancer = 0;
    if (main->batch_vars == NGX_CONF_UNSET) main->batch_vars = 0;
    if (main->http_var_plan == NGX_CONF_UNSET) main->http_var_plan = 0;
    if (main->lazy_route_opts == NGX_CONF_UNSET) main->lazy_route_opts = 0;
    if (main->route_candidates == NGX_CONF_UNSET) main->route_candidates = 0;
    if (main->request_pool == NGX_CONF_UNSET) main->request_pool = 0;
    if (main->request_pool_native == NGX_CONF_UNSET) main->request_pool_native = 0;
    if (main->matched_pool == NGX_CONF_UNSET) main->matched_pool = 0;
    if (main->balancer_peer_plan == NGX_CONF_UNSET) main->balancer_peer_plan = 0;
    if (main->lazy_meta_matches == NGX_CONF_UNSET) main->lazy_meta_matches = 0;
    if (main->phase_index == NGX_CONF_UNSET) main->phase_index = 0;
    if (main->phase_context == NGX_CONF_UNSET) main->phase_context = 0;
    if (main->request_header_flags == NGX_CONF_UNSET) main->request_header_flags = 0;
    if (main->header_scratch == NGX_CONF_UNSET) main->header_scratch = 0;
    if (main->plain_uri == NGX_CONF_UNSET) main->plain_uri = 0;
    if (main->sdk_cache_replay == NGX_CONF_UNSET) main->sdk_cache_replay = 0;
    if (main->native_var_writes == NGX_CONF_UNSET) main->native_var_writes = 0;
    if (ngx_http_lua_add_package_preload(cf, "apisix.native.engine", ac_open) != NGX_OK) {
        return NGX_ERROR;
    }
    if (ac_request_variables(cf) != NGX_OK) return NGX_ERROR;
    if (ac_http_register(cf) != NGX_OK) return NGX_ERROR;
    if (ac_helpers_register(cf) != NGX_OK) return NGX_ERROR;
    if (ac_script_register(cf) != NGX_OK) return NGX_ERROR;
    if (ac_snapshot_register(cf) != NGX_OK) return NGX_ERROR;
    if (ac_sdk_register(cf) != NGX_OK) return NGX_ERROR;
    if (ac_pipeline_register(cf) != NGX_OK) return NGX_ERROR;
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
