/* SPDX-License-Identifier: Apache-2.0 */
#include "engine.h"

static ngx_flag_t
ac_literal_ip(lua_State *L, int host)
{
    ngx_flag_t literal;
    host = ac_abs(L, host);
    ac_require(L, "resty.ipmatcher"); lua_getfield(L, -1, "parse_ipv4");
    lua_pushvalue(L, host); lua_call(L, 1, 1); literal = lua_toboolean(L, -1); lua_pop(L, 1);
    if (!literal) {
        lua_getfield(L, -1, "parse_ipv6"); lua_pushvalue(L, host);
        lua_call(L, 1, 1); literal = lua_toboolean(L, -1); lua_pop(L, 1);
    }
    lua_pop(L, 1); return literal;
}

int
ac_filter_upstream(lua_State *L)
{
    int nodes, array, i, host, port, node;
    if (!lua_toboolean(L, 1)) return 0;
    lua_settop(L, 2);
    if (lua_toboolean(L, 2)) {
        lua_getfield(L, 2, "key"); lua_setfield(L, 1, "resource_key");
        lua_getfield(L, 2, "modifiedIndex");
        if (!lua_toboolean(L, -1)) { lua_pop(L, 1); lua_getfield(L, 1, "modifiedIndex"); }
        lua_setfield(L, 1, "resource_version");
        lua_getfield(L, 2, "value"); lua_getfield(L, -1, "id"); lua_remove(L, -2);
    } else {
        lua_pushnil(L); lua_setfield(L, 1, "resource_key");
        lua_getfield(L, 1, "modifiedIndex"); lua_setfield(L, 1, "resource_version");
        lua_getfield(L, 1, "id");
    }
    lua_setfield(L, 1, "resource_id");
    lua_getfield(L, 1, "nodes"); nodes = lua_gettop(L);
    if (!lua_toboolean(L, nodes)) return 0;
    ac_require(L, "table.isarray"); lua_pushvalue(L, nodes); lua_call(L, 1, 1);
    array = lua_toboolean(L, -1); lua_pop(L, 1);
    if (array) {
        for (i = 1; i <= (int) lua_objlen(L, nodes); i++) {
            lua_rawgeti(L, nodes, i); lua_getfield(L, -1, "host");
            if (!ac_literal_ip(L, -1)) { lua_pushboolean(L, 1); lua_setfield(L, 2, "has_domain"); }
            lua_pop(L, 2);
        }
    } else {
        lua_newtable(L); array = lua_gettop(L); i = 0;
        lua_pushnil(L);
        while (lua_next(L, nodes)) {
            ac_require(L, "apisix.core.utils"); lua_getfield(L, -1, "parse_addr"); lua_remove(L, -2);
            lua_pushvalue(L, -3); lua_call(L, 1, 2); host = lua_gettop(L) - 1; port = host + 1;
            if (!ac_literal_ip(L, host)) { lua_pushboolean(L, 1); lua_setfield(L, 2, "has_domain"); }
            lua_newtable(L); node = lua_gettop(L);
            lua_pushvalue(L, host); lua_setfield(L, node, "host");
            lua_pushvalue(L, port); lua_setfield(L, node, "port");
            lua_pushvalue(L, host - 1); lua_setfield(L, node, "weight");
            lua_rawseti(L, array, ++i); lua_pop(L, 3);
        }
        lua_pushvalue(L, array); lua_setfield(L, 1, "nodes"); lua_pop(L, 1);
    }
    if (ac_bool_field(L, 2, "has_domain")) {
        lua_getfield(L, 1, "nodes"); lua_setfield(L, 1, "dns_nodes");
    }
    return 0;
}

