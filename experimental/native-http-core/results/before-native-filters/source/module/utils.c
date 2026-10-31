/* SPDX-License-Identifier: Apache-2.0 */
#include "engine.h"
#include <math.h>
#include <ctype.h>
#include <stdio.h>

static void
ac_number_field(lua_State *L, int table, const char *name, lua_Number number)
{
    table = ac_abs(L, table); lua_pushnumber(L, number); lua_setfield(L, table, name);
}

static lua_Number
ac_number(lua_State *L, int table, const char *name, lua_Number fallback)
{
    lua_Number n;
    lua_getfield(L, table, name); n = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : fallback;
    lua_pop(L, 1); return n;
}

static int
ac_parse_addr(lua_State *L)
{
    size_t len, i, split = 0, colons = 0;
    const char *text = luaL_checklstring(L, 1, &len);
    if (!len) { lua_pushvalue(L, 1); return 1; }
    if (text[0] == '[') {
        if (text[len - 1] == ']') { lua_pushvalue(L, 1); lua_pushnil(L); return 2; }
        for (i = len > 1 ? len - 1 : 0; i; i--) if (text[i - 1] == ':') { split = i; break; }
        if (!split || split < 2 || text[split - 2] != ']') split = 0;
    } else {
        for (i = 0; i < len; i++) {
            if (text[i] == '.') break;
            if (text[i] == ':' && ++colons == 2) { lua_pushvalue(L, 1); return 1; }
        }
        for (i = len > 1 ? len - 1 : 0; i; i--) if (text[i - 1] == ':') { split = i; break; }
    }
    if (!split) { lua_pushvalue(L, 1); lua_pushnil(L); return 2; }
    lua_pushlstring(L, text, split - 1);
    lua_pushlstring(L, text + split, len - split);
    if (lua_isnumber(L, -1)) lua_pushnumber(L, lua_tonumber(L, -1)); else lua_pushnil(L);
    lua_remove(L, -2); return 2;
}

static int
ac_header_valid(lua_State *L)
{
    int field = lua_toboolean(L, lua_upvalueindex(1));
    size_t len, i;
    const u_char *value;
    if (!field && lua_type(L, 1) != LUA_TSTRING) { lua_pushboolean(L, 1); return 1; }
    value = (u_char *) luaL_checklstring(L, 1, &len);
    for (i = 0; i < len; i++) {
        if (value[i] < (field ? 33 : 32) || value[i] >= 127 || (field && value[i] == ':')) {
            lua_pushboolean(L, 0); return 1;
        }
    }
    lua_pushboolean(L, 1); return 1;
}

static int
ac_escape(lua_State *L)
{
    size_t len, i, count = 0;
    const u_char *text = (u_char *) luaL_checklstring(L, 1, &len);
    int controls = lua_toboolean(L, lua_upvalueindex(1));
    u_char *out, *write;
    static const u_char hex[] = "0123456789ABCDEF";
    if (controls) {
        for (i = 0; i < len; i++) if (text[i] < 32 || text[i] == 127) count++;
    } else count = ngx_escape_uri(NULL, (u_char *) text, len, NGX_ESCAPE_URI);
    if (!count) { lua_pushvalue(L, 1); return 1; }
    if (count > (NGX_MAX_SIZE_T_VALUE - len) / 2) return luaL_error(L, "escaped URI is too large");
    out = lua_newuserdata(L, len + 2 * count); write = out;
    if (controls) {
        for (i = 0; i < len; i++) {
            if (text[i] < 32 || text[i] == 127) { *write++ = '%'; *write++ = hex[text[i] >> 4]; *write++ = hex[text[i] & 15]; }
            else *write++ = text[i];
        }
    } else ngx_escape_uri(out, (u_char *) text, len, NGX_ESCAPE_URI);
    lua_pushlstring(L, (char *) out, len + 2 * count); return 1;
}

static int
ac_hostname(lua_State *L)
{
    char host[256];
    if (gethostname(host, sizeof(host) - 1) != 0) { lua_pushnil(L); lua_pushstring(L, strerror(errno)); return 2; }
    host[sizeof(host) - 1] = 0; lua_pushstring(L, host); return 1;
}

static int
ac_seed(lua_State *L)
{
    unsigned int seed;
    FILE *file = fopen("/dev/urandom", "rb");
    if (!file || fread(&seed, sizeof(seed), 1, file) != 1) {
        if (file) fclose(file); lua_pushnil(L); lua_pushliteral(L, "failed to read urandom"); return 2;
    }
    fclose(file); lua_pushnumber(L, seed); return 1;
}

