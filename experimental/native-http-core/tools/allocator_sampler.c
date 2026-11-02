/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Independent, diagnostic-only Lua allocator counter. Load with:
 *   local open = assert(package.loadlib(path, "luaopen_allocator_sampler"))
 *   local sampler = open()
 *   assert(sampler.start())
 *   -- bounded diagnostic window, followed by:
 *   local result = assert(sampler.stop())
 *
 * Uses public Lua 5.1 APIs only. It never changes the native core module or
 * allocator behavior: the original callback and userdata are forwarded intact.
 * A registry-rooted userdata restores the allocator in __gc at VM shutdown as
 * a fallback for the normal timed stop. Do not unload this DSO while active or
 * combine it with another component that replaces the VM allocator.
 *
 * Counts cover this VM's allocator, including background tasks and JIT compiler
 * heap allocations. They exclude NGINX pools, external C/FFI malloc, JIT machine
 * code mappings, allocator arena overhead, and allocations before start().
 * They are not logical object counts, physical malloc traffic, or live heap.
 *
 * Source audit: research container apisix-http-core-dev,
 * /tmp/openresty-1.27.1.2/bundle/LuaJIT-2.1-20250117/src:
 *   lj_api.c:1336-1347       public get/set read and replace allocator pair
 *   lj_gc.c:861-888          realloc/newgco dynamically call that pair
 *   lj_gc.h:119-123          free dynamically calls that pair
 *   lj_asm.c:1137-1172       compiled string/table allocations call helpers
 *   lj_asm_x86.h:1835,1904   compiled cdata calls allocation helpers
 *   lj_state.c:231-236      restore before close: arena destroy checks allocf
 *   lj_mcode.c:115-128      executable mappings bypass this allocator
 *
 * Counters describe attempted operations, including failed allocations.
 * requested_new_bytes sums nsize for new calls, including failed requests;
 * requested_realloc_bytes likewise sums the full target sizes of resize calls.
 * positive_growth_bytes and freed_bytes count successful size changes only.
 * relative_bytes = successful growth - frees/shrinkage since start, including
 * release of pre-window objects; it can be negative. relative_peak_bytes is
 * its maximum above zero, not the VM's absolute peak heap size.
 */
#include <lua.h>
#include <lauxlib.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    uint64_t calls;
    uint64_t new_calls;
    uint64_t realloc_calls;
    uint64_t free_calls;
    uint64_t failed_calls;
    uint64_t requested_new_bytes;
    uint64_t requested_realloc_bytes;
    uint64_t positive_growth_bytes;
    uint64_t freed_bytes;
    int64_t relative_bytes;
    int64_t relative_peak_bytes;
    int saturated;
} allocator_counts;

typedef struct {
    lua_Alloc original;
    void *original_ud;
    int active;
    allocator_counts counts;
} allocator_sampler;

static const char active_guard_key;

static void
count_add(allocator_counts *c, uint64_t *counter, uint64_t amount)
{
    if (UINT64_MAX - *counter < amount) {
        *counter = UINT64_MAX;
        c->saturated = 1;
    } else {
        *counter += amount;
    }
}

static void
count_growth(allocator_counts *c, size_t amount)
{
    count_add(c, &c->positive_growth_bytes, (uint64_t) amount);
    if ((uint64_t) amount > INT64_MAX ||
        c->relative_bytes > INT64_MAX - (int64_t) amount) {
        c->relative_bytes = INT64_MAX;
        c->saturated = 1;
    } else {
        c->relative_bytes += (int64_t) amount;
    }
    if (c->relative_bytes > c->relative_peak_bytes)
        c->relative_peak_bytes = c->relative_bytes;
}

static void
count_release(allocator_counts *c, size_t amount)
{
    count_add(c, &c->freed_bytes, (uint64_t) amount);
    if ((uint64_t) amount > INT64_MAX ||
        c->relative_bytes < INT64_MIN + (int64_t) amount) {
        c->relative_bytes = INT64_MIN;
        c->saturated = 1;
    } else {
        c->relative_bytes -= (int64_t) amount;
    }
}

/* No Lua calls, logging, allocation, pointer tracking, or reentrancy here. */
static void *
sample_allocate(void *ud, void *ptr, size_t osize, size_t nsize)
{
    allocator_sampler *s = (allocator_sampler *) ud;
    allocator_counts *c = &s->counts;
    void *result;
    count_add(c, &c->calls, 1);
    if (nsize == 0) {
        count_add(c, &c->free_calls, 1);
    } else if (ptr == NULL) {
        count_add(c, &c->new_calls, 1);
        count_add(c, &c->requested_new_bytes, (uint64_t) nsize);
    } else {
        count_add(c, &c->realloc_calls, 1);
        count_add(c, &c->requested_realloc_bytes, (uint64_t) nsize);
    }

    result = s->original(s->original_ud, ptr, osize, nsize);
    if (nsize != 0 && result == NULL) {
        count_add(c, &c->failed_calls, 1);
        return result;  /* A failed resize preserves the old allocation. */
    }
    if (ptr == NULL) {
        if (nsize != 0) count_growth(c, nsize);
    } else if (nsize >= osize) {
        count_growth(c, nsize - osize);
    } else {
        count_release(c, osize - nsize);
    }
    return result;
}

