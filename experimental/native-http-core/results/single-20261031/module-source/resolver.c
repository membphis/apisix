/* SPDX-License-Identifier: Apache-2.0 */
#include "engine.h"

static int ac_dns_step(lua_State *L, int state);

static void
ac_dns_copy(lua_State *L, int source)
{
    source = ac_abs(L, source);
    ac_require(L, "apisix.core.table"); lua_getfield(L, -1, "deepcopy"); lua_remove(L, -2);
    lua_pushvalue(L, source); lua_call(L, 1, 1);
}

static int
ac_dns_created(lua_State *L)
{
    int state = lua_upvalueindex(1), answers, i, record, resolved, result, copy;
    lua_Number weight;
    lua_settop(L, 3);
    if (!lua_toboolean(L, 1)) { lua_pushvalue(L, 2); return lua_error(L); }
    if (lua_isnil(L, 2)) { lua_pushnil(L); lua_pushnil(L); lua_pushfstring(L, "failed to resolve SRV record: %s", lua_tostring(L, 3)); return 3; }
    answers = 2;
    lua_getfield(L, state, "record"); record = lua_gettop(L);
    lua_getfield(L, record, "weight"); weight = lua_tonumber(L, -1); lua_pop(L, 1);
    if (weight == 0) weight = 1;
    lua_getfield(L, state, "resolved"); resolved = lua_gettop(L);
    lua_getfield(L, state, "counts"); lua_pushinteger(L, (lua_Integer) lua_objlen(L, answers));
    lua_rawseti(L, -2, (int) lua_objlen(L, -2) + 1); lua_pop(L, 1);
    for (i = 1; i <= (int) lua_objlen(L, answers); i++) {
        lua_rawgeti(L, answers, i); ac_dns_copy(L, -1); copy = lua_gettop(L);
        lua_pushnumber(L, weight / lua_objlen(L, answers)); lua_setfield(L, copy, "weight");
        lua_getfield(L, record, "port"); lua_setfield(L, copy, "port");
        lua_getfield(L, record, "priority"); lua_setfield(L, copy, "priority");
        result = (int) lua_objlen(L, resolved) + 1;
        lua_rawseti(L, resolved, result); lua_pop(L, 1);
    }
    return ac_dns_step(L, state);
}

static int
ac_dns_step(lua_State *L, int state)
{
    int index, answers, record, counts, i, resolved;
    ngx_uint_t lcm = 1, count, a, b, tmp;
    state = ac_abs(L, state);
    lua_getfield(L, state, "index"); index = (int) lua_tointeger(L, -1) + 1; lua_pop(L, 1);
    lua_pushinteger(L, index); lua_setfield(L, state, "index");
    lua_getfield(L, state, "answers"); answers = lua_gettop(L);
    if (index <= (int) lua_objlen(L, answers)) {
        lua_rawgeti(L, answers, index); record = lua_gettop(L);
        lua_getfield(L, record, "type");
        if (lua_tointeger(L, -1) != 33) { lua_pushnil(L); lua_pushnil(L); lua_pushliteral(L, "mess SRV with other record"); return 3; }
        lua_pop(L, 1); lua_pushvalue(L, record); lua_setfield(L, state, "record");
        lua_pushvalue(L, state); lua_pushcclosure(L, ac_dns_created, 1);
        lua_getfield(L, state, "client"); lua_getfield(L, -1, "resolve"); lua_remove(L, -2);
        lua_createtable(L, 1, 1); lua_getfield(L, record, "target"); lua_rawseti(L, -2, 1);
        lua_pushinteger(L, 1); lua_setfield(L, -2, "n"); return 3;
    }
    lua_getfield(L, state, "counts"); counts = lua_gettop(L);
    for (i = 1; i <= (int) lua_objlen(L, counts); i++) {
        lua_rawgeti(L, counts, i); count = (ngx_uint_t) lua_tointeger(L, -1); lua_pop(L, 1);
        a = count; b = lcm;
        while (b) { tmp = a % b; a = b; b = tmp; }
        if (!a) continue;
        lcm = count / a * lcm;
    }
    lua_getfield(L, state, "resolved"); resolved = lua_gettop(L);
    for (i = 1; i <= (int) lua_objlen(L, resolved); i++) {
        lua_rawgeti(L, resolved, i); lua_getfield(L, -1, "weight");
        lua_pushnumber(L, lua_tonumber(L, -1) * lcm); lua_setfield(L, -3, "weight"); lua_pop(L, 2);
    }
    lua_pushnil(L); lua_pushvalue(L, resolved); return 2;
}

