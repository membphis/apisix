/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Standalone lifecycle check for tools/allocator_sampler.c. No NGINX or load.
 * Build against the same installed public LuaJIT headers/library as the DSO:
 *   cc -O2 -I/usr/local/openresty/luajit/include/luajit-2.1 \
 *     test_allocator.c -L/usr/local/openresty/luajit/lib -lluajit-5.1 \
 *     -Wl,-rpath,/usr/local/openresty/luajit/lib -o /tmp/test_allocator
 *   /tmp/test_allocator /absolute/path/to/allocator_sampler.so
 *
 * Checks allocator callback/userdata restoration and successful VM shutdown.
 * This does not establish that uninstrumented LuaJIT internals are leak-free.
 */
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <stdio.h>
#include <stdlib.h>

static void
check(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(1);
    }
}

static void
call_ok(lua_State *L, int nargs, int nresults, const char *where)
{
    if (lua_pcall(L, nargs, nresults, 0) != 0) {
        const char *error = lua_tostring(L, -1);
        fprintf(stderr, "FAIL: %s: %s\n", where,
                error ? error : "non-string Lua error");
        exit(1);
    }
}

static lua_State *
new_vm(void)
{
    lua_State *L = luaL_newstate();
    check(L != NULL, "luaL_newstate");
    luaL_openlibs(L);
    return L;
}

/* Exercise package.loadlib's real DSO lifetime, including its unload guard. */
static int
load_sampler(lua_State *L, const char *path)
{
    lua_getglobal(L, "package");
    lua_getfield(L, -1, "loadlib");
    lua_remove(L, -2);
    lua_pushstring(L, path);
    lua_pushliteral(L, "luaopen_allocator_sampler");
    call_ok(L, 2, 3, "package.loadlib");
    if (!lua_isfunction(L, -3)) {
        const char *error = lua_tostring(L, -2);
        fprintf(stderr, "FAIL: cannot load sampler: %s\n",
                error ? error : "unknown loader error");
        exit(1);
    }
    lua_pop(L, 2);
    call_ok(L, 0, 1, "luaopen_allocator_sampler");
    check(lua_istable(L, -1), "sampler module is a table");
    return luaL_ref(L, LUA_REGISTRYINDEX);
}

static void
push_method(lua_State *L, int module, const char *method)
{
    lua_rawgeti(L, LUA_REGISTRYINDEX, module);
    lua_getfield(L, -1, method);
    lua_remove(L, -2);
    check(lua_isfunction(L, -1), "sampler method exists");
}

static void
start(lua_State *L, int module)
{
    push_method(L, module, "start");
    call_ok(L, 0, 1, "sampler.start");
    check(lua_toboolean(L, -1), "start returns true");
    lua_pop(L, 1);
}

static void
check_pair(lua_State *L, lua_Alloc expected, void *expected_ud)
{
    void *ud = NULL;
    lua_Alloc current = lua_getallocf(L, &ud);
    check(current == expected, "original allocator callback restored");
    check(ud == expected_ud, "original allocator userdata restored");
}

static void
allocate_and_collect(lua_State *L)
{
    int i;
    lua_createtable(L, 128, 0);
    for (i = 1; i <= 128; i++) {
        lua_createtable(L, 16, 16);
        lua_rawseti(L, -2, i);
    }
    lua_pop(L, 1);
    lua_gc(L, LUA_GCCOLLECT, 0);
}

static lua_Number
number_field(lua_State *L, const char *name)
{
    lua_Number value;
    lua_getfield(L, -1, name);
    check(lua_isnumber(L, -1), name);
    value = lua_tonumber(L, -1);
    lua_pop(L, 1);
    return value;
}

static void
normal_stop(const char *path)
{
    lua_State *L = new_vm();
    int module = load_sampler(L, path);
    void *original_ud = NULL, *active_ud = NULL;
    lua_Alloc original = lua_getallocf(L, &original_ud), active;
    lua_Number calls, allocations, resizes, frees;

    start(L, module);
    active = lua_getallocf(L, &active_ud);
    check(active != original, "start installs a distinct allocator callback");
    check(active_ud != NULL, "start installs a counter userdata");
    push_method(L, module, "start");
    check(lua_pcall(L, 0, 1, 0) != 0, "duplicate start fails");
    lua_pop(L, 1);
    check_pair(L, active, active_ud);
    allocate_and_collect(L);

    push_method(L, module, "stop");
    call_ok(L, 0, 1, "sampler.stop");
    check_pair(L, original, original_ud);
    check(lua_istable(L, -1), "stop returns counters");
    calls = number_field(L, "calls");
    allocations = number_field(L, "new_calls");
    resizes = number_field(L, "realloc_calls");
    frees = number_field(L, "free_calls");
    check(calls > 0 && allocations > 0 && frees > 0, "allocation and collection counted");
    check(calls == allocations + resizes + frees, "operation classes partition calls");
    check(number_field(L, "failed_calls") == 0, "small allocations succeed");
    check(number_field(L, "requested_new_bytes") > 0, "new request bytes counted");
    check(number_field(L, "positive_growth_bytes") > 0, "successful growth counted");
    check(number_field(L, "freed_bytes") > 0, "successful release counted");
    lua_getfield(L, -1, "allocator_restored");
    check(lua_toboolean(L, -1), "stop reports allocator restoration");
    lua_pop(L, 1);
    lua_getfield(L, -1, "counters_exact");
    check(lua_toboolean(L, -1), "small-window counters remain exact");
    lua_pop(L, 2);

    /* Normal stop must also allow a fresh window on the same module. */
    start(L, module);
    allocate_and_collect(L);
    push_method(L, module, "stop");
    call_ok(L, 0, 1, "second sampler.stop");
    check_pair(L, original, original_ud);
    lua_pop(L, 1);
    lua_close(L);
    puts("PASS normal start/count/stop/restart and duplicate-start rejection");
}

static void
explicit_guard(const char *path)
{
    lua_State *L = new_vm();
    int module = load_sampler(L, path), guard;
    void *original_ud = NULL;
    lua_Alloc original = lua_getallocf(L, &original_ud);
    start(L, module);
    allocate_and_collect(L);

    /* Public C API access ignores the Lua-level protected metatable label. */
    push_method(L, module, "start");
    check(lua_getupvalue(L, -1, 1) != NULL, "start has its guard upvalue");
    check(lua_isuserdata(L, -1), "guard upvalue is userdata");
    guard = lua_gettop(L);
    check(lua_getmetatable(L, guard), "guard has a metatable");
    lua_getfield(L, -1, "__gc");
    check(lua_isfunction(L, -1), "guard has a finalizer");
    lua_pushvalue(L, guard);
    call_ok(L, 1, 0, "explicit guard __gc");
    check_pair(L, original, original_ud);
    lua_settop(L, 0);
    lua_close(L);  /* The automatically repeated finalizer must be harmless. */
    puts("PASS explicit guard finalizer restores the original allocator pair");
}

static void
close_without_stop(const char *path)
{
    lua_State *L = new_vm();
    int module = load_sampler(L, path);
    start(L, module);
    allocate_and_collect(L);
    /* Drop the module too: the registry guard must survive until shutdown. */
    luaL_unref(L, LUA_REGISTRYINDEX, module);
    lua_gc(L, LUA_GCCOLLECT, 0);
    lua_close(L);
    puts("PASS lua_close completes without an explicit stop (not a leak check)");
}

int
main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s /absolute/path/to/allocator_sampler.so\n", argv[0]);
        return 2;
    }
    normal_stop(argv[1]);
    explicit_guard(argv[1]);
    close_without_stop(argv[1]);
    return 0;
}
