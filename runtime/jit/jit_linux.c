// Linux backend: the same RW/RX split built from one memfd mapped twice. Used for tests and
// for running the runtime on Linux; there is no debugger protocol, so it reports "debugged"
// and "TXM absent" and uses map_split.
#if defined(__linux__)
#define _GNU_SOURCE
#include "jit.h"

#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

static int linux_is_debugged(void* ctx) {
  (void)ctx;
  return 1;
}

static jit_txm linux_txm_presence(void* ctx) {
  (void)ctx;
  return JIT_TXM_ABSENT;
}

static void* linux_prepare_region(void* ctx, void* addr, size_t len) {
  (void)ctx, (void)addr, (void)len;
  return NULL;
}

static void linux_detach(void* ctx) {
  (void)ctx;
}

static void* linux_make_rw_alias(void* ctx, void* rx, size_t len) {
  (void)ctx, (void)rx, (void)len;
  return NULL;
}

int jit_linux_map_split(size_t len, void** rw, void** rx, int* fd_out) {
  int fd = memfd_create("rl-jit", MFD_CLOEXEC);
  if (fd < 0 || ftruncate(fd, (off_t)len) != 0) return -1;
  void* w = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  void* x = mmap(NULL, len, PROT_READ | PROT_EXEC, MAP_SHARED, fd, 0);
  if (w == MAP_FAILED || x == MAP_FAILED) return -1;
  *rw = w;
  *rx = x;
  if (fd_out) *fd_out = fd;
  else close(fd);
  return 0;
}

static int linux_map_split(void* ctx, size_t len, void** rw, void** rx) {
  (void)ctx;
  return jit_linux_map_split(len, rw, rx, NULL);
}

static void linux_log(void* ctx, const char* msg) {
  (void)ctx;
  fprintf(stderr, "%s\n", msg);
}

static const jit_backend linux_backend = {
  .ctx = NULL,
  .is_debugged = linux_is_debugged,
  .txm_presence = linux_txm_presence,
  .prepare_region = linux_prepare_region,
  .detach = linux_detach,
  .make_rw_alias = linux_make_rw_alias,
  .map_split = linux_map_split,
  .log = linux_log,
};

const jit_backend* jit_platform_backend(void) {
  return &linux_backend;
}
#endif
