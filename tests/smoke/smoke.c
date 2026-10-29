/*
 * smoke.c - synthetic x86-64 Windows console program for the Wine (ARM64EC) + FEX stack.
 *
 * Synthetic code only: nothing here comes from, or is derived from, any game.
 *
 * Output: one "PASS <name>" or "FAIL <name>: <detail>" line per check, "INFO ..." lines for
 * environment facts, and a final "SUMMARY checks=N failures=F" line.
 * Exit code = number of failed checks (capped at 255 by the OS). A crash mid-run leaves no
 * SUMMARY line; run sections individually to isolate it:  smoke.exe <section>
 *
 *   smoke.exe             run every section
 *   smoke.exe --list      print section names
 *   smoke.exe threads     run one section (several names may be given)
 *
 * Build (CI and local):
 *   x86_64-w64-mingw32-clang -O1 -g -static -fms-extensions -o smoke.exe smoke.c   (llvm-mingw)
 *   x86_64-w64-mingw32-gcc   -O1 -g -static -o smoke.exe smoke.c                   (Debian mingw-w64)
 * The frame-based SEH checks (__try/__except/__finally) are compiled only under clang.
 */
#define _WIN32_WINNT 0x0A00
#define __USE_MINGW_ANSI_STDIO 1   /* %zx, %tx, %llx in the formatted output below */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------------------------ */
/* reporting                                                                                   */
/* ------------------------------------------------------------------------------------------ */

static int g_checks, g_failures;

static void check(int ok, const char *name, const char *fmt, ...)
{
    g_checks++;
    if (ok) {
        printf("PASS %s\n", name);
    } else {
        char buf[512];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(buf, sizeof(buf), fmt ? fmt : "", ap);
        va_end(ap);
        g_failures++;
        printf("FAIL %s: %s\n", name, buf);
    }
    fflush(stdout);
}

static void info(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    printf("INFO %s\n", buf);
    fflush(stdout);
}

