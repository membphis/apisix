/* SPDX-License-Identifier: Apache-2.0 */
#include "engine.h"

typedef enum { AE_TRUE, AE_AND, AE_OR, AE_EQ, AE_NE, AE_LT, AE_LE, AE_GT, AE_GE,
               AE_REGEX, AE_IREGEX, AE_IN, AE_HAS, AE_IP } ac_operator_e;

struct ac_expr_s {
    ac_operator_e op;
    unsigned reverse:1;
    int variable;
    int right;
    ngx_array_t children;
    ngx_regex_t *regex;
    ngx_array_t cidrs;
};

static int
ac_operator(const char *name)
{
    static struct { const char *name; int op; } operations[] = {
        {"==", AE_EQ}, {"~=", AE_NE}, {"<", AE_LT}, {"<=", AE_LE}, {">", AE_GT}, {">=", AE_GE},
        {"~~", AE_REGEX}, {"~*", AE_IREGEX}, {"in", AE_IN}, {"has", AE_HAS}, {"ipmatch", AE_IP}, {NULL, -1}
    };
    int i;
    for (i = 0; operations[i].name; i++) if (ngx_strcmp(name, operations[i].name) == 0) return operations[i].op;
    return -1;
}

static ac_expr_t *
ac_compile_expr(lua_State *L, ngx_pool_t *pool, int rules, ngx_uint_t depth)
{
    int count, first = 1, operator_index, value_index, op, i, stack = lua_gettop(L);
    const char *text;
    size_t length;
    char normalized[16];
    ac_expr_t *expr, **child;
    ngx_regex_compile_t regex;
    u_char error[NGX_MAX_CONF_ERRSTR];
    ngx_str_t address;
    ngx_cidr_t *cidr;
    rules = ac_abs(L, rules); luaL_checktype(L, rules, LUA_TTABLE);
    if (depth > 256) luaL_error(L, "expression nesting exceeds maximum depth");
    count = (int) lua_objlen(L, rules);
    expr = ngx_pcalloc(pool, sizeof(*expr));
    if (!expr) luaL_error(L, "native expression allocation failed");
    expr->variable = expr->right = LUA_NOREF;
    if (!count) {
        if (depth) luaL_error(L, "rule too short");
        return expr;
    }
    lua_rawgeti(L, rules, 1);
    if (lua_istable(L, -1)) expr->op = AE_AND;
    else {
        text = luaL_checklstring(L, -1, &length);
        if (length < sizeof(normalized)) {
            for (i = 0; i < (int) length; i++) normalized[i] = ngx_toupper(text[i]);
            normalized[length] = 0;
        } else normalized[0] = 0;
        if (ngx_strcmp(normalized, "AND") == 0 || ngx_strcmp(normalized, "!AND") == 0
            || ngx_strcmp(normalized, "OR") == 0 || ngx_strcmp(normalized, "!OR") == 0) {
            expr->reverse = normalized[0] == '!';
            expr->op = ngx_strstr(normalized, "OR") ? AE_OR : AE_AND;
            first = 2;
            if (count <= 2) luaL_error(L, "rule too short");
        } else {
            if (!depth) luaL_error(L, "rule should be wrapped inside brackets");
            lua_rawgeti(L, rules, 1); expr->variable = ac_ref(L, -1); lua_pop(L, 1);
            operator_index = count == 4 ? 3 : 2;
            value_index = count == 4 ? 4 : 3;
            if (count == 4) {
                lua_rawgeti(L, rules, 2);
                if (!lua_isstring(L, -1) || ngx_strcmp(lua_tostring(L, -1), "!") != 0) luaL_error(L, "bad 'not' expression");
                lua_pop(L, 1); expr->reverse = 1;
            }
            lua_rawgeti(L, rules, operator_index);
            if (lua_isnil(L, -1)) luaL_error(L, "invalid expression");
            text = luaL_checklstring(L, -1, &length);
            if (length >= sizeof(normalized)) {
                lua_pushvalue(L, -1); lua_getglobal(L, "string"); lua_getfield(L, -1, "lower");
                lua_remove(L, -2); lua_insert(L, -2); lua_call(L, 1, 1);
            } else {
                for (i = 0; i < (int) length; i++) normalized[i] = ngx_tolower(text[i]);
                normalized[length] = 0; lua_pushstring(L, normalized);
            }
            text = lua_tostring(L, -1); op = ac_operator(text);
            lua_rawgeti(L, rules, value_index);
            if (lua_isnil(L, -1) && op < 0) { lua_pop(L, 1); lua_pushvalue(L, -1); op = AE_EQ; }
            if (op < 0) luaL_error(L, "invalid operator '%s'", text);
            expr->op = op; expr->right = ac_ref(L, -1);
            if (op == AE_REGEX || op == AE_IREGEX) {
                ngx_memzero(&regex, sizeof(regex)); regex.pool = pool;
                if (lua_isstring(L, -1)) {
                    regex.pattern = ac_string(L, pool, -1); regex.options = op == AE_IREGEX ? NGX_REGEX_CASELESS : 0;
                    regex.err.data = error; regex.err.len = sizeof(error);
                    if (ngx_regex_compile(&regex) == NGX_OK) expr->regex = regex.regex;
                }
            } else if (op == AE_IP) {
                int list = lua_gettop(L), n = lua_istable(L, list) ? (int) lua_objlen(L, list) : 1;
                if (!n || !lua_toboolean(L, list)) luaL_error(L, "invalid ip address");
                if (ngx_array_init(&expr->cidrs, pool, n, sizeof(ngx_cidr_t)) != NGX_OK) luaL_error(L, "CIDR expression allocation failed");
                for (i = 1; i <= n; i++) {
                    if (lua_istable(L, list)) lua_rawgeti(L, list, i); else lua_pushvalue(L, list);
                    address = ac_string(L, pool, -1); cidr = ngx_array_push(&expr->cidrs);
                    if (!cidr || ngx_ptocidr(&address, cidr) == NGX_ERROR) luaL_error(L, "invalid ip address");
                    lua_pop(L, 1);
                }
            }
            lua_settop(L, stack); return expr;
        }
    }
    lua_pop(L, 1);
    if (ngx_array_init(&expr->children, pool, count, sizeof(ac_expr_t *)) != NGX_OK) luaL_error(L, "expression children allocation failed");
    for (i = first; i <= count; i++) {
        lua_rawgeti(L, rules, i); child = ngx_array_push(&expr->children);
        if (!child) luaL_error(L, "expression child allocation failed");
        *child = ac_compile_expr(L, pool, -1, depth + 1); lua_pop(L, 1);
    }
    lua_settop(L, stack); return expr;
}