static int
owns_allocator(lua_State *L, allocator_sampler *s)
{
    void *ud = NULL;
    lua_Alloc allocator = lua_getallocf(L, &ud);
    return allocator == sample_allocate && ud == s;
}

/* __gc receives the userdata as its first argument. Restore before LuaJIT
 * close_state chooses whether to destroy its internal allocator arena. */
static int
sampler_gc(lua_State *L)
{
    allocator_sampler *s = (allocator_sampler *) lua_touserdata(L, 1);
    if (s != NULL && s->active && owns_allocator(L, s)) {
        lua_setallocf(L, s->original, s->original_ud);
        s->active = 0;
    }
    return 0;
}

static int
sampler_start(lua_State *L)
{
    allocator_sampler *s =
        (allocator_sampler *) lua_touserdata(L, lua_upvalueindex(1));
    void *ud = NULL;
    lua_Alloc allocator = lua_getallocf(L, &ud);
    if (s->active || allocator == sample_allocate)
        return luaL_error(L, "allocator sampler is already active");
    if (allocator == NULL)
        return luaL_error(L, "VM has no allocator");
    lua_pushlightuserdata(L, (void *) &active_guard_key);
    lua_rawget(L, LUA_REGISTRYINDEX);
    if (!lua_isnil(L, -1))
        return luaL_error(L, "an allocator sampler guard remains active");
    lua_pop(L, 1);

    /* Root the guard before installing the callback: a lost Lua module
     * reference must not leave a dangling allocator userdata pointer. */
    lua_pushlightuserdata(L, (void *) &active_guard_key);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_rawset(L, LUA_REGISTRYINDEX);
    memset(&s->counts, 0, sizeof(s->counts));
    s->original = allocator;
    s->original_ud = ud;
    s->active = 1;
    lua_setallocf(L, sample_allocate, s);
    lua_pushboolean(L, 1);
    return 1;
}

static void
set_number(lua_State *L, const char *name, lua_Number value)
{
    lua_pushnumber(L, value);
    lua_setfield(L, -2, name);
}

static int
sampler_stop(lua_State *L)
{
    allocator_sampler *s =
        (allocator_sampler *) lua_touserdata(L, lua_upvalueindex(1));
    allocator_counts c;
    int exact;
    if (!s->active) {
        lua_pushnil(L);
        lua_pushliteral(L, "allocator sampler is not active");
        return 2;
    }
    if (!owns_allocator(L, s)) {
        /* Do not replace somebody else's allocator or free a guard it might
         * still reference. Treat this diagnostic run as invalid. */
        lua_pushnil(L);
        lua_pushliteral(L, "allocator changed during sampling; guard remains pinned");
        return 2;
    }
    lua_setallocf(L, s->original, s->original_ud);
    s->active = 0;
    c = s->counts;
    lua_pushlightuserdata(L, (void *) &active_guard_key);
    lua_pushnil(L);
    lua_rawset(L, LUA_REGISTRYINDEX);

    /* Result construction is deliberately outside the measured window. */
    lua_createtable(L, 0, 16);
#define EMIT(name) set_number(L, #name, (lua_Number) c.name)
    EMIT(calls);
    EMIT(new_calls);
    EMIT(realloc_calls);
    EMIT(free_calls);
    EMIT(failed_calls);
    EMIT(requested_new_bytes);
    EMIT(requested_realloc_bytes);
    EMIT(positive_growth_bytes);
    EMIT(freed_bytes);
    EMIT(relative_bytes);
    EMIT(relative_peak_bytes);
#undef EMIT
    /* LuaJIT uses double numbers. Refuse to describe rounded large values as
     * exact counters, even though bounded diagnostic windows are far smaller. */
    exact = !c.saturated && c.calls <= UINT64_C(9007199254740991) &&
        c.requested_new_bytes <= UINT64_C(9007199254740991) &&
        c.requested_realloc_bytes <= UINT64_C(9007199254740991) &&
        c.positive_growth_bytes <= UINT64_C(9007199254740991) &&
        c.freed_bytes <= UINT64_C(9007199254740991);
    lua_pushboolean(L, exact);
    lua_setfield(L, -2, "counters_exact");
    lua_pushboolean(L, c.saturated);
    lua_setfield(L, -2, "counter_saturated");
    lua_pushboolean(L, 1);
    lua_setfield(L, -2, "allocator_restored");
    lua_pushliteral(L, "VM allocator requests; byte balance and peak are relative to start, not live heap");
    lua_setfield(L, -2, "scope");
    return 1;
}

LUA_API int
luaopen_allocator_sampler(lua_State *L)
{
    allocator_sampler *s;
    int guard;
    s = (allocator_sampler *) lua_newuserdata(L, sizeof(*s));
    memset(s, 0, sizeof(*s));
    guard = lua_gettop(L);
    lua_createtable(L, 0, 2);
    lua_pushcfunction(L, sampler_gc);
    lua_setfield(L, -2, "__gc");
    lua_pushliteral(L, "allocator sampler guard");
    lua_setfield(L, -2, "__metatable");
    lua_setmetatable(L, guard);
    lua_createtable(L, 0, 2);
    lua_pushvalue(L, guard);
    lua_pushcclosure(L, sampler_start, 1);
    lua_setfield(L, -2, "start");
    lua_pushvalue(L, guard);
    lua_pushcclosure(L, sampler_stop, 1);
    lua_setfield(L, -2, "stop");
    return 1;
}
