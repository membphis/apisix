/* SPDX-License-Identifier: Apache-2.0 */
#include "engine.h"

static void
ac_time_nano(lua_State *L)
{
    ngx_time_update(); ac_require(L, "opentelemetry.util"); lua_getfield(L, -1, "time_nano");
    lua_call(L, 0, 1); lua_remove(L, -2);
}

static void
ac_pool_fetch(lua_State *L, const char *name, int array, int hash)
{
    ac_require(L, "tablepool"); lua_getfield(L, -1, "fetch"); lua_pushstring(L, name);
    lua_pushinteger(L, array); lua_pushinteger(L, hash); lua_call(L, 3, 1); lua_remove(L, -2);
}

static void
ac_pool_release(lua_State *L, const char *name, int table)
{
    table = ac_abs(L, table); ac_require(L, "tablepool"); lua_getfield(L, -1, "release");
    lua_pushstring(L, name); lua_pushvalue(L, table); lua_call(L, 2, 0); lua_pop(L, 1);
}

static int
ac_span_new(lua_State *L)
{
    int tracing, spans, self, parent, children, id;
    lua_settop(L, 3); lua_getfield(L, 1, "tracing"); tracing = lua_gettop(L);
    ac_pool_fetch(L, "opentelemetry_span", 0, 16); self = lua_gettop(L);
    ac_time_nano(L); lua_setfield(L, self, "start_time");
    lua_pushvalue(L, 2); lua_setfield(L, self, "name"); lua_pushvalue(L, 3); lua_setfield(L, self, "kind");
    lua_getfield(L, tracing, "spans"); spans = lua_gettop(L); id = (int) lua_objlen(L, spans) + 1;
    lua_pushvalue(L, self); lua_rawseti(L, spans, id); lua_pushinteger(L, id); lua_setfield(L, self, "id");
    lua_getfield(L, tracing, "current_span"); parent = lua_gettop(L);
    if (lua_istable(L, parent)) {
        lua_getfield(L, parent, "id"); lua_setfield(L, self, "parent_id");
        lua_getfield(L, parent, "child_ids");
        if (lua_isnil(L, -1)) { lua_pop(L, 1); lua_createtable(L, 10, 0); lua_pushvalue(L, -1); lua_setfield(L, parent, "child_ids"); }
        children = lua_gettop(L); lua_pushinteger(L, id); lua_rawseti(L, children, (int) lua_objlen(L, children) + 1); lua_pop(L, 1);
    } else { lua_pushvalue(L, self); lua_setfield(L, tracing, "root_span"); }
    lua_pushvalue(L, self); lua_setfield(L, tracing, "current_span");
    lua_newtable(L); lua_pushvalue(L, lua_upvalueindex(1)); lua_setfield(L, -2, "__index"); lua_setmetatable(L, self);
    lua_pushvalue(L, self); return 1;
}

static int
ac_span_status(lua_State *L)
{
    int status;
    lua_settop(L, 3); ac_require(L, "opentelemetry.trace.span_status"); lua_getfield(L, -1, "validate");
    lua_pushvalue(L, 2); lua_call(L, 1, 1); lua_replace(L, 2); lua_pop(L, 1);
    lua_getfield(L, 1, "status"); status = lua_gettop(L);
    if (lua_isnil(L, status)) { lua_pop(L, 1); lua_newtable(L); lua_pushvalue(L, -1); lua_setfield(L, 1, "status");
        lua_pushliteral(L, ""); lua_setfield(L, status, "message"); }
    lua_pushvalue(L, 2); lua_setfield(L, status, "code");
    ac_require(L, "opentelemetry.trace.span_status"); lua_getfield(L, -1, "ERROR");
    if (lua_equal(L, 2, -1)) { lua_pushvalue(L, 3); lua_setfield(L, status, "message"); }
    return 0;
}

static int
ac_span_attributes(lua_State *L)
{
    int n = lua_gettop(L), i, count;
    lua_getfield(L, 1, "attributes");
    if (lua_isnil(L, -1)) { lua_pop(L, 1); lua_createtable(L, 10, 0); lua_pushvalue(L, -1); lua_setfield(L, 1, "attributes"); }
    count = (int) lua_objlen(L, -1);
    for (i = 2; i <= n; i++) { lua_pushvalue(L, i); lua_rawseti(L, n + 1, ++count); }
    return 0;
}

static int
ac_span_finish(lua_State *L)
{
    int tracing;
    lua_getfield(L, 2, "tracing"); tracing = lua_gettop(L);
    ac_time_nano(L); lua_setfield(L, 1, "end_time");
    lua_getfield(L, 1, "parent_id");
    if (!lua_toboolean(L, -1)) return 0;
    lua_getfield(L, tracing, "spans"); lua_rawgeti(L, -1, (int) lua_tointeger(L, -2));
    lua_setfield(L, tracing, "current_span"); return 0;
}

