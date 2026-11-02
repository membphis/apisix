/* SPDX-License-Identifier: Apache-2.0 */
/* Diagnostic-only synchronous-request scope clocks and VM allocation census. */
#define _GNU_SOURCE
#include <lua.h>
#include <lauxlib.h>
#include <luajit.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>

#define SLOTS 8192
#define SAMPLES 32768
#define CORE 1
#define PLUGIN 2
typedef struct { uintptr_t id; int sampled, live; uint64_t cpu[3], wall[3], spans; } request_t;
typedef struct { uint64_t calls, new_calls, resize_calls, free_calls, failed, requested_new, growth; } alloc_t;
static request_t requests[SLOTS], scratch[SLOTS], *current;
static uint64_t sample_cpu[SAMPLES][3], sample_wall[SAMPLES][3];
static alloc_t allocation[3];
static uint64_t accepted, completed, samples, invalid, missing, switches;
static uint64_t plugin_entries, sdk_entries;
static uint64_t stamp_cpu, stamp_wall, gc_samples, vm_samples, vm_states[256], scope_gc[3];
static unsigned depth, stride = 64;
static int active, scope, allocator_active, profile_active;
static lua_Alloc original_alloc;
static void *original_ud;

static uint64_t clock_ns(clockid_t clock)
{ struct timespec t; clock_gettime(clock, &t); return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec; }
void ac_cost_spin(uint64_t nanoseconds)
{ uint64_t start = clock_ns(CLOCK_THREAD_CPUTIME_ID); while (clock_ns(CLOCK_THREAD_CPUTIME_ID) - start < nanoseconds) {} }

static request_t *lookup(uintptr_t id, int create)
{
    unsigned start = ((id >> 4) ^ (id >> 17)) & (SLOTS - 1), i;
    request_t *reuse = NULL;
    for (i = 0; i < SLOTS; i++) {
        request_t *r = &requests[(start + i) & (SLOTS - 1)];
        if (r->id == id) return r;
        if (!r->live && !reuse) reuse = r;
        if (!r->id) break;
    }
    if (!create || !reuse) return NULL;
    memset(reuse, 0, sizeof(*reuse)); reuse->id = id;
    return reuse;
}

static void compact(void)
{
    unsigned i;
    if (completed % (SLOTS / 2) || current) return;
    memcpy(scratch, requests, sizeof(requests)); memset(requests, 0, sizeof(requests));
    for (i = 0; i < SLOTS; i++) if (scratch[i].live) {
        request_t *r = lookup(scratch[i].id, 1);
        if (!r) { invalid++; return; }
        *r = scratch[i];
    }
}

static void charge(void)
{
    uint64_t cpu, wall;
    if (!current || !current->sampled) return;
    cpu = clock_ns(CLOCK_THREAD_CPUTIME_ID); wall = clock_ns(CLOCK_MONOTONIC_RAW);
    if (scope) { current->cpu[scope] += cpu - stamp_cpu; current->wall[scope] += wall - stamp_wall; }
    stamp_cpu = cpu; stamp_wall = wall;
}