static int
ac_resolve_match(lua_State *L)
{
    int ctx = lua_upvalueindex(1), counter = lua_upvalueindex(2), escaper = lua_upvalueindex(3);
    size_t len, a = 0, b, split;
    const char *raw;
    lua_rawgeti(L, 1, 2);
    if (!lua_toboolean(L, -1)) { lua_pop(L, 1); lua_rawgeti(L, 1, 3); }
    raw = luaL_checklstring(L, -1, &len);
    for (split = 0; split + 1 < len; split++) if (raw[split] == '?' && raw[split + 1] == '?') break;
    if (split + 1 >= len) split = len;
    b = split;
    while (a < b && isspace((u_char) raw[a])) a++;
    while (b > a && isspace((u_char) raw[b - 1])) b--;
    lua_pushlstring(L, raw + a, b - a); lua_gettable(L, ctx);
    if (lua_isnil(L, -1)) {
        lua_pop(L, 1);
        if (split == len) { lua_pushliteral(L, ""); return 1; }
        a = split + 2;
        while (a < len && isspace((u_char) raw[a])) a++;
        b = len;
        for (split = a; split + 1 < len; split++) if (raw[split] == '?' && raw[split + 1] == '?') { b = split; break; }
        while (b > a && isspace((u_char) raw[b - 1])) b--;
        lua_pushlstring(L, raw + a, b - a);
    }
    lua_getfield(L, counter, "count"); int n = (int) lua_tointeger(L, -1); lua_pop(L, 1);
    lua_pushinteger(L, n + 1); lua_setfield(L, counter, "count");
    lua_getglobal(L, "tostring"); lua_insert(L, -2); lua_call(L, 1, 1);
    if (lua_isfunction(L, escaper)) { lua_pushvalue(L, escaper); lua_insert(L, -2); lua_call(L, 1, 1); }
    return 1;
}

static int
ac_resolve_var(lua_State *L)
{
    int state, result;
    lua_settop(L, 3);
    if (lua_isnil(L, 1)) { lua_pushnil(L); lua_pushnil(L); lua_pushinteger(L, 0); return 3; }
    luaL_checkstring(L, 1); luaL_checktype(L, 2, LUA_TTABLE);
    lua_newtable(L); state = lua_gettop(L);
    ac_ngx(L, "re", "gsub"); lua_pushvalue(L, 1);
    lua_pushliteral(L, "(?<!\\\\)\\$(\\{\\s*([^}]+?)\\s*\\}|([\\w\\.]+))");
    lua_pushvalue(L, 2); lua_pushvalue(L, state); lua_pushvalue(L, 3); lua_pushcclosure(L, ac_resolve_match, 3);
    lua_pushliteral(L, "jo"); lua_call(L, 4, 3); result = lua_gettop(L) - 2;
    lua_pushvalue(L, result); lua_pushvalue(L, result + 2); lua_getfield(L, state, "count");
    if (lua_isnil(L, -1)) { lua_pop(L, 1); lua_pushinteger(L, 0); }
    return 3;
}

static int
ac_capture_match(lua_State *L)
{
    lua_rawgeti(L, 1, 1);
    lua_rawgeti(L, lua_upvalueindex(1), (int) lua_tointeger(L, -1));
    if (!lua_toboolean(L, -1)) lua_pushliteral(L, "");
    return 1;
}

static int
ac_resolve_captures(lua_State *L)
{
    int result;
    lua_settop(L, 2);
    if (lua_isnil(L, 1)) { lua_pushnil(L); lua_pushnil(L); return 2; }
    if (!lua_istable(L, 2)) { lua_newtable(L); lua_replace(L, 2); }
    ac_ngx(L, "re", "gsub"); lua_pushvalue(L, 1);
    lua_pushliteral(L, "(?<!\\\\)\\$\\{?(\\d+)\\}?");
    lua_pushvalue(L, 2); lua_pushcclosure(L, ac_capture_match, 1);
    lua_pushliteral(L, "jo"); lua_call(L, 4, 3); result = lua_gettop(L) - 2;
    lua_pushvalue(L, result); lua_pushvalue(L, result + 2); return 2;
}

static void
ac_null_field(lua_State *L, int dest, int source, const char *name, const char *new_name, lua_Number scale)
{
    dest = ac_abs(L, dest); source = ac_abs(L, source);
    lua_getfield(L, source, name);
    if (lua_isnil(L, -1)) { lua_pop(L, 1); lua_pushlightuserdata(L, NULL); }
    else if (scale != 1) { lua_Number n = floor(lua_tonumber(L, -1) * scale + 0.5); lua_pop(L, 1); lua_pushnumber(L, n); }
    lua_setfield(L, dest, new_name);
}

