// Tests for runtime/jit on Linux: startup protocol order with a fake debugger, W^X of the
// split mapping, executing generated code through the RX view, and the region allocator.
#define _GNU_SOURCE
#include "../../runtime/jit/jit.h"

#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

int jit_linux_map_split(size_t len, void** rw, void** rx, int* fd_out);

static int fails;
#define CHECK(c, ...)                                     \
  do {                                                    \
    if (!(c)) {                                           \
      fails++;                                            \
      printf("FAIL %s:%d: ", __FILE__, __LINE__);         \
      printf(__VA_ARGS__);                                \
      printf("\n");                                       \
    }                                                     \
  } while (0)

// ---- fake debugger backend -------------------------------------------------------------
typedef struct {
  int debugged;
  jit_txm txm;
  int prepare_returns_null;
  char calls[256];  // call order, one letter per call
  int fd;
  void* rw;
  void* rx;
} fake;

static void rec(fake* f, char c) {
  size_t n = strlen(f->calls);
  f->calls[n] = c;
  f->calls[n + 1] = 0;
}
static int f_dbg(void* c) { rec(c, 'D'); return ((fake*)c)->debugged; }
static jit_txm f_txm(void* c) { rec(c, 'T'); return ((fake*)c)->txm; }
static void* f_prepare(void* c, void* addr, size_t len) {
  fake* f = c;
  rec(f, 'P');
  if (addr || f->prepare_returns_null) return NULL;
  jit_linux_map_split(len, &f->rw, &f->rx, &f->fd);  // the debugger allocates the RX region
  return f->rx;
}
static void* f_alias(void* c, void* rx, size_t len) {
  fake* f = c;
  rec(f, 'A');
  (void)len;
  return rx == f->rx ? f->rw : NULL;
}
static void f_detach(void* c) { rec(c, 'X'); }
static int f_split(void* c, size_t len, void** rw, void** rx) { rec(c, 'S'); return jit_linux_map_split(len, rw, rx, NULL); }
static void f_log(void* c, const char* m) { (void)c; (void)m; }

static jit_backend backend_for(fake* f) {
  jit_backend b = {f, f_dbg, f_txm, f_prepare, f_detach, f_alias, f_split, f_log};
  return b;
}

static void test_protocol(void) {
  char why[256];
  fake f = {.debugged = 0, .txm = JIT_TXM_PRESENT};
  jit_backend b = backend_for(&f);
  jit_reset_for_tests();
  CHECK(jit_init(&b, 1 << 20, -1, why, sizeof why) == JIT_STATUS_NOT_DEBUGGED, "not debugged");
  CHECK(strcmp(f.calls, "D") == 0, "no breakpoint call before CS_DEBUGGED, got %s", f.calls);
  CHECK(strstr(why, "StikDebug") != NULL, "user-facing reason mentions StikDebug: %s", why);

  f = (fake){.debugged = 1, .txm = JIT_TXM_UNKNOWN};
  b = backend_for(&f);
  jit_reset_for_tests();
  CHECK(jit_init(&b, 1 << 20, -1, why, sizeof why) == JIT_STATUS_TXM_UNKNOWN, "unknown TXM refused");
  CHECK(strcmp(f.calls, "DT") == 0, "no breakpoint call when TXM unknown, got %s", f.calls);

  f = (fake){.debugged = 1, .txm = JIT_TXM_PRESENT};
  b = backend_for(&f);
  jit_reset_for_tests();
  CHECK(jit_init(&b, 1 << 20, -1, why, sizeof why) == JIT_STATUS_READY, "TXM ready: %s", why);
  CHECK(strcmp(f.calls, "DTPAX") == 0, "TXM order debugged,txm,prepare,alias,detach; got %s", f.calls);
  CHECK(jit_rw_minus_rx() == (char*)f.rw - (char*)f.rx, "rw-rx offset");

  f = (fake){.debugged = 1, .txm = JIT_TXM_PRESENT, .prepare_returns_null = 1};
  b = backend_for(&f);
  jit_reset_for_tests();
  CHECK(jit_init(&b, 1 << 20, -1, why, sizeof why) == JIT_STATUS_PREPARE_FAILED, "prepare failure reported");
  CHECK(strcmp(f.calls, "DTPX") == 0, "still detaches after a failed prepare; got %s", f.calls);

  f = (fake){.debugged = 1, .txm = JIT_TXM_UNKNOWN};
  b = backend_for(&f);
  jit_reset_for_tests();
  CHECK(jit_init(&b, 1 << 20, 0, why, sizeof why) == JIT_STATUS_READY, "config override absent");
  CHECK(strcmp(f.calls, "DS") == 0, "override skips detection and uses split mapping; got %s", f.calls);
  CHECK(jit_init(&b, 1 << 20, 0, why, sizeof why) == JIT_STATUS_ALREADY_INIT, "region reserved once");
}

