/* SPDX-License-Identifier: Apache-2.0 */
/* Development-only Linux PC sampling. Never linked into the gateway module. */
#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <ucontext.h>

#define AC_SAMPLE_CAPACITY 200000
static uintptr_t sample_pcs[AC_SAMPLE_CAPACITY];
static volatile sig_atomic_t sample_count;
static struct sigaction old_action;
static struct itimerval old_timer;

static void
sample_pc(int signo, siginfo_t *info, void *context)
{
    ucontext_t *state = context;
    sig_atomic_t index = sample_count;
    (void) signo;
    (void) info;
    if (index >= AC_SAMPLE_CAPACITY) return;
#if defined(__x86_64__)
    sample_pcs[index] = (uintptr_t) state->uc_mcontext.gregs[REG_RIP];
#elif defined(__aarch64__)
    sample_pcs[index] = (uintptr_t) state->uc_mcontext.pc;
#else
#error Unsupported development sampler architecture
#endif
    sample_count = index + 1;
}

int
ac_profile_start(void)
{
    struct sigaction action;
    struct itimerval timer = {{0, 1000}, {0, 1000}};
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = sample_pc;
    action.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&action.sa_mask);
    sample_count = 0;
    if (sigaction(SIGPROF, &action, &old_action)) return -1;
    if (setitimer(ITIMER_PROF, &timer, &old_timer)) {
        sigaction(SIGPROF, &old_action, NULL);
        return -1;
    }
    return 0;
}

int
ac_profile_stop(const char *output)
{
    struct itimerval timer = {{0, 0}, {0, 0}};
    FILE *file;
    sig_atomic_t index;
    if (setitimer(ITIMER_PROF, &timer, NULL)) return -1;
    sigaction(SIGPROF, &old_action, NULL);
    setitimer(ITIMER_PROF, &old_timer, NULL);
    file = fopen(output, "w");
    if (!file) return -1;
    for (index = 0; index < sample_count; index++)
        fprintf(file, "%lx\n", (unsigned long) sample_pcs[index]);
    if (fclose(file)) return -1;
    return (int) sample_count;
}