static int
ac_rate_info(lua_State *L)
{
    int info, detail = 6, current, previous, delayed;
    lua_Number size, now, last, start, weight;
    if (lua_isnoneornil(L, 1)) return 0;
    lua_settop(L, 6); lua_createtable(L, 0, 12); info = lua_gettop(L);
    lua_getglobal(L, "tostring");
    if (lua_toboolean(L, 2)) lua_pushvalue(L, 2); else lua_pushliteral(L, "");
    lua_call(L, 1, 1); lua_setfield(L, info, "rate_limiting_key");
    ac_number_field(L, info, "rate_limiting_limit", floor(lua_tonumber(L, 3)));
    ac_number_field(L, info, "rate_limiting_remaining", floor(lua_tonumber(L, 4)));
    ac_number_field(L, info, "rate_limiting_reset", floor(lua_tonumber(L, 5)));
    if (lua_istable(L, detail)) {
        lua_getfield(L, detail, "window_type"); lua_setfield(L, info, "window_type");
        ac_null_field(L, info, detail, "window_size", "window_size_ms", 1000);
        lua_getfield(L, detail, "decision"); lua_setfield(L, info, "decision");
        lua_getfield(L, detail, "decision");
        int error = lua_isstring(L, -1) && ngx_strcmp(lua_tostring(L, -1), "error") == 0; lua_pop(L, 1);
        if (!error) {
            lua_getfield(L, detail, "cost"); lua_setfield(L, info, "cost");
            ac_null_field(L, info, detail, "now", "evaluated_at_ms", 1000);
            lua_getfield(L, detail, "delayed_sync"); delayed = lua_gettop(L);
            if (lua_istable(L, delayed)) {
                lua_createtable(L, 0, 3);
                ac_null_field(L, -1, delayed, "synced_at", "synced_at_ms", 1000);
                ac_null_field(L, -1, delayed, "synced_count", "synced_count", 1);
                ac_null_field(L, -1, delayed, "local_delta", "local_delta", 1);
                lua_setfield(L, info, "delayed_sync");
            }
            lua_pop(L, 1);
            lua_createtable(L, 0, 4); current = lua_gettop(L);
            lua_getfield(L, detail, "window_type");
            int sliding = lua_isstring(L, -1) && ngx_strcmp(lua_tostring(L, -1), "sliding") == 0; lua_pop(L, 1);
            size = ac_number(L, detail, "window_size", 0);
            if (!sliding) {
                lua_getfield(L, detail, "window_end");
                if (lua_isnil(L, -1)) { lua_pushlightuserdata(L, NULL); lua_setfield(L, current, "start_ms"); }
                else ac_number_field(L, current, "start_ms", floor((lua_tonumber(L, -1) - size) * 1000 + 0.5));
                lua_pop(L, 1);
                ac_null_field(L, current, detail, "window_end", "end_ms", 1000);
                ac_null_field(L, current, detail, "created", "created", 1);
            } else {
                now = ac_number(L, detail, "window_now", -1);
                if (now < 0 || size <= 0) {
                    lua_pushlightuserdata(L, NULL); lua_setfield(L, current, "id");
                    lua_pushlightuserdata(L, NULL); lua_setfield(L, current, "start_ms");
                    lua_pushlightuserdata(L, NULL); lua_setfield(L, current, "end_ms");
                } else {
                    start = floor(now / size) * size;
                    ac_number_field(L, current, "id", floor(now / size));
                    ac_number_field(L, current, "start_ms", floor(start * 1000 + 0.5));
                    ac_number_field(L, current, "end_ms", floor((start + size) * 1000 + 0.5));
                }
                lua_createtable(L, 0, 3); previous = lua_gettop(L);
                ac_null_field(L, previous, detail, "last_count", "count", 1);
                if (now >= 0 && size > 0) {
                    weight = (size - fmod(now, size)) / size;
                    ac_number_field(L, previous, "weight", floor(weight * 1e6 + 0.5) / 1e6);
                    last = ac_number(L, detail, "last_count", -1);
                    if (last >= 0) ac_number_field(L, previous, "weighted_count", floor(last * weight * 1000 + 0.5) / 1000);
                    else { lua_pushlightuserdata(L, NULL); lua_setfield(L, previous, "weighted_count"); }
                } else {
                    lua_pushlightuserdata(L, NULL); lua_setfield(L, previous, "weight");
                    lua_pushlightuserdata(L, NULL); lua_setfield(L, previous, "weighted_count");
                }
                lua_setfield(L, info, "previous_window");
            }
            ac_null_field(L, current, detail, "count", "count", 1);
            lua_setfield(L, info, "current_window");
        }
    }
    ac_require(L, "cjson.safe"); lua_getfield(L, -1, "encode"); lua_pushvalue(L, info); lua_call(L, 1, 1);
    lua_getfield(L, 1, "var"); lua_pushvalue(L, -2); lua_setfield(L, -2, "rate_limiting_info"); return 0;
}