static int
ac_resource_latest(lua_State *L)
{
    static const char *kinds[] = {"upstreams", "routes", "services", "stream_routes",
        "plugin_configs", "global_rules", "consumers", "consumer_groups", NULL};
    const u_char *path;
    size_t length, end, slash, i;
    path = (u_char *) luaL_checklstring(L, 1, &length);
    for (end = 0; end < length && path[end] != '#'; end++) { }
    for (slash = 1; slash < end && path[slash] != '/'; slash++) { }
    if (end < 4 || path[0] != '/' || slash == end || slash + 1 == end
        || ngx_strlchr((u_char *) path + slash + 1, (u_char *) path + end, '/')) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "invalid resource path");
        lua_pushnil(L); return 1;
    }
    for (i = 0; kinds[i]; i++) {
        if (ngx_strlen(kinds[i]) == slash - 1 && ngx_memcmp(path + 1, kinds[i], slash - 1) == 0) break;
    }
    if (!kinds[i]) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "unsupported resource type");
        lua_pushnil(L); return 1;
    }
    lua_pushlstring(L, (char *) path + slash + 1, end - slash - 1);
    ac_resource_get(L, ac_main(), kinds[i], -1); return 1;
}

static int
ac_resource_nodes(lua_State *L, ngx_flag_t set)
{
    int upstream;
    lua_pushcfunction(L, ac_resource_latest); lua_pushvalue(L, 1); lua_call(L, 1, 1);
    lua_getfield(L, -1, "value"); lua_getfield(L, -1, "upstream");
    if (!lua_toboolean(L, -1)) { lua_pop(L, 1); lua_pushvalue(L, -1); }
    upstream = lua_gettop(L);
    if (!set) { lua_getfield(L, upstream, "_nodes_ver"); return 1; }
    lua_pushvalue(L, 2); lua_setfield(L, upstream, "_nodes_ver");
    lua_pushvalue(L, 3); lua_setfield(L, upstream, "nodes"); return 0;
}

static int ac_get_nodes(lua_State *L) { return ac_resource_nodes(L, 0); }
static int ac_set_nodes(lua_State *L) { return ac_resource_nodes(L, 1); }

static int
ac_resource_api(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, ac_resource_latest); lua_setfield(L, -2, "fetch_latest_conf");
    lua_pushcfunction(L, ac_get_nodes); lua_setfield(L, -2, "get_nodes_ver");
    lua_pushcfunction(L, ac_set_nodes); lua_setfield(L, -2, "set_nodes_ver_and_nodes"); return 1;
}

static int
ac_host_less(lua_State *L)
{
    lua_getfield(L, 1, "host"); lua_getfield(L, 2, "host");
    lua_pushboolean(L, lua_lessthan(L, -2, -1)); return 1;
}

static int
ac_compare_nodes(lua_State *L)
{
    static const char *fields[] = {"host", "port", "weight", "priority", "metadata", NULL};
    int old, i, j, left, right;
    if (lua_isnil(L, 1)) goto different;
    lua_getfield(L, 1, "nodes"); old = lua_gettop(L);
    if (lua_equal(L, old, 2)) goto equal;
    if (!lua_istable(L, old)) goto different;
    lua_getfield(L, 1, "original_nodes");
    if (lua_toboolean(L, -1) && lua_objlen(L, old) > 0) lua_replace(L, old);
    else lua_pop(L, 1);
    if (lua_objlen(L, old) != lua_objlen(L, 2)) goto different;
    lua_getglobal(L, "table"); lua_getfield(L, -1, "sort");
    lua_pushvalue(L, old); lua_pushcfunction(L, ac_host_less); lua_call(L, 2, 0);
    lua_getfield(L, -1, "sort"); lua_pushvalue(L, 2); lua_pushcfunction(L, ac_host_less); lua_call(L, 2, 0);
    lua_pop(L, 1);
    for (i = 1; i <= (int) lua_objlen(L, 2); i++) {
        lua_rawgeti(L, old, i); left = lua_gettop(L); lua_rawgeti(L, 2, i); right = lua_gettop(L);
        for (j = 0; fields[j]; j++) {
            lua_getfield(L, left, fields[j]); lua_getfield(L, right, fields[j]);
            if (!lua_equal(L, -2, -1)) goto different;
            lua_pop(L, 2);
        }
        lua_pop(L, 2);
    }
equal:
    lua_pushboolean(L, 1); return 1;
different:
    lua_pushboolean(L, 0); return 1;
}