#define OK(cond, name) check((cond) ? 1 : 0, name, "%s (line %d, GetLastError=%lu)", #cond, __LINE__, GetLastError())
#define EQ(a, b, name) do { long long _a = (long long)(a), _b = (long long)(b); \
    check(_a == _b, name, "%s == %s: got 0x%llx expected 0x%llx (line %d)", #a, #b, _a, _b, __LINE__); } while (0)


/* ------------------------------------------------------------------------------------------ */
/* vectored exception handler shared by the seh / virtual / fiber / threadname sections         */
/* ------------------------------------------------------------------------------------------ */

enum veh_action {
    VEH_NONE = 0,
    VEH_COMMIT,          /* commit the faulting page read/write, then resume */
    VEH_PROTECT_RW,      /* make the faulting page read/write, then resume   */
    VEH_FIXUP_RAX,       /* point RAX at a valid variable, then resume       */
    VEH_SKIP_DIV,        /* advance RIP past a 2-byte "div %ecx"             */
    VEH_SKIP_INT3,       /* advance RIP past int3 (x64 Windows reports RIP at the 0xCC byte) */
    VEH_PLAIN_RESUME,    /* just resume                                      */
    VEH_RECORD_ONLY,     /* record, then continue the search                 */
};

static struct {
    volatile LONG   armed_code;      /* exception code the handler should react to (0 = none) */
    enum veh_action action;
    volatile LONG   hits;
    DWORD           code;
    DWORD           flags;
    ULONG_PTR       nparams;
    ULONG_PTR       info[4];
    void           *exc_addr;
    DWORD           tid;
    DWORD64         rip_in_ctx;
    char            name_copy[64];   /* for the thread-name exception */
} g_veh;

static volatile int g_fixup_value = 0x1234abcd;

static LONG CALLBACK veh(EXCEPTION_POINTERS *ep)
{
    EXCEPTION_RECORD *r = ep->ExceptionRecord;
    CONTEXT *c = ep->ContextRecord;
    ULONG i;

    if (!g_veh.armed_code || (DWORD)g_veh.armed_code != r->ExceptionCode)
        return EXCEPTION_CONTINUE_SEARCH;

    g_veh.hits++;
    g_veh.code = r->ExceptionCode;
    g_veh.flags = r->ExceptionFlags;
    g_veh.nparams = r->NumberParameters;
    for (i = 0; i < 4 && i < r->NumberParameters; i++) g_veh.info[i] = r->ExceptionInformation[i];
    g_veh.exc_addr = r->ExceptionAddress;
    g_veh.tid = GetCurrentThreadId();
    g_veh.rip_in_ctx = c->Rip;

    if (r->ExceptionCode == 0x406D1388 && r->NumberParameters >= 2) {
        const char *s = (const char *)r->ExceptionInformation[1];
        /* THREADNAME_INFO: dwType, szName, dwThreadID, dwFlags (ExceptionInformation is ULONG_PTR[]) */
        g_veh.name_copy[0] = 0;
        if (s) { strncpy(g_veh.name_copy, s, sizeof(g_veh.name_copy) - 1); g_veh.name_copy[sizeof(g_veh.name_copy) - 1] = 0; }
    }

    switch (g_veh.action) {
    case VEH_COMMIT: {
        void *page = (void *)(r->ExceptionInformation[1] & ~(ULONG_PTR)0xfff);
        void *p = VirtualAlloc(page, 0x1000, MEM_COMMIT, PAGE_READWRITE);
        return p ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
    }
    case VEH_PROTECT_RW: {
        void *page = (void *)(r->ExceptionInformation[1] & ~(ULONG_PTR)0xfff);
        DWORD old;
        return VirtualProtect(page, 0x1000, PAGE_READWRITE, &old) ? EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
    }
    case VEH_FIXUP_RAX:
        c->Rax = (DWORD64)(uintptr_t)&g_fixup_value;
        return EXCEPTION_CONTINUE_EXECUTION;
    case VEH_SKIP_DIV:
        if (*(volatile uint16_t *)(uintptr_t)c->Rip != 0xf1f7) return EXCEPTION_CONTINUE_SEARCH;
        c->Rip += 2;
        return EXCEPTION_CONTINUE_EXECUTION;
    case VEH_SKIP_INT3:
        if (*(volatile uint8_t *)(uintptr_t)c->Rip != 0xcc) return EXCEPTION_CONTINUE_SEARCH;
        c->Rip += 1;
        return EXCEPTION_CONTINUE_EXECUTION;
    case VEH_PLAIN_RESUME:
        return EXCEPTION_CONTINUE_EXECUTION;
    case VEH_RECORD_ONLY:
    default:
        return EXCEPTION_CONTINUE_SEARCH;
    }
}

static void veh_arm(DWORD code, enum veh_action action)
{
    memset(&g_veh, 0, sizeof(g_veh));
    g_veh.action = action;
    g_veh.armed_code = (LONG)code;
}

static void veh_disarm(void) { g_veh.armed_code = 0; }

/* ------------------------------------------------------------------------------------------ */
/* section: env - environment facts, page size / granularity as the guest sees them             */
/* ------------------------------------------------------------------------------------------ */

typedef BOOL (WINAPI *IsWow64Process2_t)(HANDLE, USHORT *, USHORT *);

static void section_env(void)
{
    SYSTEM_INFO si;
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    const char *(CDECL *wine_get_version)(void) = ntdll ? (const char *(CDECL *)(void))GetProcAddress(ntdll, "wine_get_version") : NULL;
    IsWow64Process2_t pIsWow64Process2 = (IsWow64Process2_t)GetProcAddress(GetModuleHandleA("kernel32.dll"), "IsWow64Process2");
    USHORT proc_machine = 0, native_machine = 0;

    GetNativeSystemInfo(&si);
    info("sizeof(void*)=%u wine=%s", (unsigned)sizeof(void *), wine_get_version ? wine_get_version() : "(not wine)");
    info("native: arch=%u pagesize=%lu granularity=%lu cpus=%lu", si.wProcessorArchitecture, si.dwPageSize,
         si.dwAllocationGranularity, si.dwNumberOfProcessors);
    GetSystemInfo(&si);
    info("guest : arch=%u pagesize=%lu granularity=%lu cpus=%lu", si.wProcessorArchitecture, si.dwPageSize,
         si.dwAllocationGranularity, si.dwNumberOfProcessors);
    if (pIsWow64Process2 && pIsWow64Process2(GetCurrentProcess(), &proc_machine, &native_machine))
        info("IsWow64Process2: process machine=0x%04x native machine=0x%04x", proc_machine, native_machine);

    EQ(si.dwPageSize, 4096, "env.page_size_is_4k");
    EQ(si.dwAllocationGranularity, 65536, "env.allocation_granularity_is_64k");
    OK(si.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64, "env.guest_reports_amd64");
    OK(si.dwNumberOfProcessors >= 1, "env.cpu_count");
    OK(IsProcessorFeaturePresent(PF_XMMI64_INSTRUCTIONS_AVAILABLE), "env.sse2_feature");
}

/* ------------------------------------------------------------------------------------------ */
/* section: threads - threads, events, semaphores, critical sections, SRW locks                 */
/* ------------------------------------------------------------------------------------------ */

#define NTHREADS 4
#define ITERS    20000

static CRITICAL_SECTION g_cs;
static SRWLOCK g_srw = SRWLOCK_INIT;
static volatile long long g_counter;
static HANDLE g_start_evt;
static HANDLE g_ready_evt[NTHREADS];

struct tparam { int id; int use_srw; };

static DWORD WINAPI counter_thread(LPVOID arg)
{
    struct tparam *p = arg;
    int i;
    SetEvent(g_ready_evt[p->id]);
    if (WaitForSingleObject(g_start_evt, 20000) != WAIT_OBJECT_0) return 0xdead;
    for (i = 0; i < ITERS; i++) {
        if (p->use_srw) AcquireSRWLockExclusive(&g_srw); else EnterCriticalSection(&g_cs);
        {   /* deliberately non-atomic read-modify-write: only the lock keeps it correct */
            long long v = g_counter;
            v++;
            g_counter = v;
        }
        if (p->use_srw) ReleaseSRWLockExclusive(&g_srw); else LeaveCriticalSection(&g_cs);
    }
    return 100 + p->id;
}

static void run_contended(int use_srw, const char *tag)
{
    HANDLE th[NTHREADS];
    struct tparam prm[NTHREADS];
    char nm[96];
    DWORD code, w;
    int i, all_started = 1, exit_ok = 1;

    g_counter = 0;
    g_start_evt = CreateEventA(NULL, TRUE, FALSE, NULL);     /* manual reset */
    for (i = 0; i < NTHREADS; i++) {
        g_ready_evt[i] = CreateEventA(NULL, FALSE, FALSE, NULL);
        prm[i].id = i; prm[i].use_srw = use_srw;
        th[i] = CreateThread(NULL, 0, counter_thread, &prm[i], 0, NULL);
        if (!th[i]) all_started = 0;
    }
    snprintf(nm, sizeof(nm), "threads.%s.create_4_threads", tag);
    OK(all_started, nm);
    w = WaitForMultipleObjects(NTHREADS, g_ready_evt, TRUE, 20000);
    snprintf(nm, sizeof(nm), "threads.%s.all_threads_signalled_ready", tag);
    EQ(w, WAIT_OBJECT_0, nm);
    SetEvent(g_start_evt);                                   /* releases all four at once */
    w = WaitForMultipleObjects(NTHREADS, th, TRUE, 60000);
    snprintf(nm, sizeof(nm), "threads.%s.join_all", tag);
    EQ(w, WAIT_OBJECT_0, nm);
    for (i = 0; i < NTHREADS; i++) {
        code = 0;
        GetExitCodeThread(th[i], &code);
        if (code != (DWORD)(100 + i)) exit_ok = 0;
        CloseHandle(th[i]);
        CloseHandle(g_ready_evt[i]);
    }
    snprintf(nm, sizeof(nm), "threads.%s.exit_codes", tag);
    OK(exit_ok, nm);
    snprintf(nm, sizeof(nm), "threads.%s.contended_counter", tag);
    EQ(g_counter, (long long)NTHREADS * ITERS, nm);
    CloseHandle(g_start_evt);
}

static HANDLE g_pp_a, g_pp_b;
static volatile int g_pp_val;
static DWORD WINAPI pingpong_thread(LPVOID arg)
{
    int i, n = (int)(intptr_t)arg;
    for (i = 0; i < n; i++) {
        if (WaitForSingleObject(g_pp_a, 20000) != WAIT_OBJECT_0) return 1;
        g_pp_val++;
        SetEvent(g_pp_b);
    }
    return 0;
}

static void section_threads(void)
{
    CRITICAL_SECTION cs2;
    HANDLE ev, sem, th;
    DWORD code = 99;
    int i, seq_ok = 1;

    InitializeCriticalSection(&g_cs);
    run_contended(0, "critsec");
    run_contended(1, "srwlock");
    DeleteCriticalSection(&g_cs);

    /* critical section semantics */
    InitializeCriticalSectionAndSpinCount(&cs2, 100);
    OK(TryEnterCriticalSection(&cs2), "threads.critsec.tryenter_free");
    EnterCriticalSection(&cs2);
    EQ(cs2.RecursionCount, 2, "threads.critsec.recursion_count");
    LeaveCriticalSection(&cs2);
    LeaveCriticalSection(&cs2);
    EQ(cs2.RecursionCount, 0, "threads.critsec.recursion_released");
    DeleteCriticalSection(&cs2);

    /* auto-reset event semantics */
    ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    EQ(WaitForSingleObject(ev, 0), WAIT_TIMEOUT, "threads.event.unsignalled_times_out");
    SetEvent(ev);
    EQ(WaitForSingleObject(ev, 0), WAIT_OBJECT_0, "threads.event.autoreset_first_wait");
    EQ(WaitForSingleObject(ev, 0), WAIT_TIMEOUT, "threads.event.autoreset_second_wait");
    CloseHandle(ev);

    /* counting semaphore */
    sem = CreateSemaphoreA(NULL, 0, 4, NULL);
    ReleaseSemaphore(sem, 2, NULL);
    EQ(WaitForSingleObject(sem, 0), WAIT_OBJECT_0, "threads.semaphore.first");
    EQ(WaitForSingleObject(sem, 0), WAIT_OBJECT_0, "threads.semaphore.second");
    EQ(WaitForSingleObject(sem, 0), WAIT_TIMEOUT, "threads.semaphore.third_times_out");
    CloseHandle(sem);

    /* event ping-pong between two threads: strict alternation 500 times */
    g_pp_a = CreateEventA(NULL, FALSE, FALSE, NULL);
    g_pp_b = CreateEventA(NULL, FALSE, FALSE, NULL);
    g_pp_val = 0;
    th = CreateThread(NULL, 0, pingpong_thread, (LPVOID)(intptr_t)500, 0, NULL);
    for (i = 0; i < 500; i++) {
        int before = g_pp_val;
        SetEvent(g_pp_a);
        if (WaitForSingleObject(g_pp_b, 20000) != WAIT_OBJECT_0) { seq_ok = 0; break; }
        if (g_pp_val != before + 1) { seq_ok = 0; break; }
    }
    OK(seq_ok, "threads.event.pingpong_500");
    WaitForSingleObject(th, 20000);
    GetExitCodeThread(th, &code);
    EQ(code, 0, "threads.event.pingpong_thread_exit");
    CloseHandle(th);
    CloseHandle(g_pp_a);
    CloseHandle(g_pp_b);
}

/* ------------------------------------------------------------------------------------------ */
/* section: fileio - mixed-case file names                                                      */
/* ------------------------------------------------------------------------------------------ */

static void section_fileio(void)
{
    static const char payload[] = "mixed-case payload 0123456789\r\n";
    char buf[128];
    DWORD n = 0, attr;
    HANDLE h, f;
    WIN32_FIND_DATAA fd;
    int found_exact_case = 0, found_any = 0;
    FILE *cf;

    /* start from a clean directory (ignore errors) */
    DeleteFileA("SmokeDir\\test.txt");
    RemoveDirectoryA("SMOKEDIR");

    OK(CreateDirectoryA("SmokeDir", NULL), "fileio.create_directory");
    h = CreateFileA("SmokeDir\\TeSt.TxT", GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    OK(h != INVALID_HANDLE_VALUE, "fileio.create_TeSt.TxT");
    if (h != INVALID_HANDLE_VALUE) {
        OK(WriteFile(h, payload, sizeof(payload) - 1, &n, NULL) && n == sizeof(payload) - 1, "fileio.write");
        CloseHandle(h);
    }

    /* open with different case in both the directory and file component */
    h = CreateFileA("smokedir\\test.txt", GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    OK(h != INVALID_HANDLE_VALUE, "fileio.open_as_lowercase");
    if (h != INVALID_HANDLE_VALUE) {
        memset(buf, 0, sizeof(buf));
        n = 0;
        OK(ReadFile(h, buf, sizeof(buf) - 1, &n, NULL), "fileio.read");
        check(n == sizeof(payload) - 1 && memcmp(buf, payload, n) == 0, "fileio.read_back_matches", "read %lu bytes: '%.40s'", n, buf);
        EQ(GetFileSize(h, NULL), sizeof(payload) - 1, "fileio.size");
        CloseHandle(h);
    }

    /* a second file name differing only by case must collide, not create a twin */
    f = CreateFileA("SMOKEDIR\\TEST.TXT", GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    check(f == INVALID_HANDLE_VALUE && GetLastError() == ERROR_FILE_EXISTS, "fileio.create_new_collides_across_case",
          "handle=%p err=%lu", f, GetLastError());
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);

    /* directory listing returns the name as created */
    h = FindFirstFileA("SMOKEDIR\\*", &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            found_any++;
            if (!strcmp(fd.cFileName, "TeSt.TxT")) found_exact_case = 1;
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    check(found_exact_case && found_any == 1, "fileio.findfirst_preserves_case", "entries=%d exact=%d name='%s'", found_any, found_exact_case, fd.cFileName);

    /* wildcard match is case-insensitive */
    h = FindFirstFileA("smokedir\\T*.TXT", &fd);
    OK(h != INVALID_HANDLE_VALUE, "fileio.wildcard_case_insensitive");
    if (h != INVALID_HANDLE_VALUE) FindClose(h);

    attr = GetFileAttributesA("sMoKeDiR\\tEsT.tXt");
    OK(attr != INVALID_FILE_ATTRIBUTES, "fileio.getfileattributes_other_case");

    /* the C runtime goes through the same lookup */
    cf = fopen("SMOKEDIR/test.TXT", "rb");
    OK(cf != NULL, "fileio.crt_fopen_other_case");
    if (cf) {
        memset(buf, 0, sizeof(buf));
        n = (DWORD)fread(buf, 1, sizeof(buf) - 1, cf);
        EQ(n, sizeof(payload) - 1, "fileio.crt_fread_size");
        fclose(cf);
    }

    OK(MoveFileA("SMOKEDIR\\TEST.txt", "SmokeDir\\Renamed.Dat"), "fileio.move_with_other_case_source");
    OK(DeleteFileA("smokedir\\RENAMED.DAT"), "fileio.delete_other_case");
    OK(RemoveDirectoryA("SMOKEDIR"), "fileio.remove_directory_other_case");
    attr = GetFileAttributesA("SmokeDir");
    OK(attr == INVALID_FILE_ATTRIBUTES, "fileio.directory_gone");
}

/* ------------------------------------------------------------------------------------------ */
/* section: seh - vectored handlers, access violations, frame-based SEH (clang only)            */
/* ------------------------------------------------------------------------------------------ */

static int div_by_zero_via_asm(void)
{
    int ret = 0x55;
    __asm__ volatile("xorl %%edx, %%edx\n\t"
                     "xorl %%ecx, %%ecx\n\t"
                     "movl $1, %%eax\n\t"
                     "divl %%ecx\n\t"          /* F7 F1: 2 bytes, faults with #DE */
                     "movl $0x66, %0"
                     : "+r"(ret) : : "eax", "ecx", "edx", "cc");
    return ret;
}

static int read_via_rax(volatile int *p)
{
    int v;
    __asm__ volatile("movq %1, %%rax\n\t"
                     "movl (%%rax), %%ecx\n\t"
                     "movl %%ecx, %0"
                     : "=r"(v) : "r"(p) : "rax", "rcx", "memory");
    return v;
}

#ifdef __clang__
static int g_finally_ran;
/* clang drops a __try whose body cannot "throw" (a plain store); an opaque call is an invoke, so it stays */
static __attribute__((noinline)) void poke(volatile int *p) { *p = 1; }
static int seh_try_except_av(void)
{
    int caught = 0;
    __try { poke((volatile int *)(uintptr_t)0x10); }       /* low unmapped page */
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
    { caught = 1; }
    return caught;
}
static ULONG_PTR g_filter_info1;
static int seh_filter(EXCEPTION_POINTERS *ep)
{
    g_filter_info1 = ep->ExceptionRecord->ExceptionInformation[1];
    return EXCEPTION_EXECUTE_HANDLER;
}
static int seh_try_except_raise(ULONG_PTR *got)
{
    int caught = 0;
    __try { ULONG_PTR a[2] = { 11, 22 }; RaiseException(0xE0534D02, 0, 2, a); }
    __except (seh_filter(GetExceptionInformation())) { *got = g_filter_info1; caught = 1; }
    return caught;
}
static int seh_finally_unwind(void)
{
    int caught = 0;
    g_finally_ran = 0;
    __try {
        __try { RaiseException(0xE0534D03, EXCEPTION_NONCONTINUABLE, 0, NULL); }
        __finally { g_finally_ran = 1; }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { caught = 1; }
    return caught;
}
static int nested_depth(int n)
{
    if (n == 0) { RaiseException(0xE0534D04, 0, 0, NULL); return -1; }
    {
        int r = -2;
        __try { r = nested_depth(n - 1); }
        __finally { g_finally_ran++; }
        return r;
    }
}
static int seh_unwind_through_frames(void)
{
    int caught = 0;
    g_finally_ran = 0;
    __try { nested_depth(8); }
    __except (EXCEPTION_EXECUTE_HANDLER) { caught = 1; }
    return caught;
}
#endif

static void section_seh(void)
{
    PVOID h = AddVectoredExceptionHandler(1, veh);
    ULONG_PTR args[2] = { 0x1111, 0x2222 };
    volatile int *p;
    int v;

    OK(h != NULL, "seh.add_vectored_handler");

    /* 1. software exception raised with RaiseException, resumed by the vectored handler */
    veh_arm(0xE0534D01, VEH_PLAIN_RESUME);
    RaiseException(0xE0534D01, 0, 2, args);
    EQ(g_veh.hits, 1, "seh.raiseexception_caught_by_vectored_handler");
    EQ(g_veh.info[0], 0x1111, "seh.raiseexception_param0");
    EQ(g_veh.info[1], 0x2222, "seh.raiseexception_param1");
    EQ(g_veh.nparams, 2, "seh.raiseexception_nparams");
    OK(g_veh.tid == GetCurrentThreadId(), "seh.handler_runs_on_faulting_thread");

    /* 2. access violation: read of reserved-but-uncommitted page; handler commits it and resumes */
    p = VirtualAlloc(NULL, 0x1000, MEM_RESERVE, PAGE_NOACCESS);
    OK(p != NULL, "seh.av_read.reserve");
    veh_arm(EXCEPTION_ACCESS_VIOLATION, VEH_COMMIT);
    v = *p;                                   /* faults, handler commits, instruction re-executes */
    EQ(g_veh.hits, 1, "seh.av_read.caught_once");
    EQ(g_veh.info[0], 0, "seh.av_read.is_read_access");
    EQ(g_veh.info[1], (ULONG_PTR)p, "seh.av_read.fault_address");
    OK(g_veh.exc_addr != NULL, "seh.av_read.exception_address_set");
    EQ(v, 0, "seh.av_read.resumed_and_read_zero");
    VirtualFree((void *)p, 0, MEM_RELEASE);

    /* 3. access violation on a page that was released (unmapped): context edit + resume */
    p = VirtualAlloc(NULL, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    VirtualFree((void *)p, 0, MEM_RELEASE);
    veh_arm(EXCEPTION_ACCESS_VIOLATION, VEH_FIXUP_RAX);
    v = read_via_rax(p);                      /* handler rewrites RAX; faulting load re-executes */
    EQ(g_veh.hits, 1, "seh.av_unmapped.caught_once");
    EQ(g_veh.info[1], (ULONG_PTR)p, "seh.av_unmapped.fault_address");
    EQ(v, 0x1234abcd, "seh.av_unmapped.context_modification_honoured");

    /* 4. write to a read-only page */
    p = VirtualAlloc(NULL, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_READONLY);
    veh_arm(EXCEPTION_ACCESS_VIOLATION, VEH_PROTECT_RW);
    *p = 0x5a5a5a5a;
    EQ(g_veh.hits, 1, "seh.av_write_readonly.caught_once");
    EQ(g_veh.info[0], 1, "seh.av_write_readonly.is_write_access");
    EQ(*p, 0x5a5a5a5a, "seh.av_write_readonly.store_completed_after_resume");
    VirtualFree((void *)p, 0, MEM_RELEASE);

    /* 5. division by zero, instruction skipped by the handler */
    veh_arm(EXCEPTION_INT_DIVIDE_BY_ZERO, VEH_SKIP_DIV);
    v = div_by_zero_via_asm();
    EQ(g_veh.hits, 1, "seh.divide_by_zero.caught");
    EQ(v, 0x66, "seh.divide_by_zero.resumed_after_instruction");

    /* 6. int3 */
    veh_arm(EXCEPTION_BREAKPOINT, VEH_SKIP_INT3);
    __asm__ volatile("int3");
    EQ(g_veh.hits, 1, "seh.int3.caught_and_resumed_after_rip_adjust");

    veh_disarm();

#ifdef __clang__
    {
        ULONG_PTR got = 0;
        OK(seh_try_except_av(), "seh.try_except.access_violation");
        OK(seh_try_except_raise(&got) && got == 22, "seh.try_except.raiseexception_info");
        OK(seh_finally_unwind() && g_finally_ran, "seh.try_finally.runs_during_unwind");
        OK(seh_unwind_through_frames() && g_finally_ran == 8, "seh.unwind_through_8_frames");
    }
#else
    info("frame-based SEH checks skipped (compiler is not clang)");
#endif

    RemoveVectoredExceptionHandler(h);
}

/* ------------------------------------------------------------------------------------------ */
/* section: timers                                                                              */
/* ------------------------------------------------------------------------------------------ */

static void section_timers(void)
{
    LARGE_INTEGER f, a, b, prev;
    int i, mono = 1;
    DWORD64 t0, t1;
    HANDLE t;
    LARGE_INTEGER due;
    DWORD w;
    double ms;

    OK(QueryPerformanceFrequency(&f), "timers.qpf_call");
    check(f.QuadPart >= 1000000 && f.QuadPart <= 10000000000LL, "timers.qpf_frequency_sane", "frequency=%lld", f.QuadPart);

    QueryPerformanceCounter(&prev);
    for (i = 0; i < 100000; i++) {
        QueryPerformanceCounter(&a);
        if (a.QuadPart < prev.QuadPart) { mono = 0; break; }
        prev = a;
    }
    OK(mono, "timers.qpc_monotonic_100000_reads");

    QueryPerformanceCounter(&a);
    Sleep(100);
    QueryPerformanceCounter(&b);
    ms = (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart;
    check(ms >= 90.0 && ms < 3000.0, "timers.qpc_elapsed_over_sleep_100ms", "measured %.2f ms", ms);

    t0 = GetTickCount64();
    Sleep(60);
    t1 = GetTickCount64();
    check(t1 > t0 && t1 - t0 < 3000, "timers.gettickcount64_advances", "delta %llu ms", (unsigned long long)(t1 - t0));

    /* waitable timer */
    t = CreateWaitableTimerA(NULL, TRUE, NULL);
    OK(t != NULL, "timers.create_waitable_timer");
    due.QuadPart = -1500000LL;                /* 150 ms, relative */
    QueryPerformanceCounter(&a);
    OK(SetWaitableTimer(t, &due, 0, NULL, NULL, FALSE), "timers.set_waitable_timer");
    EQ(WaitForSingleObject(t, 0), WAIT_TIMEOUT, "timers.waitable_not_yet_signalled");
    w = WaitForSingleObject(t, 5000);
    QueryPerformanceCounter(&b);
    EQ(w, WAIT_OBJECT_0, "timers.waitable_timer_fires");
    ms = (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart;
    check(ms >= 120.0 && ms < 3000.0, "timers.waitable_timer_delay", "measured %.2f ms for a 150 ms timer", ms);
    CloseHandle(t);

    /* periodic timer: auto-reset, fires at least 3 times in ~500 ms */
    t = CreateWaitableTimerA(NULL, FALSE, NULL);
    due.QuadPart = -500000LL;                 /* 50 ms */
    SetWaitableTimer(t, &due, 50, NULL, NULL, FALSE);
    for (i = 0; i < 3; i++) if (WaitForSingleObject(t, 2000) != WAIT_OBJECT_0) break;
    EQ(i, 3, "timers.periodic_timer_fires_3_times");
    CancelWaitableTimer(t);
    CloseHandle(t);

    {   /* precise system time */
        FILETIME ft1, ft2;
        GetSystemTimePreciseAsFileTime(&ft1);
        Sleep(20);
        GetSystemTimePreciseAsFileTime(&ft2);
        OK(CompareFileTime(&ft2, &ft1) > 0, "timers.system_time_precise_advances");
    }
}

/* ------------------------------------------------------------------------------------------ */
/* section: tls - TlsAlloc, __thread, __declspec(thread), TLS callbacks, FLS                    */
/* ------------------------------------------------------------------------------------------ */

static __thread int t_gnu_int = 7;
static __thread char t_gnu_bss[512];
#ifdef __clang__
static __declspec(thread) int t_decl_int = 11;     /* MSVC spelling, as the game's own TLS is compiled */
#else
static __thread int t_decl_int = 11;               /* gcc ignores __declspec(thread) */
#endif

static DWORD g_tls_idx;
static volatile LONG g_tls_cb_attach_threads, g_tls_cb_detach_threads, g_tls_cb_process_attach;

static void NTAPI tls_callback(PVOID hmod, DWORD reason, PVOID reserved)
{
    (void)hmod; (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) InterlockedIncrement(&g_tls_cb_process_attach);
    if (reason == DLL_THREAD_ATTACH)  InterlockedIncrement(&g_tls_cb_attach_threads);
    if (reason == DLL_THREAD_DETACH)  InterlockedIncrement(&g_tls_cb_detach_threads);
}
#ifdef _MSC_VER
#pragma section(".CRT$XLB", read)
__declspec(allocate(".CRT$XLB")) PIMAGE_TLS_CALLBACK p_tls_callback = tls_callback;
#else
__attribute__((section(".CRT$XLB"), used)) PIMAGE_TLS_CALLBACK p_tls_callback = tls_callback;
#endif

static DWORD WINAPI tls_thread(LPVOID arg)
{
    int id = (int)(intptr_t)arg, i, ok = 1;

    if (t_gnu_int != 7) ok = 0;                                  /* initialised TLS template */
    if (t_decl_int != 11) ok = 0;
    for (i = 0; i < (int)sizeof(t_gnu_bss); i++) if (t_gnu_bss[i]) ok = 0;   /* zero-filled tail */
    if (TlsGetValue(g_tls_idx) != NULL) ok = 0;                  /* fresh slot per thread */

    t_gnu_int = 1000 + id;
    t_decl_int = 2000 + id;
    memset(t_gnu_bss, id + 1, sizeof(t_gnu_bss));
    TlsSetValue(g_tls_idx, (LPVOID)(intptr_t)(0x100 + id));
    FlsSetValue(FlsAlloc(NULL), (PVOID)(intptr_t)id);

    for (i = 0; i < 20; i++) {                                   /* let the other threads run and write theirs */
        Sleep(1);
        SwitchToThread();
        if (t_gnu_int != 1000 + id || t_decl_int != 2000 + id) ok = 0;
        if (t_gnu_bss[0] != id + 1 || t_gnu_bss[sizeof(t_gnu_bss) - 1] != id + 1) ok = 0;
        if ((intptr_t)TlsGetValue(g_tls_idx) != 0x100 + id) ok = 0;
    }
    return ok ? 0 : 1;
}

static void section_tls(void)
{
    HANDLE th[4];
    DWORD code, i, all_ok = 1;
    DWORD fls;
    LONG attach_before = g_tls_cb_attach_threads;

    g_tls_idx = TlsAlloc();
    OK(g_tls_idx != TLS_OUT_OF_INDEXES, "tls.tlsalloc");
    OK(TlsSetValue(g_tls_idx, (LPVOID)0x77), "tls.main_set");
    t_gnu_int = 8;                                               /* main thread changes its own copy */

    for (i = 0; i < 4; i++) th[i] = CreateThread(NULL, 0, tls_thread, (LPVOID)(intptr_t)i, 0, NULL);
    EQ(WaitForMultipleObjects(4, th, TRUE, 30000), WAIT_OBJECT_0, "tls.join_4_threads");
    for (i = 0; i < 4; i++) {
        code = 1; GetExitCodeThread(th[i], &code);
        if (code) all_ok = 0;
        CloseHandle(th[i]);
    }
    OK(all_ok, "tls.threads_see_own_values_and_templates");
    EQ(t_gnu_int, 8, "tls.main_thread_gnu_thread_unchanged_by_others");
    EQ(t_decl_int, 11, "tls.main_thread_declspec_thread_unchanged");
    EQ((intptr_t)TlsGetValue(g_tls_idx), 0x77, "tls.main_tlsgetvalue_unchanged");
    OK(TlsFree(g_tls_idx), "tls.tlsfree");

    /* TLS callbacks (.CRT$XLB) are called for process attach and for each new thread */
    OK(g_tls_cb_process_attach >= 1, "tls.callback_process_attach");
    EQ(g_tls_cb_attach_threads - attach_before, 4, "tls.callback_thread_attach_x4");
    OK(g_tls_cb_detach_threads >= 4, "tls.callback_thread_detach");

    /* FLS on the main thread */
    fls = FlsAlloc(NULL);
    OK(fls != FLS_OUT_OF_INDEXES, "tls.flsalloc");
    FlsSetValue(fls, (PVOID)0x1234);
    EQ((intptr_t)FlsGetValue(fls), 0x1234, "tls.fls_roundtrip");
    FlsFree(fls);
}

/* ------------------------------------------------------------------------------------------ */
/* section: virtual - VirtualAlloc / VirtualProtect / VirtualQuery at 4 KB granularity           */
/* ------------------------------------------------------------------------------------------ */

static int vq(void *addr, MEMORY_BASIC_INFORMATION *mbi)
{
    memset(mbi, 0xcc, sizeof(*mbi));
    return VirtualQuery(addr, mbi, sizeof(*mbi)) == sizeof(*mbi);
}

static void expect_region(char *base, size_t off, size_t size, DWORD prot, DWORD state, const char *tag)
{
    MEMORY_BASIC_INFORMATION m;
    char nm[96];
    int got = vq(base + off, &m);

    snprintf(nm, sizeof(nm), "virtual.query.%s", tag);
    check(got && m.BaseAddress == base + off && m.RegionSize == size && m.State == state &&
          (state != MEM_COMMIT || m.Protect == prot) && m.AllocationBase == base,
          nm, "off=0x%zx: base=+0x%tx size=0x%zx state=0x%lx prot=0x%lx allocbase=%s; expected base=+0x%zx size=0x%zx state=0x%lx prot=0x%lx",
          off, (char *)m.BaseAddress - base, (size_t)m.RegionSize, m.State, m.Protect,
          m.AllocationBase == base ? "ok" : "WRONG", off, size, state, prot);
}

static void section_virtual(void)
{
    SYSTEM_INFO si;
    MEMORY_BASIC_INFORMATION m;
    char *base;
    DWORD old = 0xdead;
    volatile char *vp;
    PVOID h;

    GetSystemInfo(&si);
    base = VirtualAlloc(NULL, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    OK(base != NULL, "virtual.alloc_64k");
    if (!base) return;
    EQ((uintptr_t)base & 0xffff, 0, "virtual.alloc_is_64k_aligned");

    OK(vq(base, &m), "virtual.query.initial_call");
    check(m.BaseAddress == base && m.AllocationBase == base && m.RegionSize == 0x10000 && m.State == MEM_COMMIT &&
          m.Protect == PAGE_READWRITE && m.AllocationProtect == PAGE_READWRITE && m.Type == MEM_PRIVATE,
          "virtual.query.initial_region", "base=%p allocbase=%p size=0x%zx state=0x%lx prot=0x%lx allocprot=0x%lx type=0x%lx",
          m.BaseAddress, m.AllocationBase, (size_t)m.RegionSize, m.State, m.Protect, m.AllocationProtect, m.Type);

    /* page 5 -> READONLY, page 10 -> NOACCESS (both in the middle of the 64 KB block) */
    old = 0xdead;
    OK(VirtualProtect(base + 0x5000, 0x1000, PAGE_READONLY, &old), "virtual.protect_page5_readonly");
    EQ(old, PAGE_READWRITE, "virtual.protect_page5_old_protection");
    old = 0xdead;
    OK(VirtualProtect(base + 0xA000, 0x1000, PAGE_NOACCESS, &old), "virtual.protect_page10_noaccess");
    EQ(old, PAGE_READWRITE, "virtual.protect_page10_old_protection");

    expect_region(base, 0x0000, 0x5000,  PAGE_READWRITE, MEM_COMMIT, "pages0to4_rw");
    expect_region(base, 0x5000, 0x1000,  PAGE_READONLY,  MEM_COMMIT, "page5_readonly");
    expect_region(base, 0x6000, 0x4000,  PAGE_READWRITE, MEM_COMMIT, "pages6to9_rw");
    expect_region(base, 0xA000, 0x1000,  PAGE_NOACCESS,  MEM_COMMIT, "page10_noaccess");
    expect_region(base, 0xB000, 0x5000,  PAGE_READWRITE, MEM_COMMIT, "pages11to15_rw");

    /* a query inside a page reports the page-aligned region start */
    check(vq(base + 0x5800, &m) && m.BaseAddress == base + 0x5000 && m.RegionSize == 0x1000 && m.Protect == PAGE_READONLY,
          "virtual.query.mid_page_address_rounds_down", "base=+0x%tx size=0x%zx prot=0x%lx",
          (char *)m.BaseAddress - base, (size_t)m.RegionSize, m.Protect);

    /* old-protection return values */
    old = 0xdead;
    OK(VirtualProtect(base + 0x5000, 0x1000, PAGE_READWRITE, &old), "virtual.protect_page5_back_to_rw");
    EQ(old, PAGE_READONLY, "virtual.protect_page5_back_old_protection");
    OK(VirtualProtect(base + 0x5000, 0x1000, PAGE_READONLY, &old), "virtual.protect_page5_readonly_again");

    old = 0xdead;      /* range spanning pages with different protections: old = protection of the FIRST page */
    OK(VirtualProtect(base + 0x4000, 0x3000, PAGE_READWRITE, &old), "virtual.protect_span_pages4to6");
    EQ(old, PAGE_READWRITE, "virtual.protect_span_old_is_first_page");
    expect_region(base, 0x0000, 0xA000,  PAGE_READWRITE, MEM_COMMIT, "span_merged_rw_after_reprotect");
    OK(VirtualProtect(base + 0x5000, 0x1000, PAGE_READONLY, &old), "virtual.protect_page5_readonly_third");
    EQ(old, PAGE_READWRITE, "virtual.protect_page5_third_old_protection");

    /* faulting accesses match the protections */
    vp = base + 0x5000;
    h = AddVectoredExceptionHandler(1, veh);
    *(volatile char *)(base + 0x4fff) = 0x41;               /* last byte of page 4: writable */
    *(volatile char *)(base + 0x6000) = 0x42;               /* first byte of page 6: writable */
    EQ(*(volatile char *)(base + 0x4fff), 0x41, "virtual.access.write_before_readonly_page_ok");
    EQ(*(volatile char *)(base + 0x6000), 0x42, "virtual.access.write_after_readonly_page_ok");
    EQ(*vp, 0, "virtual.access.read_readonly_page_ok");

    veh_arm(EXCEPTION_ACCESS_VIOLATION, VEH_PROTECT_RW);
    *vp = 0x43;
    EQ(g_veh.hits, 1, "virtual.access.write_to_readonly_page_faults");
    EQ(g_veh.info[0], 1, "virtual.access.readonly_fault_is_write");
    EQ(g_veh.info[1], (ULONG_PTR)(base + 0x5000), "virtual.access.readonly_fault_address");

    veh_arm(EXCEPTION_ACCESS_VIOLATION, VEH_PROTECT_RW);
    { volatile char c = *(volatile char *)(base + 0xA123); (void)c; }
    EQ(g_veh.hits, 1, "virtual.access.read_noaccess_page_faults");
    EQ(g_veh.info[0], 0, "virtual.access.noaccess_fault_is_read");
    EQ(g_veh.info[1], (ULONG_PTR)(base + 0xA123), "virtual.access.noaccess_fault_address_exact");
    veh_disarm();

    /* PAGE_GUARD is one-shot and reports STATUS_GUARD_PAGE_VIOLATION at 4 KB granularity */
    OK(VirtualProtect(base + 0xC000, 0x1000, PAGE_READWRITE | PAGE_GUARD, &old), "virtual.guard.set");
    expect_region(base, 0xC000, 0x1000, PAGE_READWRITE | PAGE_GUARD, MEM_COMMIT, "page12_guard");
    veh_arm(STATUS_GUARD_PAGE_VIOLATION, VEH_PLAIN_RESUME);
    *(volatile char *)(base + 0xC010) = 0x44;
    EQ(g_veh.hits, 1, "virtual.guard.violation_raised");
    EQ(*(volatile char *)(base + 0xC010), 0x44, "virtual.guard.access_completes_after_resume");
    check(vq(base + 0xC000, &m) && !(m.Protect & PAGE_GUARD), "virtual.guard.cleared_after_first_access", "prot=0x%lx", m.Protect);
    veh_disarm();
    RemoveVectoredExceptionHandler(h);

    /* decommit one 4 KB page in the middle */
    OK(VirtualFree(base + 0xD000, 0x1000, MEM_DECOMMIT), "virtual.decommit_page13");
    expect_region(base, 0xD000, 0x1000, 0, MEM_RESERVE, "page13_reserved_only");
    OK(vq(base + 0xE000, &m) && m.State == MEM_COMMIT, "virtual.neighbour_page_still_committed");
    OK(VirtualAlloc(base + 0xD000, 0x1000, MEM_COMMIT, PAGE_EXECUTE_READ) != NULL, "virtual.recommit_page13_exec_read");
    expect_region(base, 0xD000, 0x1000, PAGE_EXECUTE_READ, MEM_COMMIT, "page13_exec_read");
    OK(VirtualProtect(base + 0xD000, 0x1000, PAGE_READWRITE, &old), "virtual.page13_back_to_rw");
    EQ(old, PAGE_EXECUTE_READ, "virtual.page13_old_protection_exec_read");

    OK(VirtualFree(base, 0, MEM_RELEASE), "virtual.release");
    check(vq(base, &m) && m.State == MEM_FREE, "virtual.query.free_after_release", "state=0x%lx", m.State);
}

/* ------------------------------------------------------------------------------------------ */
/* section: fibers (risk R-005)                                                                 */
/* ------------------------------------------------------------------------------------------ */

#define FIBER_ROUNDS 1000

struct fctx {
    LPVOID main_fiber, fiber;
    volatile int steps;
    volatile int order_ok;
    uintptr_t stack_addr_in_fiber;
    uintptr_t fiber_limit_lo, fiber_limit_hi;
    int data_ok, current_ok, fls_ok, tls_ok, exc_ok;
    DWORD fls_idx;
};

static VOID CALLBACK fiber_proc(LPVOID param)
{
    struct fctx *c = param;
    ULONG_PTR lo, hi;
    volatile char local_marker = 1;
    int last = 0;

    GetCurrentThreadStackLimits(&lo, &hi);
    c->stack_addr_in_fiber = (uintptr_t)&local_marker;
    c->fiber_limit_lo = lo; c->fiber_limit_hi = hi;
    c->data_ok = (GetFiberData() == (PVOID)c);
    c->current_ok = (GetCurrentFiber() == c->fiber);
    FlsSetValue(c->fls_idx, (PVOID)0x2222);
    c->fls_ok = ((intptr_t)FlsGetValue(c->fls_idx) == 0x2222);
    c->tls_ok = (t_gnu_int == 8);                    /* __thread is per thread, shared by the thread's fibers */

    for (;;) {                                       /* never returns: returning would end the thread */
        if (c->steps != last) c->order_ok = 0;       /* main must not have touched steps while we were away */
        c->steps = ++last;
        if (last == 5) {                             /* exception raised while running on the fiber stack */
            veh_arm(EXCEPTION_INT_DIVIDE_BY_ZERO, VEH_SKIP_DIV);
            c->exc_ok = (div_by_zero_via_asm() == 0x66 && g_veh.hits == 1);
            veh_disarm();
        }
        SwitchToFiber(c->main_fiber);
    }
}

static void run_fiber_test(const char *tag, int on_worker)
{
    struct fctx c;
    char nm[96];
    ULONG_PTR mlo, mhi;
    int i, ok = 1;
    PVOID veh_h = AddVectoredExceptionHandler(1, veh);

    memset(&c, 0, sizeof(c));
    c.order_ok = 1;
    c.fls_idx = FlsAlloc(NULL);
    FlsSetValue(c.fls_idx, (PVOID)0x1111);
    t_gnu_int = 8;

    c.main_fiber = ConvertThreadToFiberEx((PVOID)0xabc, 0);
    snprintf(nm, sizeof(nm), "fibers.%s.convert_thread_to_fiber", tag);
    OK(c.main_fiber != NULL, nm);
    GetCurrentThreadStackLimits(&mlo, &mhi);

    c.fiber = CreateFiberEx(0x10000, 0x40000, 0, fiber_proc, &c);
    snprintf(nm, sizeof(nm), "fibers.%s.create_fiber_ex", tag);
    OK(c.fiber != NULL, nm);
    if (!c.fiber) { RemoveVectoredExceptionHandler(veh_h); return; }

    for (i = 1; i <= FIBER_ROUNDS; i++) {
        SwitchToFiber(c.fiber);
        if (c.steps != i) { ok = 0; break; }
    }
    snprintf(nm, sizeof(nm), "fibers.%s.roundtrip_%d_switches", tag, FIBER_ROUNDS);
    check(ok && i == FIBER_ROUNDS + 1, nm, "stopped at round %d, steps=%d", i, c.steps);
    snprintf(nm, sizeof(nm), "fibers.%s.alternation_order", tag);
    OK(c.order_ok, nm);

    snprintf(nm, sizeof(nm), "fibers.%s.fiber_runs_on_its_own_stack", tag);
    check(c.stack_addr_in_fiber && (c.stack_addr_in_fiber < mlo || c.stack_addr_in_fiber >= mhi),
          nm, "fiber local at %p, main stack [%p,%p)", (void *)c.stack_addr_in_fiber, (void *)mlo, (void *)mhi);
    snprintf(nm, sizeof(nm), "fibers.%s.stack_limits_follow_fiber", tag);
    check(c.stack_addr_in_fiber >= c.fiber_limit_lo && c.stack_addr_in_fiber < c.fiber_limit_hi,
          nm, "fiber local at %p, reported limits [%p,%p)", (void *)c.stack_addr_in_fiber, (void *)c.fiber_limit_lo, (void *)c.fiber_limit_hi);
    snprintf(nm, sizeof(nm), "fibers.%s.getfiberdata", tag);
    OK(c.data_ok, nm);
    snprintf(nm, sizeof(nm), "fibers.%s.getcurrentfiber", tag);
    OK(c.current_ok, nm);
    snprintf(nm, sizeof(nm), "fibers.%s.main_fiber_data", tag);
    EQ((intptr_t)GetFiberData(), 0xabc, nm);
    snprintf(nm, sizeof(nm), "fibers.%s.fls_is_per_fiber", tag);
    check(c.fls_ok && (intptr_t)FlsGetValue(c.fls_idx) == 0x1111, nm, "fiber_ok=%d main_value=%p", c.fls_ok, FlsGetValue(c.fls_idx));
    snprintf(nm, sizeof(nm), "fibers.%s.tls_is_per_thread", tag);
    OK(c.tls_ok, nm);
    snprintf(nm, sizeof(nm), "fibers.%s.exception_on_fiber_stack", tag);
    OK(c.exc_ok, nm);

    DeleteFiber(c.fiber);
    snprintf(nm, sizeof(nm), "fibers.%s.convert_fiber_to_thread", tag);
    OK(ConvertFiberToThread(), nm);
    FlsFree(c.fls_idx);
    RemoveVectoredExceptionHandler(veh_h);
    (void)on_worker;
}

static DWORD WINAPI fiber_worker(LPVOID arg) { (void)arg; run_fiber_test("worker_thread", 1); return 0; }

static void section_fibers(void)
{
    HANDLE th;
    run_fiber_test("main_thread", 0);
    th = CreateThread(NULL, 0, fiber_worker, NULL, 0, NULL);
    EQ(WaitForSingleObject(th, 60000), WAIT_OBJECT_0, "fibers.worker_thread.finished");
    CloseHandle(th);
}

/* ------------------------------------------------------------------------------------------ */
/* section: threadname - MSVC thread-naming exception and SetThreadDescription                  */
/* ------------------------------------------------------------------------------------------ */

#pragma pack(push, 8)
typedef struct {
    DWORD dwType;      /* must be 0x1000 */
    LPCSTR szName;
    DWORD dwThreadID;  /* -1 = caller thread */
    DWORD dwFlags;
} THREADNAME_INFO;
#pragma pack(pop)

typedef HRESULT (WINAPI *SetThreadDescription_t)(HANDLE, PCWSTR);
typedef HRESULT (WINAPI *GetThreadDescription_t)(HANDLE, PWSTR *);

static void msvc_set_thread_name(const char *name, DWORD tid)
{
    THREADNAME_INFO ti;
    memset(&ti, 0, sizeof(ti));                      /* padding must be zero: Wine compares the full ULONG_PTR */
    ti.dwType = 0x1000; ti.szName = name; ti.dwThreadID = tid; ti.dwFlags = 0;
    RaiseException(0x406D1388, 0, sizeof(ti) / sizeof(ULONG_PTR), (const ULONG_PTR *)&ti);
}

#ifdef __clang__
static int msvc_set_thread_name_seh(const char *name)
{
    int survived = 0;
    __try { msvc_set_thread_name(name, (DWORD)-1); survived = 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { survived = 2; }   /* what MSVC code does: swallow it */
    return survived;
}
#endif

static DWORD WINAPI named_thread(LPVOID arg)
{
    (void)arg;
    msvc_set_thread_name("RenderThread", (DWORD)-1);
    return 0;
}

static void section_threadname(void)
{
    HMODULE k32 = GetModuleHandleA("kernel32.dll"), kb = GetModuleHandleA("kernelbase.dll");
    SetThreadDescription_t pSet = (SetThreadDescription_t)GetProcAddress(k32, "SetThreadDescription");
    GetThreadDescription_t pGet = (GetThreadDescription_t)GetProcAddress(k32, "GetThreadDescription");
    PVOID h;
    PWSTR desc = NULL;
    HRESULT hr;
    HANDLE th;

    if (!pSet && kb) pSet = (SetThreadDescription_t)GetProcAddress(kb, "SetThreadDescription");
    if (!pGet && kb) pGet = (GetThreadDescription_t)GetProcAddress(kb, "GetThreadDescription");

    h = AddVectoredExceptionHandler(1, veh);

    /* the MSVC idiom: RaiseException(0x406D1388) with THREADNAME_INFO; the vectored handler resumes */
    veh_arm(0x406D1388, VEH_PLAIN_RESUME);
    msvc_set_thread_name("GameThread", (DWORD)-1);
    EQ(g_veh.hits, 1, "threadname.exception_0x406D1388_seen");
    EQ(g_veh.info[0], 0x1000, "threadname.info_dwType");
    check(!strcmp(g_veh.name_copy, "GameThread"), "threadname.info_name_pointer_valid", "name='%s'", g_veh.name_copy);
    EQ((DWORD)g_veh.info[2], (DWORD)-1, "threadname.info_thread_id");
    EQ(g_veh.nparams, sizeof(THREADNAME_INFO) / sizeof(ULONG_PTR), "threadname.info_param_count");

    /* naming a thread from another thread */
    th = CreateThread(NULL, 0, named_thread, NULL, 0, NULL);
    WaitForSingleObject(th, 10000);
    CloseHandle(th);
    OK(g_veh.hits >= 2, "threadname.exception_from_worker_thread");
    veh_disarm();

#ifdef __clang__
    /* with a frame-based handler, as MSVC-compiled code does it (exception not seen by any VEH) */
    EQ(msvc_set_thread_name_seh("SehThread"), 2, "threadname.caught_by_frame_seh_handler");
#endif
    RemoveVectoredExceptionHandler(h);

    OK(pSet && pGet, "threadname.description_apis_present");
    if (pSet && pGet) {
        hr = pSet(GetCurrentThread(), L"RLPort.Smoke.Main");
        check(SUCCEEDED(hr), "threadname.SetThreadDescription", "hr=0x%lx", (unsigned long)hr);
        hr = pGet(GetCurrentThread(), &desc);
        check(SUCCEEDED(hr), "threadname.GetThreadDescription", "hr=0x%lx", (unsigned long)hr);
        check(desc && !wcscmp(desc, L"RLPort.Smoke.Main"), "threadname.description_roundtrip", "got '%ls'", desc ? desc : L"(null)");
        if (desc) LocalFree(desc);
    }
}

/* ------------------------------------------------------------------------------------------ */

struct section { const char *name; void (*fn)(void); };
static const struct section sections[] = {
    { "env",        section_env },
    { "threads",    section_threads },
    { "fileio",     section_fileio },
    { "seh",        section_seh },
    { "timers",     section_timers },
    { "tls",        section_tls },
    { "virtual",    section_virtual },
    { "fibers",     section_fibers },
    { "threadname", section_threadname },
};

int main(int argc, char **argv)
{
    size_t i, j;
    int ran_any = 0;

    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc == 2 && !strcmp(argv[1], "--list")) {
        for (i = 0; i < sizeof(sections) / sizeof(sections[0]); i++) puts(sections[i].name);
        return 0;
    }
    info("smoke.exe built %s %s", __DATE__, __TIME__);
    for (i = 0; i < sizeof(sections) / sizeof(sections[0]); i++) {
        int selected = (argc == 1);
        for (j = 1; j < (size_t)argc; j++) if (!strcmp(argv[j], sections[i].name)) selected = 1;
        if (!selected) continue;
        printf("SECTION %s\n", sections[i].name);
        sections[i].fn();
        ran_any = 1;
    }
    if (!ran_any) { printf("no such section\n"); return 255; }
    printf("SUMMARY checks=%d failures=%d\n", g_checks, g_failures);
    fflush(stdout);
    return g_failures > 255 ? 255 : g_failures;
}