static int
ac_span_release(lua_State *L)
{
    ac_pool_release(L, "opentelemetry_span", 1); return 0;
}

static int
ac_span_open(lua_State *L)
{
    static const luaL_Reg methods[] = {{"set_status", ac_span_status}, {"set_attributes", ac_span_attributes},
        {"finish", ac_span_finish}, {"release", ac_span_release}, {NULL, NULL}};
    lua_newtable(L); luaL_register(L, NULL, methods);
    lua_pushvalue(L, -1); lua_pushcclosure(L, ac_span_new, 1); lua_setfield(L, -2, "new"); return 1;
}

static int
ac_noop_method(lua_State *L)
{
    /* Disabled tracing intentionally has no effects, matching noop_span.lua. */
    return 0;
}

static int
ac_noop_new(lua_State *L)
{
    lua_newtable(L); lua_newtable(L); lua_pushvalue(L, lua_upvalueindex(1));
    lua_setfield(L, -2, "__index"); lua_setmetatable(L, -2); return 1;
}

static int
ac_noop_open(lua_State *L)
{
    static const char *methods[] = {"set_status", "set_attributes", "finish", "release"};
    lua_newtable(L);
    for (int i = 0; i < 4; i++) { lua_pushinteger(L, i); lua_pushcclosure(L, ac_noop_method, 1); lua_setfield(L, -2, methods[i]); }
    lua_pushvalue(L, -1); lua_pushcclosure(L, ac_noop_new, 1); lua_setfield(L, -2, "new"); return 1;
}

static void
ac_span_make(lua_State *L, int ctx, const char *name, int kind)
{
    ctx = ac_abs(L, ctx); kind = ac_abs(L, kind);
    ac_require(L, "apisix.utils.span"); lua_getfield(L, -1, "new");
    lua_pushvalue(L, ctx); lua_pushstring(L, name);
    if (kind) lua_pushvalue(L, kind); else lua_pushnil(L);
    lua_call(L, 3, 1); lua_remove(L, -2);
}

static int
ac_trace_start(lua_State *L)
{
    int tracing;
    lua_settop(L, 3);
    if (!ac_main()->tracing) { lua_pushvalue(L, lua_upvalueindex(1)); return 1; }
    lua_pushliteral(L, "tracing"); lua_rawget(L, 1);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1); ac_pool_fetch(L, "tracing", 0, 8); tracing = lua_gettop(L);
        ac_pool_fetch(L, "tracing_spans", 20, 0); lua_setfield(L, tracing, "spans");
        lua_pushliteral(L, "tracing"); lua_pushvalue(L, tracing); lua_rawset(L, 1);
        ac_span_make(L, 1, "root", 0); lua_pop(L, 1);
    }
    tracing = lua_gettop(L);
    if (ac_bool_field(L, tracing, "skip")) { lua_pushvalue(L, lua_upvalueindex(1)); return 1; }
    ac_require(L, "apisix.utils.span"); lua_getfield(L, -1, "new");
    lua_pushvalue(L, 1); lua_pushvalue(L, 2); lua_pushvalue(L, 3); lua_call(L, 3, 1); return 1;
}

static int
ac_trace_all(lua_State *L)
{
    int tracing, span;
    lua_settop(L, 3); lua_getfield(L, 1, "tracing"); tracing = lua_gettop(L);
    if (!lua_istable(L, tracing)) return 0;
    lua_getfield(L, tracing, "current_span"); span = lua_gettop(L);
    if (!lua_istable(L, span)) return 0;
    lua_pushcfunction(L, ac_span_status); lua_pushvalue(L, span); lua_pushvalue(L, 2); lua_pushvalue(L, 3); lua_call(L, 3, 0);
    do {
        lua_pushcfunction(L, ac_span_finish); lua_pushvalue(L, span); lua_pushvalue(L, 1); lua_call(L, 2, 0);
        lua_getfield(L, tracing, "current_span"); lua_replace(L, span);
        lua_getfield(L, span, "parent_id"); int parent = lua_toboolean(L, -1); lua_pop(L, 1);
        if (!parent) break;
    } while (1);
    return 0;
}

static int
ac_trace_release(lua_State *L)
{
    int tracing, spans, i;
    lua_getfield(L, 1, "tracing"); tracing = lua_gettop(L);
    if (!lua_istable(L, tracing)) return 0;
    lua_getfield(L, tracing, "spans"); spans = lua_gettop(L);
    for (i = 1; i <= (int) lua_objlen(L, spans); i++) { lua_rawgeti(L, spans, i); ac_pool_release(L, "opentelemetry_span", -1); lua_pop(L, 1); }
    ac_pool_release(L, "tracing_spans", spans); ac_pool_release(L, "tracing", tracing);
    lua_pushliteral(L, "tracing"); lua_pushnil(L); lua_rawset(L, 1); return 0;
}

