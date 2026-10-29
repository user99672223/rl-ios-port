/*
 * probe.c - what does this macOS ARM64 host actually allow? (stand-in for iOS, see PROJECT_SPEC 5)
 *
 * Prints one "PROBE <name>: <result>" line per question. Facts only, no pass/fail: the numbers
 * decide how Wine's virtual memory manager and FEX's JIT buffer have to be set up.
 *
 *   probe            run everything that is safe in one process
 *   probe rwx        only the "W+X without MAP_JIT" check (used under lldb: debugger attached)
 *   probe x18        only the x18 checks (probe.sh builds this with several -mmacosx-version-min values)
 *
 * Every API used here is documented in the macOS SDK headers / man pages:
 *   mmap(2) MAP_JIT, pthread_jit_write_protect_np(3) and sys_icache_invalidate(3)
 *   (<pthread.h>, <libkern/OSCacheControl.h>), sysctlbyname(3).
 */
#include <errno.h>
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <unistd.h>

#define PROBE(name, ...) do { printf("PROBE %s: ", name); printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)

typedef int (*fn_t)(void);

static const uint32_t code_ret42[] = { 0x52800540, 0xd65f03c0 };   /* mov w0,#42 ; ret */

static void try_map_fixed(const char *name, uintptr_t addr, size_t len)
{
    void *p = mmap((void *)addr, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
    if (p == MAP_FAILED) PROBE(name, "mmap(%#lx, %#zx, MAP_FIXED) failed: %s", (unsigned long)addr, len, strerror(errno));
    else { PROBE(name, "mmap(%#lx, %#zx, MAP_FIXED) ok at %p", (unsigned long)addr, len, p); munmap(p, len); }
}

static void try_map_hint(const char *name, uintptr_t addr, size_t len)
{
    void *p = mmap((void *)addr, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) PROBE(name, "mmap hint %#lx failed: %s", (unsigned long)addr, strerror(errno));
    else { PROBE(name, "mmap hint %#lx -> %p", (unsigned long)addr, p); munmap(p, len); }
}

static void probe_rwx_plain(void)
{
    void *p = mmap(NULL, 0x4000, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) { PROBE("rwx_anon_no_map_jit", "mmap(RWX) failed: %s", strerror(errno)); }
    else {
        PROBE("rwx_anon_no_map_jit", "mmap(RWX) OK at %p", p);
        memcpy(p, code_ret42, sizeof(code_ret42));
        sys_icache_invalidate(p, sizeof(code_ret42));
        PROBE("rwx_anon_no_map_jit_exec", "executed, returned %d", ((fn_t)p)());
        munmap(p, 0x4000);
    }

    p = mmap(NULL, 0x4000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p != MAP_FAILED) {
        int r = mprotect(p, 0x4000, PROT_READ | PROT_WRITE | PROT_EXEC);
        PROBE("mprotect_rw_to_rwx", "%s", r ? strerror(errno) : "OK");
        r = mprotect(p, 0x4000, PROT_READ | PROT_EXEC);
        PROBE("mprotect_rw_to_rx", "%s", r ? strerror(errno) : "OK");
        munmap(p, 0x4000);
    }
}

static void probe_map_jit(void)
{
    void *p = mmap(NULL, 0x4000, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (p == MAP_FAILED) { PROBE("map_jit_rwx", "mmap(RWX|MAP_JIT) failed: %s", strerror(errno)); return; }
    PROBE("map_jit_rwx", "OK at %p", p);
    pthread_jit_write_protect_np(0);                     /* this thread: writable, not executable */
    memcpy(p, code_ret42, sizeof(code_ret42));
    pthread_jit_write_protect_np(1);                     /* this thread: executable, not writable */
    sys_icache_invalidate(p, sizeof(code_ret42));
    PROBE("map_jit_toggle_exec", "write-protect toggle + execute returned %d", ((fn_t)p)());
    munmap(p, 0x4000);
}

static volatile uint64_t x18_in_handler, x18_after_signal;
static void on_usr1(int sig) { uint64_t v; (void)sig; __asm__ volatile("mov %0, x18" : "=r"(v)); x18_in_handler = v; }

static void probe_x18(void)
{
    uint64_t v;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_usr1;
    sigaction(SIGUSR1, &sa, NULL);

    __asm__ volatile("mov x18, %0" : : "r"((uint64_t)0x1122334455667788ull));
    getpid();                                            /* libc -> svc */
    __asm__ volatile("mov %0, x18" : "=r"(v));
    PROBE("x18_after_syscall", "wrote 0x1122334455667788, read back %#llx (%s)", (unsigned long long)v,
          v == 0x1122334455667788ull ? "preserved" : "NOT preserved");

    __asm__ volatile("mov x18, %0" : : "r"((uint64_t)0x1122334455667788ull));
    raise(SIGUSR1);
    __asm__ volatile("mov %0, x18" : "=r"(x18_after_signal));
    PROBE("x18_in_signal_handler", "%#llx on entry", (unsigned long long)x18_in_handler);
    PROBE("x18_after_signal_return", "%#llx (%s)", (unsigned long long)x18_after_signal,
          x18_after_signal == 0x1122334455667788ull ? "preserved" : "NOT preserved");

    {   /* a context switch: yield a few times and sleep, then re-read */
        int i;
        __asm__ volatile("mov x18, %0" : : "r"((uint64_t)0x1122334455667788ull));
        for (i = 0; i < 100; i++) usleep(100);
        sched_yield();
        __asm__ volatile("mov %0, x18" : "=r"(v));
        PROBE("x18_after_sleep", "%#llx (%s)", (unsigned long long)v, v == 0x1122334455667788ull ? "preserved" : "NOT preserved");
    }
}

/* Does x18 survive plain computation under CPU contention (preemption), or only syscalls/signals that
 * reschedule? One spinning thread per CPU plus extras, each checking x18 in a tight loop. */
static volatile uint64_t spin_zeroed, spin_checks;
static void *spin_thread(void *arg)
{
    uint64_t v, zeros = 0, n = 0;
    struct timespec t0, t;
    (void)arg;
    __asm__ volatile("mov x18, %0" : : "r"((uint64_t)0x1122334455667788ull));
    clock_gettime(CLOCK_MONOTONIC, &t0);
    do {
        int i;
        for (i = 0; i < 100000; i++) {
            __asm__ volatile("mov %0, x18" : "=r"(v));
            n++;
            if (v != 0x1122334455667788ull) { zeros++; __asm__ volatile("mov x18, %0" : : "r"((uint64_t)0x1122334455667788ull)); }
        }
        clock_gettime(CLOCK_MONOTONIC, &t);       /* vDSO-less on macOS: commpage read, no scheduling point */
    } while ((t.tv_sec - t0.tv_sec) * 1000 + (t.tv_nsec - t0.tv_nsec) / 1000000 < 2000);
    __sync_fetch_and_add(&spin_zeroed, zeros);
    __sync_fetch_and_add(&spin_checks, n);
    return NULL;
}

static void probe_x18_preemption(void)
{
    pthread_t th[16];
    int i, n = 2 * (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (n > 16) n = 16;
    for (i = 0; i < n; i++) pthread_create(&th[i], NULL, spin_thread, NULL);
    for (i = 0; i < n; i++) pthread_join(th[i], NULL);
    PROBE("x18_preemption_spin", "%d threads on %ld CPUs, %llu x18 reads in pure user code, %llu observed clobbered",
          n, sysconf(_SC_NPROCESSORS_ONLN), (unsigned long long)spin_checks, (unsigned long long)spin_zeroed);
}

static void probe_tpidr(void)
{
    uint64_t ro, rw;
    __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(ro));
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(rw));
    PROBE("tpidr", "TPIDRRO_EL0=%#llx TPIDR_EL0=%#llx", (unsigned long long)ro, (unsigned long long)rw);
    __asm__ volatile("msr tpidr_el0, %0" : : "r"((uint64_t)0xabcdef0));
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(rw));
    PROBE("tpidr_el0_writable", "wrote 0xabcdef0, read %#llx", (unsigned long long)rw);
    __asm__ volatile("msr tpidr_el0, %0" : : "r"((uint64_t)0));
}

