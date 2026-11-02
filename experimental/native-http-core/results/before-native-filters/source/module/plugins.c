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

static ac_plugin_t *
ac_find_plugin(ac_main_conf_t *main, const char *name, size_t length)
{
    ac_plugin_t *plugins = main->plugins.elts;
    ngx_uint_t i;
    for (i = 0; i < main->plugins.nelts; i++) {
        if (plugins[i].name.len == length
            && ngx_memcmp(plugins[i].name.data, name, length) == 0) {
            return &plugins[i];
        }
    }
    return NULL;
}

static int
ac_compare_calls(const void *a, const void *b)
{
    const ac_call_t *left = a, *right = b;
    if (left->priority == right->priority) {
        return 0;
    }
    return left->priority > right->priority ? -1 : 1;
}

static ngx_flag_t
ac_plan_has_phase(ac_plan_t *plan, ac_phase_e phase)
{
    ac_call_t *calls = plan->calls.elts;
    ngx_uint_t i;
    for (i = 0; i < plan->calls.nelts; i++) {
        if (calls[i].plugin->callbacks[phase] != LUA_REFNIL
            && calls[i].plugin->callbacks[phase] != LUA_NOREF) return 1;
    }
    return 0;
}

void
ac_compile_plan(lua_State *L, ac_main_conf_t *main, ac_plan_t *plan,
    ngx_pool_t *pool, int plugins, ngx_flag_t validate)
{
    ac_plugin_t *plugin;
    ac_call_t *call;
    const char *name, *source;
    size_t length;
    int meta;
    plugins = ac_abs(L, plugins);
    if (ngx_array_init(&plan->calls, pool, 4, sizeof(ac_call_t)) != NGX_OK) {
        luaL_error(L, "native plugin plan allocation failed");
    }
    if (lua_isnil(L, plugins)) {
        return;
    }
    luaL_checktype(L, plugins, LUA_TTABLE);
    lua_pushnil(L);
    while (lua_next(L, plugins)) {
        name = luaL_checklstring(L, -2, &length);
        plugin = ac_find_plugin(main, name, length);
        if (plugin == NULL) {
            luaL_error(L, "plugin '%s' is not enabled", name);
        }
        luaL_checktype(L, -1, LUA_TTABLE);
        lua_getfield(L, -1, "_meta");
        meta = lua_gettop(L);
        if (lua_istable(L, meta) && ac_bool_field(L, meta, "disable")) {
            lua_pop(L, 2);
            continue;
        }
        call = ngx_array_push(&plan->calls);
        if (call == NULL) {
            luaL_error(L, "native plugin call allocation failed");
        }
        ngx_memzero(call, sizeof(*call));
        call->plugin = plugin;
        call->priority = plugin->priority;
        call->conf_ref = ac_ref(L, -2);
        call->pre_ref = LUA_NOREF;
        if (lua_istable(L, meta)) {
            lua_getfield(L, meta, "filter");
            if (!lua_isnil(L, -1)) call->expression = ac_expr_compile(L, pool, -1);
            lua_pop(L, 1);
            lua_getfield(L, meta, "priority");
            if (!lua_isnil(L, -1)) {
                call->priority = luaL_checknumber(L, -1);
            }
            lua_pop(L, 1);
            lua_getfield(L, meta, "pre_function");
            if (lua_isstring(L, -1)) {
                source = luaL_checklstring(L, -1, &length);
                lua_pushliteral(L, "return ");
                lua_pushlstring(L, source, length);
                lua_concat(L, 2);
                source = lua_tolstring(L, -1, &length);
                if (luaL_loadbuffer(L, source, length, "native_meta_pre") != 0
                    || lua_pcall(L, 0, 1, 0) != 0) {
                    luaL_error(L, "invalid plugin pre_function: %s", lua_tostring(L, -1));
                }
                luaL_checktype(L, -1, LUA_TFUNCTION);
                call->pre_ref = ac_ref(L, -1);
                lua_pop(L, 2);
            }
            lua_pop(L, 1);
        }
        ac_push(L, plugin->module_ref);
        lua_getfield(L, -1, "check_schema");
        if (validate && lua_isfunction(L, -1)) {
            ac_push(L, call->conf_ref);
            if (lua_pcall(L, 1, 2, 0) != 0) {
                luaL_error(L, "plugin schema raised: %s", lua_tostring(L, -1));
            }
            if (!lua_toboolean(L, -2)) {
                luaL_error(L, "invalid plugin configuration: %s", lua_tostring(L, -1));
            }
            lua_pop(L, 2);
        } else {
            lua_pop(L, 1);
        }
        lua_pop(L, 3);
    }
    ngx_qsort(plan->calls.elts, plan->calls.nelts, sizeof(ac_call_t), ac_compare_calls);
}