static int
ac_parse_time(lua_State *L)
{
    size_t length, i;
    const u_char *s;
    lua_Number total = 0, number = 0, step = 31536000, scale;
    int milliseconds = 0;
    if (lua_type(L, 1) == LUA_TNUMBER) { lua_pushvalue(L, 1); return 1; }
    s = (u_char *) lua_tolstring(L, 1, &length);
    if (!s || !length) { lua_pushnil(L); lua_pushliteral(L, "invalid data"); return 2; }
    for (i = 0; i < length; i++) {
        scale = 0;
        switch (s[i]) {
        case 'y': scale = 31536000; break; case 'M': scale = 2592000; break;
        case 'w': scale = 604800; break; case 'd': scale = 86400; break;
        case 'h': scale = 3600; break; case 'm':
            if (i + 1 < length && s[i + 1] == 's') { total *= 1000; milliseconds = 1; step = 0; goto finish; }
            scale = 60; break;
        case 's': scale = 1; break;
        default:
            if (s[i] >= '0' && s[i] <= '9') number = number * 10 + s[i] - '0';
            else if (s[i] != ' ') { lua_pushnil(L); lua_pushfstring(L, "invalid data: %c", s[i]); return 2; }
        }
        if (scale) {
            if (scale > step) { lua_pushnil(L); lua_pushfstring(L, "unexpected unit: %c", s[i]); return 2; }
            step = scale; total += scale * number; number = 0;
        }
    }
finish:
    if (number > 0) {
        if (step == 1) { lua_pushnil(L); lua_pushliteral(L, "specific unit conflicts with the default unit second"); return 2; }
        total += number;
    }
    lua_pushnumber(L, milliseconds ? total / 1000 : total); return 1;
}

static int
ac_path_parse(lua_State *L)
{
    size_t len;
    const char *s = luaL_checklstring(L, 1, &len), *p = memchr(s, '#', len), *end;
    if (!p) { lua_pushvalue(L, 1); lua_pushliteral(L, ""); return 2; }
    lua_pushlstring(L, s, p - s); end = memchr(p + 1, '#', len - (p + 1 - s));
    lua_pushlstring(L, p + 1, end ? (size_t) (end - p - 1) : len - (p + 1 - s)); return 2;
}

static int
ac_values_iter(lua_State *L)
{
    int i;
    lua_getfield(L, 1, "idx"); i = (int) lua_tointeger(L, -1); lua_pop(L, 1);
    do {
        lua_rawgeti(L, 2, ++i);
        if (lua_istable(L, -1)) { lua_pushinteger(L, i); lua_setfield(L, 1, "idx"); lua_pushinteger(L, i); lua_insert(L, -2); return 2; }
        if (lua_isnil(L, -1)) return 0;
        lua_pop(L, 1);
    } while (1);
}

static int
ac_iterate_values(lua_State *L)
{
    lua_newtable(L); lua_newtable(L); lua_pushcfunction(L, ac_values_iter); lua_setfield(L, -2, "__call");
    lua_setmetatable(L, -2); lua_pushvalue(L, 1); lua_pushinteger(L, 0); return 3;
}

static int
ac_config_util_open(lua_State *L)
{
    static const luaL_Reg functions[] = {{"parse_time_unit", ac_parse_time}, {"parse_path", ac_path_parse},
        {"iterate_values", ac_iterate_values}, {NULL, NULL}};
    lua_newtable(L); luaL_register(L, NULL, functions); return 1;
}

static int
ac_gcd(lua_State *L)
{
    lua_Number a = luaL_checknumber(L, 1), b = luaL_checknumber(L, 2), next;
    while (b != 0) { next = a - floor(a / b) * b; a = b; b = next; }
    lua_pushnumber(L, a); return 1;
}

static int
ac_math_open(lua_State *L)
{
    lua_newtable(L); lua_pushcfunction(L, ac_gcd); lua_setfield(L, -2, "gcd"); return 1;
}

static int
ac_redis_created(lua_State *L)
{
    lua_settop(L, 3);
    if (!lua_toboolean(L, 1)) { lua_pushvalue(L, 2); return lua_error(L); }
    lua_pushnil(L); lua_pushvalue(L, 2);
    if (lua_isnil(L, 2)) {
        lua_pushfstring(L, "failed to new redis cluster: %s", lua_tostring(L, 3)); return 3;
    }
    return 2;
}