// ---- W^X and execution -------------------------------------------------------------------
static sigjmp_buf jb;
static void on_segv(int s) { (void)s; siglongjmp(jb, 1); }

static void test_wx_and_exec(void) {
  char why[256];
  jit_reset_for_tests();
  CHECK(jit_init(jit_platform_backend(), 1 << 20, -1, why, sizeof why) == JIT_STATUS_READY, "linux backend: %s", why);
  jit_block blk;
  CHECK(jit_alloc(64, 16, &blk) == 0, "alloc");
  // x86-64: mov eax, 42; ret  (this test runs on the x86-64 host)
  static const unsigned char code[] = {0xb8, 0x2a, 0x00, 0x00, 0x00, 0xc3};
  memcpy(blk.rw, code, sizeof code);
  jit_flush(blk.rx, sizeof code);
  int (*fn)(void) = (int (*)(void))(void*)blk.rx;
  CHECK(fn() == 42, "code written through RW runs through RX");

  struct sigaction sa = {0}, old_segv, old_bus;
  sa.sa_handler = on_segv;
  sigaction(SIGSEGV, &sa, &old_segv);
  sigaction(SIGBUS, &sa, &old_bus);
  int faulted = 0;
  if (sigsetjmp(jb, 1) == 0) blk.rx[0] = 0x90;
  else faulted = 1;
  CHECK(faulted, "RX view is not writable");
  faulted = 0;
  if (sigsetjmp(jb, 1) == 0) ((int (*)(void))(void*)blk.rw)();
  else faulted = 1;
  CHECK(faulted, "RW view is not executable");
  sigaction(SIGSEGV, &old_segv, NULL);
  sigaction(SIGBUS, &old_bus, NULL);
  jit_free(&blk);
}

// ---- allocator ---------------------------------------------------------------------------
static void test_allocator(void) {
  char why[256];
  const size_t R = 1 << 20;
  jit_reset_for_tests();
  CHECK(jit_init(jit_platform_backend(), R, -1, why, sizeof why) == JIT_STATUS_READY, "init");
  jit_block a, b, c;
  CHECK(jit_alloc(100, 16384, &a) == 0 && ((uintptr_t)a.rx & 16383) == 0, "16 KB alignment");
  CHECK(a.size == 112, "size rounded to 16, got %zu", a.size);
  CHECK(jit_alloc(R, 16, &b) != 0, "exhaustion is an error, not a crash");
  CHECK(jit_alloc(R - 16384, 16, &b) == 0, "rest of region usable");
  CHECK(jit_free(&a) == 0 && jit_free(&b) == 0, "free");
  CHECK(jit_free(&a) == -1, "double free detected");
  jit_block bad = {.rx = (uint8_t*)&bad, .rw = NULL, .size = 16};
  CHECK(jit_free(&bad) == -1, "foreign pointer rejected");
  CHECK(jit_bytes_in_use() == 0, "all freed");
  CHECK(jit_alloc(R, 16, &c) == 0, "coalesced back to one full-size range");
  jit_free(&c);

  // Randomized: live blocks never overlap and stay inside the region.
  srand(1);
  jit_block live[512];
  int n = 0;
  for (int it = 0; it < 200000; it++) {
    if (n && (rand() % 2 || n == 512)) {
      int i = rand() % n;
      CHECK(jit_free(&live[i]) == 0, "free live block");
      live[i] = live[--n];
    } else {
      size_t sz = 16 + (size_t)(rand() % 8192);
      size_t al = (size_t)16 << (rand() % 11);
      jit_block x;
      if (jit_alloc(sz, al, &x) == 0) {
        CHECK(((uintptr_t)x.rx & (al - 1)) == 0, "alignment %zu", al);
        live[n++] = x;
      }
    }
    if (it % 5000 == 0) {
      for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
          CHECK(live[i].rx + live[i].size <= live[j].rx || live[j].rx + live[j].size <= live[i].rx, "overlap");
    }
  }
  while (n) jit_free(&live[--n]);
  CHECK(jit_bytes_in_use() == 0, "randomized run leaves nothing allocated");
}

int main(void) {
  test_protocol();
  test_wx_and_exec();
  test_allocator();
  printf(fails ? "FAILED (%d)\n" : "PASS\n", fails);
  return fails != 0;
}