void
ac_compile_plugins(lua_State *L, ac_main_conf_t *main, int config)
{
    ngx_uint_t i, j, k;
    int list, module;
    ac_plugin_t *plugin, *loaded;
    const char *name;
    size_t length;
    config = ac_abs(L, config);
    if (ngx_array_init(&main->plugins, main->pool, 16,
                       sizeof(ac_plugin_t)) != NGX_OK) {
        luaL_error(L, "native plugin registry allocation failed");
    }
    lua_getfield(L, config, "plugins");
    list = lua_gettop(L);
    luaL_checktype(L, list, LUA_TTABLE);
    for (i = 1; i <= lua_objlen(L, list); i++) {
        lua_rawgeti(L, list, (int) i);
        name = luaL_checklstring(L, -1, &length);
        if (ac_find_plugin(main, name, length) != NULL) {
            luaL_error(L, "duplicate enabled plugin '%s'", name);
        }
        plugin = ngx_array_push(&main->plugins);
        if (plugin == NULL) {
            luaL_error(L, "native plugin descriptor allocation failed");
        }
        ngx_memzero(plugin, sizeof(*plugin));
        plugin->module_ref = LUA_NOREF;
        plugin->name = ac_string(L, main->pool, -1);
        lua_getglobal(L, "require");
        lua_pushliteral(L, "apisix.plugins.");
        lua_pushvalue(L, -3);
        lua_concat(L, 2);
        if (lua_pcall(L, 1, 1, 0) != 0) {
            luaL_error(L, "failed loading plugin '%s': %s", name, lua_tostring(L, -1));
        }
        module = lua_gettop(L);
        luaL_checktype(L, module, LUA_TTABLE);
        lua_pushlstring(L, (char *) plugin->name.data, plugin->name.len);
        lua_setfield(L, module, "name");
        lua_getfield(L, module, "schema");
        if (lua_istable(L, -1)) {
            int schema = lua_gettop(L);
            lua_getfield(L, schema, "properties");
            if (lua_isnil(L, -1)) {
                lua_pop(L, 1); lua_newtable(L); lua_pushvalue(L, -1); lua_setfield(L, schema, "properties");
            }
            ac_require(L, "apisix.schema_def"); lua_getfield(L, -1, "plugin_injected_schema");
            lua_getfield(L, -1, "_meta"); lua_setfield(L, -4, "_meta");
            lua_getfield(L, -1, "$comment"); lua_setfield(L, schema, "$comment");
            lua_pop(L, 3);
        }
        lua_pop(L, 1);
        lua_getfield(L, module, "priority");
        plugin->priority = luaL_checknumber(L, -1);
        lua_pop(L, 1);
        loaded = main->plugins.elts;
        for (j = 0; j + 1 < main->plugins.nelts; j++) {
            if (loaded[j].priority == plugin->priority) {
                luaL_error(L, "duplicate plugin priority for '%s'", name);
            }
        }
        lua_getfield(L, module, "run_policy");
        plugin->prefer_route = lua_isstring(L, -1)
            && ngx_strcmp(lua_tostring(L, -1), "prefer_route") == 0;
        lua_pop(L, 1);
        lua_getfield(L, module, "type");
        plugin->auth = lua_isstring(L, -1) && ngx_strcmp(lua_tostring(L, -1), "auth") == 0;
        lua_pop(L, 1);
        plugin->module_ref = ac_ref(L, module);
        lua_getfield(L, module, "init");
        if (lua_isfunction(L, -1)) {
            if (lua_pcall(L, 0, 0, 0) != 0) {
                luaL_error(L, "plugin init failed: %s", lua_tostring(L, -1));
            }
        } else {
            lua_pop(L, 1);
        }
        lua_getfield(L, module, "workflow_handler");
        if (lua_isfunction(L, -1)) {
            if (lua_pcall(L, 0, 0, 0) != 0) {
                luaL_error(L, "plugin workflow registration failed: %s", lua_tostring(L, -1));
            }
        } else lua_pop(L, 1);
        for (k = 0; k < AC_PHASES; k++) {
            lua_getfield(L, module, ac_phase_names[k]);
            if (!lua_isnil(L, -1) && !lua_isfunction(L, -1)) {
                luaL_error(L, "invalid plugin callback '%s'", ac_phase_names[k]);
            }
            plugin->callbacks[k] = ac_ref(L, -1);
            lua_pop(L, 1);
        }
        lua_pop(L, 2);
    }
    lua_pop(L, 1);
}