static int
ac_tracer_open(lua_State *L)
{
    lua_newtable(L); int module = lua_gettop(L);
    ac_require(L, "opentelemetry.trace.span_kind"); lua_setfield(L, module, "kind");
    ac_require(L, "opentelemetry.trace.span_status"); lua_setfield(L, module, "status");
    ac_require(L, "apisix.utils.noop_span"); lua_getfield(L, -1, "new"); lua_call(L, 0, 1); lua_remove(L, -2);
    lua_pushcclosure(L, ac_trace_start, 1); lua_setfield(L, module, "start");
    lua_pushcfunction(L, ac_trace_all); lua_setfield(L, module, "finish_all");
    lua_pushcfunction(L, ac_trace_release); lua_setfield(L, module, "release"); return 1;
}

void
ac_trace_finish_all(lua_State *L, ac_request_t *ctx, ngx_uint_t code)
{
    if (!ctx->main->tracing) return;
    ac_require(L, "apisix.tracer"); lua_getfield(L, -1, "finish_all");
    ac_push(L, ctx->table_ref); lua_getfield(L, -1, "ngx_ctx"); lua_remove(L, -2);
    if (code >= 400) {
        ac_require(L, "opentelemetry.trace.span_status"); lua_getfield(L, -1, "ERROR"); lua_remove(L, -2);
        lua_pushfstring(L, "response code %d", (int) code);
    } else { lua_pushnil(L); lua_pushnil(L); }
    lua_call(L, 3, 0); lua_pop(L, 1);
}

void
ac_phase_begin(lua_State *L, ac_request_t *ctx, ac_phase_e phase)
{
    if (phase == AC_HEADER_FILTER) {
        ngx_str_t header = ngx_string("Server"), name = ngx_string("upstream_status");
        ngx_str_t value = ctx->main->server_tokens ? (ngx_str_t) ngx_string("APISIX/3.19.0")
                                                  : (ngx_str_t) ngx_string("APISIX");
        ngx_http_variable_value_t *status;
        ac_native_header_set(L, ctx->request, header, value, 1, 0);
        status = ngx_http_get_variable(ctx->request, &name, ngx_hash_key(name.data, name.len));
        if (status && !status->not_found) {
            ngx_int_t last = status->len >= 3 ? ngx_atoi(status->data + status->len - 3, 3) : -1;
            if (ctx->main->show_upstream_status || (last >= 500 && last <= 599)) {
                ngx_str_set(&header, "X-APISIX-Upstream-Status");
                value.data = status->data; value.len = status->len;
                ac_native_header_set(L, ctx->request, header, value, 1, 0);
            }
        }
        if (ctx->request->headers_out.status == 101) {
            ac_push(L, ctx->table_ref); lua_getfield(L, -1, "var"); lua_pushliteral(L, "websocket"); lua_setfield(L, -2, "request_type"); lua_pop(L, 2);
        }
    }
    if (!ctx->main->tracing) return;
    luaL_unref(L, LUA_REGISTRYINDEX, ctx->span_ref); ctx->span_ref = LUA_NOREF;
    ac_require(L, "apisix.tracer"); int module = lua_gettop(L); lua_getfield(L, module, "start");
    ac_push(L, ctx->table_ref); lua_getfield(L, -1, "ngx_ctx"); lua_remove(L, -2);
    lua_pushfstring(L, "apisix.phase.%s", ac_phase_names[phase]); lua_getfield(L, module, "kind"); lua_getfield(L, -1, "server"); lua_remove(L, -2);
    lua_call(L, 3, 1); ctx->span_ref = ac_ref(L, -1); lua_pop(L, 2);
}

void
ac_phase_finish(lua_State *L, ac_request_t *ctx)
{
    if (ctx->span_ref == LUA_NOREF) return;
    ac_push(L, ctx->span_ref); lua_getfield(L, -1, "finish"); lua_pushvalue(L, -2);
    ac_push(L, ctx->table_ref); lua_getfield(L, -1, "ngx_ctx"); lua_remove(L, -2); lua_call(L, 2, 0); lua_pop(L, 1);
    luaL_unref(L, LUA_REGISTRYINDEX, ctx->span_ref); ctx->span_ref = LUA_NOREF;
}

ngx_int_t
ac_tracing_register(ngx_conf_t *cf)
{
    if (ngx_http_lua_add_package_preload(cf, "apisix.tracer", ac_tracer_open) != NGX_OK
        || ngx_http_lua_add_package_preload(cf, "apisix.utils.span", ac_span_open) != NGX_OK
        || ngx_http_lua_add_package_preload(cf, "apisix.utils.noop_span", ac_noop_open) != NGX_OK) return NGX_ERROR;
    return NGX_OK;
}