static int
ac_dns_answer(lua_State *L)
{
    int state = lua_upvalueindex(1), i, all;
    lua_settop(L, 3);
    if (!lua_toboolean(L, 1)) { lua_pushvalue(L, 2); return lua_error(L); }
    if (lua_isnil(L, 2)) {
        lua_pushnil(L); lua_pushnil(L); lua_pushfstring(L, "failed to query the DNS server: %s", lua_tostring(L, 3)); return 3;
    }
    lua_getfield(L, 2, "errcode");
    if (!lua_isnil(L, -1)) {
        lua_getfield(L, 2, "errstr");
        lua_pushnil(L); lua_pushnil(L);
        lua_pushfstring(L, "server returned error code: %d: %s", lua_tointeger(L, -4), lua_tostring(L, -3)); return 3;
    }
    lua_pop(L, 1); all = ac_bool_field(L, state, "all");
    if (all) {
        for (i = 1; i <= (int) lua_objlen(L, 2); i++) {
            lua_rawgeti(L, 2, i); lua_getfield(L, -1, "type");
            int srv = lua_tointeger(L, -1) == 33; lua_pop(L, 2);
            if (srv) {
                lua_pushvalue(L, 2); lua_setfield(L, state, "answers");
                lua_newtable(L); lua_setfield(L, state, "resolved"); lua_newtable(L); lua_setfield(L, state, "counts");
                return ac_dns_step(L, state);
            }
        }
        lua_pushnil(L); ac_dns_copy(L, 2); return 2;
    }
    if (!lua_objlen(L, 2)) return luaL_error(L, "bad argument to random (interval is empty)");
    lua_getglobal(L, "math"); lua_getfield(L, -1, "random"); lua_pushinteger(L, 1);
    lua_pushinteger(L, (lua_Integer) lua_objlen(L, 2)); lua_call(L, 2, 1);
    lua_rawgeti(L, 2, (int) lua_tointeger(L, -1)); i = lua_gettop(L);
    lua_getfield(L, i, "type");
    if (lua_tointeger(L, -1) != 1 && lua_tointeger(L, -1) != 28) {
        lua_pushnil(L); lua_pushnil(L); lua_pushliteral(L, "unsupported DNS answer"); return 3;
    }
    lua_pop(L, 1); lua_pushnil(L); ac_dns_copy(L, i); return 2;
}

static int
ac_dns_start(lua_State *L)
{
    int state, client;
    lua_settop(L, 3); lua_newtable(L); state = lua_gettop(L);
    lua_getfield(L, 1, "client"); client = lua_gettop(L);
    lua_pushvalue(L, client); lua_setfield(L, state, "client");
    lua_pushboolean(L, lua_tointeger(L, 3) == 2); lua_setfield(L, state, "all");
    lua_pushvalue(L, state); lua_pushcclosure(L, ac_dns_answer, 1);
    lua_getfield(L, client, "resolve");
    lua_createtable(L, 1, 1); lua_pushvalue(L, 2); lua_rawseti(L, -2, 1);
    lua_pushinteger(L, 1); lua_setfield(L, -2, "n"); return 3;
}