void
ac_compile_globals(lua_State *L, ac_main_conf_t *main, int config)
{
    ngx_uint_t i;
    int merged, seen, rules, values;
    config = ac_abs(L, config);
    lua_newtable(L);
    merged = lua_gettop(L);
    lua_newtable(L);
    seen = lua_gettop(L);
    lua_getfield(L, config, "global_rules");
    rules = lua_gettop(L);
    if (lua_istable(L, rules)) {
        for (i = 1; i <= lua_objlen(L, rules); i++) {
            lua_rawgeti(L, rules, (int) i);
            lua_getfield(L, -1, "plugins");
            values = lua_gettop(L);
            if (lua_istable(L, values)) {
                lua_pushnil(L);
                while (lua_next(L, values)) {
                    lua_pushvalue(L, -2);
                    lua_rawget(L, seen);
                    if (lua_toboolean(L, -1)) {
                        lua_pop(L, 1);
                        lua_pushvalue(L, -2);
                        lua_pushnil(L);
                        lua_rawset(L, merged);
                        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                                      "duplicate global plugin removed");
                    } else {
                        lua_pop(L, 1);
                        lua_pushvalue(L, -2);
                        lua_pushboolean(L, 1);
                        lua_rawset(L, seen);
                        lua_pushvalue(L, -2);
                        lua_pushvalue(L, -2);
                        lua_rawset(L, merged);
                    }
                    lua_pop(L, 1);
                }
            }
            lua_pop(L, 2);
        }
    }
    ac_compile_plan(L, main, &main->global, main->pool, merged, 1);
    lua_pop(L, 3);
}