static int
ac_redis_start(lua_State *L)
{
    static struct { const char *dest; const char *src; } fields[] = {
        {"name", "redis_cluster_name"}, {"keepalive_timeout", "redis_keepalive_timeout"},
        {"keepalive_cons", "redis_keepalive_pool"}, {"connect_timeout", "redis_timeout"},
        {"read_timeout", "redis_timeout"}, {"send_timeout", "redis_timeout"}, {"auth", "redis_password"}, {NULL, NULL}
    };
    int config, nodes, servers, server, i;
    lua_settop(L, 2); lua_newtable(L); config = lua_gettop(L);
    for (i = 0; fields[i].dest; i++) { lua_getfield(L, 1, fields[i].src); lua_setfield(L, config, fields[i].dest); }
    lua_pushvalue(L, 2); lua_setfield(L, config, "dict_name");
    lua_newtable(L); lua_getfield(L, 1, "redis_cluster_ssl"); lua_setfield(L, -2, "ssl");
    lua_getfield(L, 1, "redis_cluster_ssl_verify"); lua_setfield(L, -2, "ssl_verify");
    lua_setfield(L, config, "connect_opts");
    lua_getfield(L, 1, "redis_cluster_nodes"); nodes = lua_gettop(L);
    lua_newtable(L); servers = lua_gettop(L);
    for (i = 1; i <= (int) lua_objlen(L, nodes); i++) {
        lua_newtable(L); server = lua_gettop(L);
        lua_pushcfunction(L, ac_parse_addr); lua_rawgeti(L, nodes, i); lua_call(L, 1, 2);
        lua_setfield(L, server, "port"); lua_setfield(L, server, "ip");
        lua_rawseti(L, servers, i);
    }
    lua_pushvalue(L, servers); lua_setfield(L, config, "serv_list");
    ac_require(L, "resty.rediscluster"); int module = lua_gettop(L);
    lua_pushcfunction(L, ac_redis_created); lua_getfield(L, module, "new");
    lua_newtable(L); lua_pushvalue(L, module); lua_rawseti(L, -2, 1);
    lua_pushvalue(L, config); lua_rawseti(L, -2, 2); lua_pushinteger(L, 2); lua_setfield(L, -2, "n");
    return 3;
}

static int
ac_redis_open(lua_State *L)
{
    lua_newtable(L); ac_bind(L, ac_redis_start, 0); lua_setfield(L, -2, "new"); return 1;
}

static int
ac_utils_open(lua_State *L)
{
    static const luaL_Reg functions[] = {
        {"parse_addr", ac_parse_addr}, {"gethostname", ac_hostname}, {"get_seed_from_urandom", ac_seed},
        {"resolve_var", ac_resolve_var}, {"resolve_var_with_captures", ac_resolve_captures},
        {"set_var_rate_limiting_info", ac_rate_info}, {NULL, NULL}
    };
    lua_newtable(L); luaL_register(L, NULL, functions);
    lua_pushboolean(L, 1); lua_pushcclosure(L, ac_header_valid, 1); lua_setfield(L, -2, "validate_header_field");
    lua_pushboolean(L, 0); lua_pushcclosure(L, ac_header_valid, 1); lua_setfield(L, -2, "validate_header_value");
    lua_pushboolean(L, 0); lua_pushcclosure(L, ac_escape, 1); lua_setfield(L, -2, "uri_safe_encode");
    lua_pushboolean(L, 1); lua_pushcclosure(L, ac_escape, 1); lua_setfield(L, -2, "escape_uri_control_chars");
    ac_ngx(L, NULL, "sleep"); lua_setfield(L, -2, "sleep");
    ac_import(L, -1, "resty.ipmatcher", "parse_ipv4");
    ac_import(L, -1, "resty.ipmatcher", "parse_ipv6");
    ac_resolver_utils(L, -1);
    return 1;
}

ngx_int_t
ac_utils_register(ngx_conf_t *cf)
{
    if (ngx_http_lua_add_package_preload(cf, "apisix.core.utils", ac_utils_open) != NGX_OK
        || ngx_http_lua_add_package_preload(cf, "apisix.core.math", ac_math_open) != NGX_OK
        || ngx_http_lua_add_package_preload(cf, "apisix.core.config_util", ac_config_util_open) != NGX_OK
        || ngx_http_lua_add_package_preload(cf, "apisix.utils.rediscluster", ac_redis_open) != NGX_OK) return NGX_ERROR;
    return ac_instrumentation_register(cf);
}