static int
ac_upstream_version(lua_State *L)
{
    if (!lua_toboolean(L, 1)) return 0;
    lua_settop(L, 2);
    if (!lua_toboolean(L, 2)) { lua_pushliteral(L, ""); lua_replace(L, 2); }
    lua_getglobal(L, "tostring"); lua_pushvalue(L, 2); lua_call(L, 1, 1);
    lua_pushvalue(L, 1); lua_insert(L, -2); lua_concat(L, 2); return 1;
}

static int ac_domain_step(lua_State *L, int state);

static void
ac_finish_dns_span(lua_State *L, int state)
{
    state = ac_abs(L, state);
    lua_getfield(L, state, "span"); lua_getfield(L, -1, "finish");
    lua_pushvalue(L, -2); ac_ngx(L, "ctx", NULL); lua_call(L, 2, 0); lua_pop(L, 1);
}

static int
ac_domain_resolved(lua_State *L)
{
    int state = lua_upvalueindex(1), node, values;
    if (!lua_toboolean(L, 1)) { lua_pushvalue(L, 2); return lua_error(L); }
    lua_settop(L, 3);
    lua_getfield(L, state, "node"); node = lua_gettop(L);
    if (lua_toboolean(L, 2)) {
        ac_require(L, "table.clone"); lua_pushvalue(L, node); lua_call(L, 1, 1);
        lua_pushvalue(L, 2); lua_setfield(L, -2, "host");
        lua_getfield(L, node, "host"); lua_setfield(L, -2, "domain");
        lua_getfield(L, state, "values"); values = lua_gettop(L);
        lua_pushvalue(L, -2); lua_rawseti(L, values, (int) lua_objlen(L, values) + 1); lua_pop(L, 2);
    }
    if (lua_toboolean(L, 3)) {
        ac_ngx(L, NULL, "log"); lua_pushinteger(L, NGX_LOG_ERR);
        lua_pushliteral(L, "dns resolver domain: "); lua_getfield(L, node, "host");
        lua_pushliteral(L, " error: "); lua_pushvalue(L, 3); lua_call(L, 5, 0);
    }
    return ac_domain_step(L, state);
}

static int
ac_domain_step(lua_State *L, int state)
{
    int nodes, node, next, literal, count;
    state = ac_abs(L, state);
    lua_getfield(L, state, "nodes"); nodes = lua_gettop(L);
    lua_getfield(L, state, "next"); next = (int) lua_tointeger(L, -1); lua_pop(L, 1);
    count = (int) lua_objlen(L, nodes);
    while (next <= count) {
        lua_rawgeti(L, nodes, next++); node = lua_gettop(L);
        ac_require(L, "resty.ipmatcher"); lua_getfield(L, -1, "parse_ipv4");
        lua_getfield(L, node, "host"); lua_call(L, 1, 1); literal = lua_toboolean(L, -1); lua_pop(L, 1);
        if (!literal) {
            lua_getfield(L, -1, "parse_ipv6"); lua_getfield(L, node, "host");
            lua_call(L, 1, 1); literal = lua_toboolean(L, -1); lua_pop(L, 1);
        }
        lua_pop(L, 1);
        if (literal) {
            lua_getfield(L, state, "values"); lua_pushvalue(L, node);
            lua_rawseti(L, -2, (int) lua_objlen(L, -2) + 1); lua_pop(L, 2); continue;
        }
        lua_pushinteger(L, next); lua_setfield(L, state, "next");
        lua_pushvalue(L, node); lua_setfield(L, state, "node");
        lua_pushvalue(L, state); lua_pushcclosure(L, ac_domain_resolved, 1);
        ac_require(L, "apisix.core.resolver"); lua_getfield(L, -1, "parse_domain"); lua_remove(L, -2);
        lua_newtable(L); lua_getfield(L, node, "host"); lua_rawseti(L, -2, 1);
        lua_pushinteger(L, 1); lua_setfield(L, -2, "n"); return 3;
    }
    ac_finish_dns_span(L, state);
    lua_pushnil(L); lua_getfield(L, state, "values"); return 2;
}