int
ac_init_worker(lua_State *L)
{
    ac_main_conf_t *main = ac_main();
    ac_plugin_t *plugins;
    ngx_uint_t i;
    if (!main->initialized || main->compiled) {
        return luaL_error(L, "native worker initialization requires one loaded configuration");
    }
    ac_push(L, main->resources_ref);
    ac_require(L, "apisix.core.env"); lua_getfield(L, -1, "init"); lua_call(L, 0, 0); lua_pop(L, 1);
    ac_resolver_worker(L);
    ac_http_worker(L);
    ac_compile_plugins(L, main, -1);
    plugins = main->plugins.elts;
    for (i = 0; i < main->plugins.nelts; i++) {
        ac_push(L, plugins[i].module_ref);
        lua_getfield(L, -1, "init_worker");
        if (lua_isfunction(L, -1)) {
            if (lua_pcall(L, 0, 0, 0) != 0) {
                return luaL_error(L, "plugin init_worker failed: %s", lua_tostring(L, -1));
            }
        } else {
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
    ac_snapshot_worker(L);
    lua_pop(L, 1);
    main->compiled = 1;
    ac_require(L, "apisix.core.lrucache"); lua_getfield(L, -1, "init_worker"); lua_call(L, 0, 0); lua_pop(L, 1);
    for (i = 0; i < main->plugins.nelts; i++) {
        if (plugins[i].name.len == sizeof("prometheus") - 1
            && ngx_memcmp(plugins[i].name.data, "prometheus", sizeof("prometheus") - 1) == 0) {
            ac_require(L, "apisix.plugins.prometheus.exporter");
            lua_getfield(L, -1, "init_exporter_timer"); lua_call(L, 0, 0); lua_pop(L, 1);
            break;
        }
    }
    return 0;
}

int
ac_shutdown(lua_State *L)
{
    ac_main_conf_t *main = ac_main();
    ac_plugin_t *plugins = main->plugins.elts;
    ngx_uint_t i;
    for (i = 0; i < main->plugins.nelts; i++) {
        ac_push(L, plugins[i].module_ref);
        lua_getfield(L, -1, "destroy");
        if (lua_isfunction(L, -1)) {
            if (lua_pcall(L, 0, 0, 0) != 0) {
                ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0,
                              "native plugin destroy failed: %s", lua_tostring(L, -1));
                lua_pop(L, 1);
            }
        } else {
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
    return 0;
}

static ngx_flag_t
ac_route_overrides(lua_State *L, ac_request_t *ctx, ac_call_t *call)
{
    ac_call_t *calls;
    ngx_uint_t i;
    if (!call->plugin->prefer_route || ctx->route == NULL) {
        return 0;
    }
    calls = ctx->route->plan.calls.elts;
    for (i = 0; i < ctx->route->plan.calls.nelts; i++) {
        if (calls[i].plugin == call->plugin) {
            return 1;
        }
    }
    return 0;
}

static ngx_flag_t
ac_enabled(lua_State *L, ac_request_t *ctx, ac_call_t *call)
{
    int table;
    ngx_uint_t i;
    ac_meta_match_t *matches, *match;
    ngx_flag_t allowed = 1;
    ac_push(L, ctx->table_ref);
    table = lua_gettop(L);
    lua_getfield(L, table, "_skip_plugins");
    if (lua_istable(L, -1)) {
        lua_pushlstring(L, (char *) call->plugin->name.data, call->plugin->name.len);
        lua_rawget(L, -2);
        allowed = !lua_toboolean(L, -1);
        lua_pop(L, 1);
    }
    lua_pop(L, 2);
    if (!allowed) {
        return 0;
    }
    if (call->expression) {
        matches = ctx->meta_matches.elts;
        for (i = 0; i < ctx->meta_matches.nelts; i++) {
            if (matches[i].call == call) return matches[i].matched;
        }
        allowed = ac_expr_eval(L, call->expression, ctx);
        match = ngx_array_push(&ctx->meta_matches);
        if (!match) luaL_error(L, "plugin filter match allocation failed");
        match->call = call; match->matched = allowed;
    }
    return allowed;
}

static ac_plan_t *
ac_current_plan(ac_request_t *ctx)
{
    return ctx->stage % 2 == 0 ? &ctx->main->global
                             : (ctx->route ? &ctx->route->plan : NULL);
}

static ngx_flag_t
ac_advance(lua_State *L, ac_request_t *ctx)
{
    ctx->stage++;
    ctx->cursor = 0;
    ctx->script_dispatched = 0;
    if (ctx->stage % 2 != 0) {
        return 1;
    }
    if (ctx->phase == AC_REWRITE) {
        if (!ctx->script_route && !ctx->consumer_processed && ctx->route) {
            ctx->consumer_processed = 1;
            if (ac_consumer_merge(L, ctx)) {
                ctx->consumer_rewrite = 1;
                ctx->stage = 1;
                return 1;
            }
            if (ctx->early_status) return 0;
        }
        ctx->consumer_rewrite = 0;
        ctx->phase = AC_ACCESS;
        return 1;
    }
    if (ctx->phase == AC_ACCESS && ctx->route != NULL) {
        ctx->phase = AC_BEFORE_PROXY;
        return 1;
    }
    if (ctx->phase == AC_BODY_FILTER) {
        ctx->phase = AC_DELAYED_BODY_FILTER;
        return 1;
    }
    return 0;
}

int
ac_enter(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    ngx_http_request_t *r = ngx_http_lua_get_request(L);
    ac_request_t *ctx;
    ngx_uint_t i;
    if (r == NULL) {
        return luaL_error(L, "phase entry requires an HTTP request");
    }
    ctx = ngx_http_get_module_ctx(r, ngx_http_apisix_core_module);
    if (ctx == NULL && ngx_strcmp(name, "access") != 0) {
        lua_pushboolean(L, 0);
        return 1;
    }
    ctx = ac_request(L);
    if (ctx->log_done) {
        lua_pushboolean(L, 0);
        return 1;
    }
    for (i = 0; i < AC_PHASES; i++) {
        if (ngx_strcmp(name, ac_phase_names[i]) == 0) {
            break;
        }
    }
    if (i == AC_PHASES) {
        return luaL_error(L, "unknown native HTTP phase '%s'", name);
    }
    ctx->phase = (i == AC_ACCESS) ? AC_REWRITE : (ac_phase_e) i;
    ctx->stage = 0;
    ctx->cursor = 0;
    ctx->active = NULL;
    ctx->callback_pending = 0;
    ctx->pre_pending = 0;
    ctx->script_dispatched = 0;
    ctx->phase_entered = 1;
    if (i == AC_LOG) ac_trace_finish_all(L, ctx, 0);
    ac_phase_begin(L, ctx, (ac_phase_e) i);
    if (i == AC_ACCESS) {
        ctx->early_status = ac_http_prepare(L, ctx);
        if (!ctx->early_status) ac_match_start(L, ctx);
    } else if ((i == AC_HEADER_FILTER || i == AC_BODY_FILTER) && !ctx->script_route) {
        ngx_flag_t callbacks = ac_plan_has_phase(&ctx->main->global, (ac_phase_e) i)
            || (ctx->route && ac_plan_has_phase(&ctx->route->plan, (ac_phase_e) i));
        if (i == AC_BODY_FILTER) {
            callbacks = callbacks || ac_plan_has_phase(&ctx->main->global, AC_DELAYED_BODY_FILTER)
                || (ctx->route && ac_plan_has_phase(&ctx->route->plan, AC_DELAYED_BODY_FILTER));
        }
        if (!callbacks) {
            ctx->phase_entered = 0;
            ac_phase_finish(L, ctx);
            lua_pushboolean(L, 0); return 1;
        }
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int
ac_return_call(lua_State *L, ac_request_t *ctx, ac_call_t *call, int function_ref,
    ngx_flag_t is_pre)
{
    ctx->active = call;
    ctx->active_pre = is_pre;
    ctx->callback_pending = 1;
    ac_route_context(L, ctx, ctx->stage % 2 == 0);
    ac_push(L, ctx->table_ref);
    if (!is_pre) {
        ac_set_string(L, -1, "_plugin_name", call->plugin->name);
    }
    lua_pop(L, 1);
    ac_push(L, function_ref);
    ac_push(L, call->conf_ref);
    ac_push(L, ctx->table_ref);
    return 3;
}

int
ac_next(lua_State *L)
{
    ac_request_t *ctx = ac_request(L);
    ac_plan_t *plan;
    ac_call_t *calls, *call;
    if (!ctx->phase_entered) {
        if (ctx->phase == AC_BEFORE_PROXY && ac_upstream_enabled(ctx)) {
            int count = ac_upstream_next(L, ctx);
            if (count) return count;
        }
        lua_pushnil(L);
        return 1;
    }
    if (ctx->callback_pending) {
        return luaL_error(L, "native callback must complete before advancing");
    }
    if (ctx->routing) {
        int count = ac_match_next(L, ctx);
        if (count) return count;
    }
    if (ctx->early_status) {
        ctx->phase_entered = 0;
        lua_pushnil(L);
        return 1;
    }
    if (ctx->pre_pending) {
        ctx->pre_pending = 0;
        return ac_return_call(L, ctx, ctx->active,
                             ctx->active->plugin->callbacks[ctx->phase], 0);
    }
    do {
        if (ctx->phase == AC_BEFORE_PROXY && ac_upstream_enabled(ctx) && !ctx->upstream_prepared) {
            int pending = ac_upstream_next(L, ctx);
            if (pending) return pending;
            int results = ac_upstream_prepare(L, ctx);
            if (results) {
                ctx->early_status = (ngx_uint_t) lua_tointeger(L, -results);
                lua_pop(L, results);
                ctx->phase_entered = 0;
                lua_pushnil(L);
                return 1;
            }
            ctx->upstream_prepared = 1;
        }
        plan = ac_current_plan(ctx);
        if (ctx->stage % 2 != 0 && ctx->route) {
            int script = ac_script_next(L, ctx);
            if (script > 0) return script;
            if (script == 0) continue;
        }
        if (plan == NULL) {
            continue;
        }
        calls = plan->calls.elts;
        while (ctx->cursor < plan->calls.nelts) {
            call = &calls[ctx->cursor++];
            if (call->plugin->callbacks[ctx->phase] == LUA_REFNIL
                || (ctx->consumer_rewrite && (!call->consumer_only || call->plugin->auth))
                || (ctx->stage % 2 == 0 && ac_route_overrides(L, ctx, call))
                || !ac_enabled(L, ctx, call)) {
                continue;
            }
            if (call->pre_ref != LUA_NOREF) {
                return ac_return_call(L, ctx, call, call->pre_ref, 1);
            }
            return ac_return_call(L, ctx, call,
                                 call->plugin->callbacks[ctx->phase], 0);
        }
    } while (ac_advance(L, ctx));
    ctx->phase_entered = 0;
    if (ctx->phase == AC_BEFORE_PROXY && ac_upstream_enabled(ctx)) {
        int count = ac_upstream_next(L, ctx);
        if (count) return count;
    }
    lua_pushnil(L);
    return 1;
}

int
ac_complete(lua_State *L)
{
    ac_request_t *ctx = ac_request(L);
    int count = lua_gettop(L);
    if (!ctx->callback_pending) {
        return luaL_error(L, "native callback completion without a pending callback");
    }
    ctx->callback_pending = 0;
    if (ctx->script_pending) {
        ctx->script_pending = 0;
        if (!lua_toboolean(L, 1)) { lua_pushvalue(L, 2); return lua_error(L); }
        if (ctx->phase == AC_REWRITE) ctx->script_loaded = 1;
        return 0;
    }
    if (!lua_toboolean(L, 1)) {
        lua_pushvalue(L, 2);
        return lua_error(L);
    }
    if (ctx->filter_pending) {
        ctx->filter_pending = 0;
        ac_match_complete(L, ctx, count >= 2 && lua_toboolean(L, 2));
        return 0;
    }
    if (ctx->dns_pending) {
        ctx->dns_pending = 0;
        ac_upstream_resolved(L, ctx);
        return 0;
    }
    if (ctx->active_pre) {
        ctx->pre_pending = 1;
        return 0;
    }
    ac_push(L, ctx->table_ref);
    lua_pushnil(L);
    lua_setfield(L, -2, "_plugin_name");
    lua_pop(L, 1);
    ac_route_context(L, ctx, 0);
    if (ctx->phase <= AC_BEFORE_PROXY
        && ((count >= 2 && lua_toboolean(L, 2)) || (count >= 3 && lua_toboolean(L, 3)))) {
        lua_Number status = luaL_checknumber(L, 2);
        if (status >= 400) {
            ac_push(L, ctx->active->conf_ref);
            lua_getfield(L, -1, "_meta");
            if (lua_istable(L, -1)) {
                lua_getfield(L, -1, "error_response");
                if (!lua_isnil(L, -1)) {
                    lua_pushnumber(L, status);
                    lua_insert(L, -2);
                    return 2;
                }
                lua_pop(L, 1);
            }
            lua_settop(L, count);
        }
        lua_pushnumber(L, status);
        if (count >= 3) {
            lua_pushvalue(L, 3);
        } else {
            lua_pushnil(L);
        }
        return 2;
    }
    return 0;
}

int
ac_leave(lua_State *L)
{
    ac_request_t *ctx = ac_request(L);
    if (ctx->callback_pending || ctx->phase_entered) {
        return luaL_error(L, "cannot leave a native phase with pending callbacks");
    }
    if (ctx->phase == AC_LOG) {
        ac_upstream_log(L, ctx);
        ac_phase_finish(L, ctx);
        ac_require(L, "apisix.core.ctx"); lua_getfield(L, -1, "release_vars");
        ac_push(L, ctx->table_ref); lua_call(L, 1, 0); lua_pop(L, 1);
        ac_require(L, "apisix.tracer"); lua_getfield(L, -1, "release");
        ac_push(L, ctx->table_ref); lua_getfield(L, -1, "ngx_ctx"); lua_remove(L, -2); lua_call(L, 1, 0); lua_pop(L, 1);
        ac_push(L, ctx->table_ref); lua_pushnil(L); lua_setfield(L, -2, "ngx_ctx"); lua_pop(L, 1);
        ctx->log_done = 1;
    } else {
        ac_phase_finish(L, ctx);
    }
    if (ctx->early_status && ctx->phase <= AC_BEFORE_PROXY) {
        lua_pushinteger(L, (lua_Integer) ctx->early_status);
        ctx->early_status = 0;
        return 1;
    }
    if (ctx->phase == AC_ACCESS && ctx->route == NULL) {
        lua_pushinteger(L, NGX_HTTP_NOT_FOUND);
        lua_newtable(L);
        lua_pushliteral(L, "404 Route Not Found");
        lua_setfield(L, -2, "error_msg");
        return 2;
    }
    if (ctx->phase == AC_BEFORE_PROXY && ac_upstream_enabled(ctx)) {
        ac_push(L, ctx->table_ref); lua_getfield(L, -1, "_resp_source");
        if (!lua_toboolean(L, -1)) { lua_pushboolean(L, 1); lua_setfield(L, -3, "_apisix_proxied"); }
        lua_pop(L, 2);
    }
    return 0;
}

/* Keep completion, scheduling and finalization in one native transition.
 * The Lua frame only invokes the returned callable and transports its tuple. */
static int
ac_phase_next(lua_State *L)
{
    int count;
    lua_settop(L, 0);
    count = ac_next(L);
    if (lua_isfunction(L, -count)) return count;
    lua_settop(L, 0);
    count = ac_leave(L);
    lua_pushnil(L);
    if (count) lua_insert(L, -(count + 1));
    return count + 1;
}

int
ac_phase_start(lua_State *L)
{
    ac_enter(L);
    if (!lua_toboolean(L, -1)) { lua_pushnil(L); return 1; }
    return ac_phase_next(L);
}

int
ac_phase_step(lua_State *L)
{
    int count = ac_complete(L);
    if (count) {
        lua_pushnil(L); lua_insert(L, -(count + 1)); return count + 1;
    }
    return ac_phase_next(L);
}