ac_expr_t *
ac_expr_compile(lua_State *L, ngx_pool_t *pool, int expression)
{
    return ac_compile_expr(L, pool, expression, 0);
}

int
ac_expr_eval(lua_State *L, ac_expr_t *expr, ac_request_t *ctx)
{
    int result = 1, left, right, i, stack = lua_gettop(L);
    lua_Number a, b;
    ac_expr_t **children;
    ngx_str_t subject;
    ngx_addr_t address;
    if (!expr || expr->op == AE_TRUE) return 1;
    if (expr->op == AE_AND || expr->op == AE_OR) {
        children = expr->children.elts; result = expr->op == AE_AND;
        for (i = 0; i < (int) expr->children.nelts; i++) {
            result = ac_expr_eval(L, children[i], ctx);
            if ((expr->op == AE_AND && !result) || (expr->op == AE_OR && result)) break;
        }
        return result != expr->reverse;
    }
    ac_push(L, ctx->table_ref); lua_getfield(L, -1, "var"); ac_push(L, expr->variable); lua_gettable(L, -2);
    left = lua_gettop(L); ac_push(L, expr->right); right = lua_gettop(L);
    if (lua_islightuserdata(L, right) && lua_touserdata(L, right) == NULL) { lua_pushnil(L); lua_replace(L, right); }
    switch (expr->op) {
    case AE_EQ: case AE_NE:
        if (lua_type(L, right) == LUA_TNUMBER) result = lua_isnumber(L, left) && lua_tonumber(L, left) == lua_tonumber(L, right);
        else result = lua_equal(L, left, right);
        if (expr->op == AE_NE) result = !result;
        break;
    case AE_LT: case AE_LE: case AE_GT: case AE_GE:
        result = 0;
        if (lua_isnumber(L, left) && lua_isnumber(L, right)) {
            a = lua_tonumber(L, left); b = lua_tonumber(L, right);
            result = expr->op == AE_LT ? a < b : expr->op == AE_LE ? a <= b : expr->op == AE_GT ? a > b : a >= b;
        }
        break;
    case AE_IN: case AE_HAS:
        if (expr->op == AE_HAS) { i = right; right = left; left = i; }
        result = 0;
        if (lua_istable(L, right)) for (i = 1; i <= (int) lua_objlen(L, right); i++) {
            lua_rawgeti(L, right, i); result = lua_equal(L, left, -1); lua_pop(L, 1); if (result) break;
        }
        break;
    case AE_REGEX: case AE_IREGEX:
        result = 0;
        if (expr->regex && lua_toboolean(L, left)) {
            lua_getglobal(L, "tostring"); lua_pushvalue(L, left); lua_call(L, 1, 1);
            subject.data = (u_char *) lua_tolstring(L, -1, &subject.len);
            result = ngx_regex_exec(expr->regex, &subject, NULL, 0) >= 0;
        }
        break;
    case AE_IP:
        result = 0;
        if (lua_type(L, left) == LUA_TSTRING) {
            subject.data = (u_char *) lua_tolstring(L, left, &subject.len); ngx_memzero(&address, sizeof(address));
            if (ngx_parse_addr(ctx->request->pool, &address, subject.data, subject.len) == NGX_OK) {
                result = ngx_cidr_match(address.sockaddr, &expr->cidrs) == NGX_OK;
            }
        }
        break;
    default: break;
    }
    lua_settop(L, stack); return result != expr->reverse;
}

void
ac_expr_release(lua_State *L, ac_expr_t *expr)
{
    ac_expr_t **children;
    ngx_uint_t i;
    if (!expr) return;
    luaL_unref(L, LUA_REGISTRYINDEX, expr->variable); luaL_unref(L, LUA_REGISTRYINDEX, expr->right);
    children = expr->children.elts;
    for (i = 0; i < expr->children.nelts; i++) ac_expr_release(L, children[i]);
}