int ac_cost_tag(void) { return active ? scope : 0; }
int ac_cost_enter(int tag)
{
    int previous = scope;
    if (!active || !current) return previous;
    if (tag < CORE || tag > PLUGIN) { invalid++; return previous; }
    if (tag != scope) {
        if (tag == PLUGIN) plugin_entries++;
        if (tag == CORE && scope == PLUGIN) sdk_entries++;
        charge(); scope = tag; switches++; current->spans++;
    }
    return previous;
}
void ac_cost_leave(int previous)
{
    if (!active || !current) return;
    if (previous != scope) { charge(); scope = previous; switches++; }
}
void ac_cost_begin(uintptr_t id, int is_new)
{
    request_t *r;
    if (!active) return;
    if (current) {
        if (current->id != id) invalid++; /* Yield/interleaving invalidates this synchronous census. */
        depth++; return;
    }
    r = lookup(id, is_new);
    if (!r || (!r->live && !is_new)) { missing++; return; }
    if (is_new) {
        if (r->live) { invalid++; return; }
        memset(r->cpu, 0, sizeof(r->cpu)); memset(r->wall, 0, sizeof(r->wall));
        r->live = 1; r->spans = 0; r->sampled = (++accepted % stride) == 0;
    }
    current = r; depth = 1; scope = CORE;
    if (r->sampled) { stamp_cpu = clock_ns(CLOCK_THREAD_CPUTIME_ID); stamp_wall = clock_ns(CLOCK_MONOTONIC_RAW); }
}
void ac_cost_end(void)
{
    if (!active || !current) return;
    if (--depth) return;
    if (scope != CORE) invalid++;
    charge(); scope = 0; current = NULL;
}
void ac_cost_finish(uintptr_t id)
{
    request_t *r;
    if (!active) return;
    if (current) { invalid++; return; }
    r = lookup(id, 0);
    if (!r || !r->live) { missing++; return; }
    completed++;
    if (r->sampled) {
        if (samples == SAMPLES) { invalid++; return; }
        memcpy(sample_cpu[samples], r->cpu, sizeof(r->cpu));
        memcpy(sample_wall[samples], r->wall, sizeof(r->wall)); samples++;
    }
    r->live = 0; compact();
}

static void *count_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
    void *result;
    alloc_t *a = &allocation[scope];
    (void)ud;
    a->calls++;
    if (!ptr && nsize) { a->new_calls++; a->requested_new += nsize; }
    else if (!nsize) a->free_calls++;
    else a->resize_calls++;
    result = original_alloc(original_ud, ptr, osize, nsize);
    if (nsize && !result) a->failed++;
    else if (nsize && (!ptr || nsize > osize)) a->growth += ptr ? nsize - osize : nsize;
    return result;
}
static void profile_callback(void *ud, lua_State *L, int count, int state)
{
    (void)ud; (void)L;
    vm_samples += count; vm_states[(unsigned char)state] += count;
    if (state == 'G') { gc_samples += count; scope_gc[scope] += count; }
}

static int reset(lua_State *L)
{
    if (current) return luaL_error(L, "cannot reset an active request");
    memset(requests, 0, sizeof(requests)); memset(allocation, 0, sizeof(allocation));
    memset(vm_states, 0, sizeof(vm_states)); memset(scope_gc, 0, sizeof(scope_gc));
    accepted = completed = samples = invalid = missing = switches = 0;
    plugin_entries = sdk_entries = gc_samples = vm_samples = 0;
    scope = 0; depth = 0;
    lua_pushboolean(L, 1); return 1;
}