static int
ac_parse_nodes(lua_State *L)
{
    int state;
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_newtable(L); state = lua_gettop(L);
    lua_pushvalue(L, 1); lua_setfield(L, state, "nodes");
    lua_createtable(L, (int) lua_objlen(L, 1), 0); lua_setfield(L, state, "values");
    lua_pushinteger(L, 1); lua_setfield(L, state, "next");
    ac_require(L, "apisix.tracer"); lua_getfield(L, -1, "start"); ac_ngx(L, "ctx", NULL);
    lua_pushliteral(L, "resolve_dns"); lua_getfield(L, -4, "kind"); lua_getfield(L, -1, "internal");
    lua_remove(L, -2); lua_call(L, 3, 1); lua_setfield(L, state, "span"); lua_pop(L, 1);
    return ac_domain_step(L, state);
}

static int
ac_domains_updated(lua_State *L)
{
    int up = lua_upvalueindex(1), value;
    lua_Number version;
    if (!lua_toboolean(L, 1)) { lua_pushvalue(L, 2); return lua_error(L); }
    if (!lua_toboolean(L, 2)) { lua_pushnil(L); lua_pushnil(L); lua_pushvalue(L, 3); return 3; }
    lua_getfield(L, up, "value"); value = lua_gettop(L);
    lua_pushcfunction(L, ac_compare_nodes); lua_pushvalue(L, value); lua_pushvalue(L, 2); lua_call(L, 2, 1);
    if (!lua_toboolean(L, -1)) {
        lua_pushcfunction(L, ac_get_nodes); lua_getfield(L, value, "resource_key"); lua_call(L, 1, 1);
        version = lua_isnil(L, -1) ? 1 : luaL_checknumber(L, -1) + 1; lua_pop(L, 1);
        lua_pushnumber(L, version); lua_setfield(L, value, "_nodes_ver");
        lua_pushvalue(L, 2); lua_setfield(L, value, "nodes");
        lua_pushcfunction(L, ac_set_nodes); lua_getfield(L, value, "resource_key");
        lua_pushnumber(L, version); lua_pushvalue(L, 2); lua_call(L, 3, 0);
    }
    lua_pushnil(L); lua_pushvalue(L, up); return 2;
}

static int
ac_parse_upstream(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_pushvalue(L, 1); lua_pushcclosure(L, ac_domains_updated, 1);
    ac_require(L, "apisix.utils.upstream"); lua_getfield(L, -1, "parse_domain_for_nodes"); lua_remove(L, -2);
    lua_newtable(L); lua_getfield(L, 1, "value"); lua_getfield(L, -1, "dns_nodes"); lua_remove(L, -2);
    lua_rawseti(L, -2, 1); lua_pushinteger(L, 1); lua_setfield(L, -2, "n"); return 3;
}

static int
ac_upstream_helpers(lua_State *L)
{
    lua_newtable(L);
    lua_pushcfunction(L, ac_compare_nodes); lua_setfield(L, -2, "compare_upstream_node");
    lua_pushcfunction(L, ac_upstream_version); lua_setfield(L, -2, "version");
    ac_bind(L, ac_parse_nodes, 0); lua_setfield(L, -2, "parse_domain_for_nodes");
    ac_bind(L, ac_parse_upstream, 0); lua_setfield(L, -2, "parse_domain_in_up"); return 1;
}

ngx_int_t
ac_helpers_register(ngx_conf_t *cf)
{
    if (ngx_http_lua_add_package_preload(cf, "apisix.resource", ac_resource_api) != NGX_OK) return NGX_ERROR;
    return ngx_http_lua_add_package_preload(cf, "apisix.utils.upstream", ac_upstream_helpers);
}
