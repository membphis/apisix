/* SPDX-License-Identifier: Apache-2.0 */
/* Diagnostic GCC function instrumentation; excluded from normal module builds. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define AC_NOINSTR __attribute__((no_instrument_function))
#define AC_BUCKETS 16384
typedef struct { void *function; uint64_t count, inclusive, self; } ac_bucket_t;
typedef struct { ac_bucket_t *bucket; uint64_t started, children; } ac_frame_t;
static ac_bucket_t buckets[AC_BUCKETS];
static ac_frame_t frames[1024];
static unsigned depth;
static int enabled, invalid;

static AC_NOINSTR uint64_t
stamp(void)
{
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC_RAW, &time);
    return (uint64_t) time.tv_sec * 1000000000 + time.tv_nsec;
}

AC_NOINSTR __attribute__((visibility("hidden"))) void
__cyg_profile_func_enter(void *function, void *caller)
{
    uintptr_t index;
    ac_bucket_t *bucket;
    (void) caller;
    if (!enabled) return;
    if (depth == 1024) { enabled = 0; invalid = 1; return; }
    index = (((uintptr_t) function >> 4) ^ ((uintptr_t) function >> 16)) & (AC_BUCKETS - 1);
    while (buckets[index].function && buckets[index].function != function)
        index = (index + 1) & (AC_BUCKETS - 1);
    bucket = &buckets[index]; bucket->function = function;
    frames[depth].bucket = bucket; frames[depth].children = 0;
    frames[depth++].started = stamp();
}

AC_NOINSTR __attribute__((visibility("hidden"))) void
__cyg_profile_func_exit(void *function, void *caller)
{
    uint64_t elapsed;
    ac_frame_t *frame;
    (void) caller;
    if (!enabled) return;
    if (!depth || frames[depth - 1].bucket->function != function) {
        enabled = 0; invalid = 1; return;
    }
    frame = &frames[--depth]; elapsed = stamp() - frame->started;
    frame->bucket->count++; frame->bucket->inclusive += elapsed;
    frame->bucket->self += elapsed - frame->children;
    if (depth) frames[depth - 1].children += elapsed;
}

AC_NOINSTR int
ac_function_profile_start(void)
{
    memset(buckets, 0, sizeof(buckets)); depth = 0; invalid = 0; enabled = 1; return 0;
}

AC_NOINSTR int
ac_function_profile_stop(const char *output)
{
    FILE *file;
    Dl_info info;
    unsigned i;
    enabled = 0;
    if (invalid || depth) return -1;
    file = fopen(output, "w");
    if (!file) return -1;
    for (i = 0; i < AC_BUCKETS; i++) {
        if (!buckets[i].function || !dladdr(buckets[i].function, &info)) continue;
        fprintf(file, "%lx %llu %llu %llu\n",
                (unsigned long) ((uintptr_t) buckets[i].function - (uintptr_t) info.dli_fbase),
                (unsigned long long) buckets[i].count,
                (unsigned long long) buckets[i].inclusive,
                (unsigned long long) buckets[i].self);
    }
    return fclose(file);
}