static int start(lua_State *L)
{
    struct itimerval timer;
    if (active || allocator_active || profile_active) return luaL_error(L, "cost sampler already active");
    stride = (unsigned)luaL_optinteger(L, 1, 64);
    if (!stride) return luaL_error(L, "stride must be positive");
    memset(requests, 0, sizeof(requests)); memset(allocation, 0, sizeof(allocation));
    memset(vm_states, 0, sizeof(vm_states)); memset(scope_gc, 0, sizeof(scope_gc));
    accepted = completed = samples = invalid = missing = switches = 0;
    plugin_entries = sdk_entries = 0;
    gc_samples = vm_samples = 0; current = NULL; scope = 0; depth = 0;
    original_alloc = lua_getallocf(L, &original_ud); lua_setallocf(L, count_alloc, NULL); allocator_active = 1;
    active = 1;
    luaJIT_profile_start(L, "i1", profile_callback, NULL); profile_active = 1;
    getitimer(ITIMER_PROF, &timer);
    if (timer.it_interval.tv_sec != 0 || timer.it_interval.tv_usec != 1000) invalid++;
    lua_pushboolean(L, 1); return 1;
}
static int compare_u64(const void *a, const void *b)
{ uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return (x > y) - (x < y); }
static void distribution(FILE *file, uint64_t data[SAMPLES][3], unsigned tag)
{
    uint64_t values[SAMPLES], total = 0, i;
    for (i = 0; i < samples; i++) { values[i] = data[i][tag]; total += values[i]; }
    qsort(values, samples, sizeof(uint64_t), compare_u64);
    fprintf(file, "{\"sample_total_ns\":%llu,\"mean_ns\":%.3f,\"p50_ns\":%llu,\"p95_ns\":%llu,\"p99_ns\":%llu}",
            (unsigned long long)total, samples ? (double)total / samples : 0,
            (unsigned long long)(samples ? values[(samples - 1) * 50 / 100] : 0),
            (unsigned long long)(samples ? values[(samples - 1) * 95 / 100] : 0),
            (unsigned long long)(samples ? values[(samples - 1) * 99 / 100] : 0));
}
static int stop(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    FILE *file; unsigned i; uint64_t pending = 0; void *ud;
    if (!active) return luaL_error(L, "cost sampler not active");
    active = 0;
    if (current) invalid++;
    if (profile_active) { luaJIT_profile_stop(L); profile_active = 0; }
    if (lua_getallocf(L, &ud) != count_alloc) return luaL_error(L, "allocator ownership changed");
    lua_setallocf(L, original_alloc, original_ud); allocator_active = 0;
    for (i = 0; i < SLOTS; i++) if (requests[i].live) pending++;
    file = fopen(path, "w"); if (!file) return luaL_error(L, "cannot write cost profile");
    fprintf(file, "{\"accepted\":%llu,\"completed\":%llu,\"sampled_completed\":%llu,\"stride\":%u,"
            "\"invalid\":%llu,\"missing\":%llu,\"pending\":%llu,\"scope_switches\":%llu,"
            "\"gc_cpu_samples\":%llu,\"total_cpu_samples\":%llu,\"gc_callback_scope_samples\":[%llu,%llu,%llu],",
            (unsigned long long)accepted,(unsigned long long)completed,(unsigned long long)samples,stride,
            (unsigned long long)invalid,(unsigned long long)missing,(unsigned long long)pending,
            (unsigned long long)switches,(unsigned long long)gc_samples,(unsigned long long)vm_samples,
            (unsigned long long)scope_gc[0],(unsigned long long)scope_gc[1],(unsigned long long)scope_gc[2]);
    fprintf(file, "\"core_cpu\":"); distribution(file, sample_cpu, CORE);
    fprintf(file, ",\"core_active_wall\":"); distribution(file, sample_wall, CORE);
    fprintf(file, ",\"plugin_cpu\":"); distribution(file, sample_cpu, PLUGIN);
    fprintf(file, ",\"plugin_active_wall\":"); distribution(file, sample_wall, PLUGIN);
    fprintf(file, ",\"allocator\":[");
    for (i = 0; i < 3; i++) {
        alloc_t *a = &allocation[i];
        fprintf(file, "%s{\"calls\":%llu,\"new_calls\":%llu,\"resize_calls\":%llu,\"free_calls\":%llu,"
                "\"failed\":%llu,\"requested_new_bytes\":%llu,\"successful_growth_bytes\":%llu}", i ? "," : "",
                (unsigned long long)a->calls,(unsigned long long)a->new_calls,(unsigned long long)a->resize_calls,
                (unsigned long long)a->free_calls,(unsigned long long)a->failed,
                (unsigned long long)a->requested_new,(unsigned long long)a->growth);
    }
    fprintf(file, "],\"plugin_entries\":%llu,\"sdk_entries\":%llu,\"allocator_restored\":true,\"gc_sampling_interval_us\":1000}\n",
            (unsigned long long)plugin_entries, (unsigned long long)sdk_entries); fclose(file);
    lua_pushboolean(L, 1); return 1;
}
int luaopen_core_cost_sampler(lua_State *L)
{
    static const luaL_Reg methods[] = {{"start",start},{"reset",reset},{"stop",stop},{NULL,NULL}};
    lua_newtable(L); luaL_register(L,NULL,methods); return 1;
}