static void sysctl_str(const char *name)
{
    char buf[256]; size_t len = sizeof(buf);
    if (sysctlbyname(name, buf, &len, NULL, 0) == 0) PROBE(name, "%s", buf);
    else PROBE(name, "(n/a)");
}

static void sysctl_int(const char *name)
{
    int64_t v = 0; size_t len = sizeof(v);
    if (sysctlbyname(name, &v, &len, NULL, 0) == 0) PROBE(name, "%lld", (long long)(len == 4 ? *(int32_t *)&v : v));
    else PROBE(name, "(n/a)");
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "rwx")) { probe_rwx_plain(); return 0; }
    if (argc > 1 && !strcmp(argv[1], "x18")) { probe_x18(); probe_x18_preemption(); return 0; }

    PROBE("sizeof_pointer", "%zu", sizeof(void *));
    PROBE("getpagesize", "%d", getpagesize());
    sysctl_int("hw.pagesize");
    sysctl_int("vm.pagesize");
    sysctl_str("machdep.cpu.brand_string");
    sysctl_int("hw.ncpu");
    sysctl_int("hw.memsize");
    sysctl_int("hw.optional.arm.FEAT_TSO");        /* only reports if the key exists */
    sysctl_int("hw.optional.arm64");
    sysctl_int("kern.hv_support");

    probe_rwx_plain();
    probe_map_jit();
    probe_x18();
    probe_x18_preemption();
    probe_tpidr();

    try_map_fixed("map_fixed_0x7ffe0000", 0x7ffe0000, 0x1000);
    try_map_fixed("map_fixed_0x10000", 0x10000, 0x10000);
    try_map_fixed("map_fixed_just_below_4g", 0xfffff0000ull, 0x4000);
    try_map_fixed("map_fixed_4g", 0x100000000ull, 0x4000);
    try_map_fixed("map_fixed_0x180000000_default_x64_dll_base", 0x180000000ull, 0x10000);
    try_map_fixed("map_fixed_0x140000000_default_x64_exe_base", 0x140000000ull, 0x10000);
    try_map_fixed("map_fixed_64g", 0x1000000000ull, 0x4000);
    try_map_fixed("map_fixed_0x7000000000", 0x7000000000ull, 0x4000);
    try_map_fixed("map_fixed_0x7ffffe000000", 0x7ffffe000000ull, 0x4000);
    try_map_fixed("map_fixed_0x6ffffe000000", 0x6ffffe000000ull, 0x4000);
    try_map_hint("map_hint_0x7ffe0000", 0x7ffe0000, 0x1000);
    try_map_hint("map_hint_0x10000000", 0x10000000, 0x4000);

    {   /* 4 KB granularity inside a 16 KB page: what does the kernel do to a sub-page mprotect? */
        char *p = mmap(NULL, 0x8000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (p != MAP_FAILED) {
            int r = mprotect(p + 0x1000, 0x1000, PROT_READ);
            PROBE("mprotect_4k_subpage", "%s", r ? strerror(errno) : "OK (rounded or applied)");
            r = mprotect(p + 0x4000, 0x4000, PROT_NONE);
            PROBE("mprotect_16k_aligned", "%s", r ? strerror(errno) : "OK");
            munmap(p, 0x8000);
        }
    }
    return 0;
}