static int
ac_dns_new(lua_State *L)
{
    int options = 1, client;
    luaL_checktype(L, options, LUA_TTABLE);
    lua_getfield(L, options, "enable_ipv6");
    if (lua_isnil(L, -1)) {
        ac_push(L, ac_main()->local_ref); lua_getfield(L, -1, "apisix"); lua_getfield(L, -1, "enable_ipv6");
        lua_pushvalue(L, -1); lua_setfield(L, options, "enable_ipv6"); lua_replace(L, -4); lua_pop(L, 2);
    }
    if (!lua_toboolean(L, -1)) {
        lua_getfield(L, options, "order");
        for (int i = 1; i <= (int) lua_objlen(L, -1); i++) {
            lua_rawgeti(L, -1, i); int aaaa = lua_isstring(L, -1) && ngx_strcmp(lua_tostring(L, -1), "AAAA") == 0; lua_pop(L, 1);
            if (aaaa) {
                lua_getglobal(L, "table"); lua_getfield(L, -1, "remove");
                lua_pushvalue(L, -3); lua_pushinteger(L, i); lua_call(L, 2, 0); lua_pop(L, 1); break;
            }
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    lua_pushinteger(L, 2000); lua_setfield(L, options, "timeout");
    lua_pushinteger(L, 5); lua_setfield(L, options, "retrans");
    lua_pushboolean(L, 1); lua_setfield(L, options, "finalCacheOnly");
    lua_getglobal(L, "package"); lua_getfield(L, -1, "loaded");
    lua_pushnil(L); lua_setfield(L, -2, "resty.dns.client"); lua_pop(L, 2);
    ac_require(L, "resty.dns.client"); client = lua_gettop(L);
    lua_getfield(L, client, "init"); lua_pushvalue(L, options); lua_call(L, 1, 2);
    if (!lua_toboolean(L, -2)) {
        lua_pushnil(L); lua_pushfstring(L, "failed to init the dns client: %s", lua_tostring(L, -2)); return 2;
    }
    lua_pop(L, 2); lua_newtable(L); lua_pushvalue(L, client); lua_setfield(L, -2, "client");
    lua_newtable(L); ac_require(L, "apisix.core.dns.client"); lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2); return 1;
}

static int
ac_dns_open(lua_State *L)
{
    lua_newtable(L); lua_pushcfunction(L, ac_dns_new); lua_setfield(L, -2, "new");
    ac_bind(L, ac_dns_start, 0); lua_setfield(L, -2, "resolve");
    lua_pushinteger(L, 1); lua_setfield(L, -2, "RETURN_RANDOM"); lua_pushinteger(L, 2); lua_setfield(L, -2, "RETURN_ALL"); return 1;
}

static void
ac_default_dns(lua_State *L)
{
    ac_main_conf_t *main = ac_main();
    int options;
    if (main->dns_client_ref != LUA_NOREF) { ac_push(L, main->dns_client_ref); return; }
    lua_newtable(L); options = lua_gettop(L);
    ac_push(L, main->resolver_names_ref); lua_setfield(L, options, "nameservers");
    lua_createtable(L, 4, 0);
    lua_pushliteral(L, "last"); lua_rawseti(L, -2, 1); lua_pushliteral(L, "A"); lua_rawseti(L, -2, 2);
    lua_pushliteral(L, "AAAA"); lua_rawseti(L, -2, 3); lua_pushliteral(L, "CNAME"); lua_rawseti(L, -2, 4);
    lua_setfield(L, options, "order");
    ac_push(L, main->local_ref); lua_getfield(L, -1, "apisix");
    lua_getfield(L, -1, "dns_resolver_valid"); lua_setfield(L, options, "validTtl");
    if (!ac_bool_field(L, -1, "enable_resolv_search_opt")) { lua_newtable(L); lua_setfield(L, options, "search"); }
    lua_pop(L, 2);
    lua_pushcfunction(L, ac_dns_new); lua_pushvalue(L, options); lua_call(L, 1, 2);
    if (lua_isnil(L, -2)) luaL_error(L, "failed to init the dns client: %s", lua_tostring(L, -1));
    lua_pop(L, 1); main->dns_client_ref = ac_ref(L, -1);
}

static int
ac_dns_forward(lua_State *L)
{
    int n = lua_gettop(L);
    if (!lua_toboolean(L, 1)) { lua_pushvalue(L, 2); return lua_error(L); }
    lua_pushnil(L); lua_replace(L, 1); return n;
}

static int
ac_dns_parse_start(lua_State *L)
{
    int client;
    lua_settop(L, 2); ac_default_dns(L); client = lua_gettop(L);
    lua_pushcfunction(L, ac_dns_forward); lua_getfield(L, client, "resolve");
    lua_createtable(L, 3, 1); lua_pushvalue(L, client); lua_rawseti(L, -2, 1);
    lua_pushvalue(L, 1); lua_rawseti(L, -2, 2); lua_pushvalue(L, 2); lua_rawseti(L, -2, 3);
    lua_pushinteger(L, 3); lua_setfield(L, -2, "n"); return 3;
}

static int
ac_set_resolver(lua_State *L)
{
    ac_main_conf_t *main = ac_main();
    luaL_unref(L, LUA_REGISTRYINDEX, main->resolver_names_ref);
    luaL_unref(L, LUA_REGISTRYINDEX, main->dns_client_ref);
    main->resolver_names_ref = ac_ref(L, 1); main->dns_client_ref = LUA_NOREF; return 0;
}

static int
ac_get_resolver(lua_State *L)
{
    ac_push(L, ac_main()->resolver_names_ref); return 1;
}

static int
ac_resolver_init(lua_State *L)
{
    ac_main_conf_t *main = ac_main();
    lua_settop(L, 1);
    ac_require(L, "resty.dns.utils"); lua_getfield(L, -1, "parseHosts"); lua_call(L, 0, 1);
    luaL_unref(L, LUA_REGISTRYINDEX, main->hosts_ref); main->hosts_ref = ac_ref(L, -1);
    if (lua_istable(L, 1)) {
        lua_getfield(L, 1, "dns_resolver"); lua_pushcfunction(L, ac_set_resolver); lua_pushvalue(L, -2); lua_call(L, 1, 0);
    }
    return 0;
}

void
ac_resolver_worker(lua_State *L)
{
    ac_push(L, ac_main()->local_ref); lua_pushcfunction(L, ac_resolver_init); lua_pushvalue(L, -2); lua_call(L, 1, 0); lua_pop(L, 1);
}

static int
ac_domain_answer(lua_State *L)
{
    int all = lua_toboolean(L, lua_upvalueindex(1)), results, i;
    lua_settop(L, 3);
    if (!lua_toboolean(L, 1)) { lua_pushvalue(L, 2); return lua_error(L); }
    if (lua_isnil(L, 2)) { lua_pushnil(L); lua_pushnil(L); lua_pushvalue(L, 3); return 3; }
    lua_pushnil(L);
    if (!all) {
        lua_getfield(L, 2, "address");
        if (lua_isnil(L, -1)) { lua_pushliteral(L, "failed to parse domain"); return 3; }
        return 2;
    }
    lua_newtable(L); results = lua_gettop(L); lua_newtable(L);
    for (i = 1; i <= (int) lua_objlen(L, 2); i++) {
        lua_rawgeti(L, 2, i); lua_getfield(L, -1, "address");
        if (!lua_isnil(L, -1)) {
            lua_pushvalue(L, -1); lua_rawget(L, results + 1);
            if (!lua_toboolean(L, -1)) {
                lua_pushvalue(L, -2); lua_rawseti(L, results, (int) lua_objlen(L, results) + 1);
                lua_pushvalue(L, -2); lua_pushboolean(L, 1); lua_rawset(L, results + 1);
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 2);
    }
    if (!lua_objlen(L, results)) { lua_pushnil(L); lua_pushnil(L); lua_pushliteral(L, "failed to parse domain"); return 3; }
    lua_getglobal(L, "table"); lua_getfield(L, -1, "sort"); lua_pushvalue(L, results); lua_call(L, 1, 0); lua_pop(L, 1);
    lua_pushnil(L); lua_pushvalue(L, results); return 2;
}

static int
ac_domain_start(lua_State *L)
{
    int all = lua_toboolean(L, lua_upvalueindex(1)), host, results;
    lua_settop(L, 1); luaL_checkstring(L, 1);
    ac_push(L, ac_main()->hosts_ref);
    if (lua_istable(L, -1)) {
        lua_pushvalue(L, 1); lua_rawget(L, -2); host = lua_gettop(L);
        if (lua_istable(L, host)) {
            lua_getfield(L, host, "ipv4");
            if (!lua_isnil(L, -1) && !all) { lua_pushnil(L); lua_insert(L, -2); return 2; }
            lua_pop(L, 1); lua_newtable(L); results = lua_gettop(L);
            lua_getfield(L, host, "ipv4"); if (!lua_isnil(L, -1)) lua_rawseti(L, results, 1); else lua_pop(L, 1);
            ac_push(L, ac_main()->local_ref); lua_getfield(L, -1, "apisix"); int ipv6 = ac_bool_field(L, -1, "enable_ipv6"); lua_pop(L, 2);
            if (ipv6) {
                lua_getfield(L, host, "ipv6");
                if (!lua_isnil(L, -1)) lua_rawseti(L, results, (int) lua_objlen(L, results) + 1); else lua_pop(L, 1);
            }
            if (lua_objlen(L, results)) {
                lua_getglobal(L, "table"); lua_getfield(L, -1, "sort"); lua_pushvalue(L, results); lua_call(L, 1, 0); lua_pop(L, 1);
                lua_pushnil(L);
                if (all) lua_pushvalue(L, results); else lua_rawgeti(L, results, 1);
                return 2;
            }
        }
    }
    lua_pushboolean(L, all); lua_pushcclosure(L, ac_domain_answer, 1);
    ac_bind(L, ac_dns_parse_start, 0);
    lua_newtable(L); lua_pushvalue(L, 1); lua_rawseti(L, -2, 1); lua_pushinteger(L, all ? 2 : 1); lua_rawseti(L, -2, 2);
    lua_pushinteger(L, 2); lua_setfield(L, -2, "n"); return 3;
}

void
ac_resolver_utils(lua_State *L, int module)
{
    module = ac_abs(L, module);
    ac_bind(L, ac_dns_parse_start, 0); lua_setfield(L, module, "dns_parse");
    lua_pushcfunction(L, ac_set_resolver); lua_setfield(L, module, "set_resolver");
    lua_pushcfunction(L, ac_get_resolver); lua_setfield(L, module, "get_resolver");
}

static int
ac_resolver_open(lua_State *L)
{
    lua_newtable(L); lua_pushcfunction(L, ac_resolver_init); lua_setfield(L, -2, "init_resolver");
    lua_pushboolean(L, 0); ac_bind(L, ac_domain_start, 1); lua_setfield(L, -2, "parse_domain");
    lua_pushboolean(L, 1); ac_bind(L, ac_domain_start, 1); lua_setfield(L, -2, "parse_domain_all"); return 1;
}

ngx_int_t
ac_resolver_register(ngx_conf_t *cf)
{
    if (ngx_http_lua_add_package_preload(cf, "apisix.core.dns.client", ac_dns_open) != NGX_OK
        || ngx_http_lua_add_package_preload(cf, "apisix.core.resolver", ac_resolver_open) != NGX_OK) return NGX_ERROR;
    return ac_background_register(cf);
}
