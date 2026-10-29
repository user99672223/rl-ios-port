// Platform-independent part: startup protocol and the region allocator.
#include "jit.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__APPLE__)
#include <libkern/OSCacheControl.h>
#endif

#define MAX_RANGES 4096

typedef struct {
  size_t off, len;
} range;

static struct {
  pthread_mutex_t lock;
  const jit_backend* be;
  uint8_t* rw;
  uint8_t* rx;
  size_t size;
  size_t in_use;
  // Free list and live list, both sorted by offset.
  range free[MAX_RANGES];
  size_t nfree;
  range live[MAX_RANGES];
  size_t nlive;
} J = {.lock = PTHREAD_MUTEX_INITIALIZER};

static void logf_(const jit_backend* be, const char* fmt, ...) {
  if (!be || !be->log) return;
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  be->log(be->ctx, buf);
}

static jit_status fail(jit_status st, char* reason, size_t n, const char* msg) {
  if (reason && n) snprintf(reason, n, "%s", msg);
  logf_(J.be, "jit: %s: %s", jit_status_str(st), msg);
  return st;
}

const char* jit_status_str(jit_status s) {
  switch (s) {
  case JIT_STATUS_READY: return "ready";
  case JIT_STATUS_NOT_DEBUGGED: return "not-debugged";
  case JIT_STATUS_TXM_UNKNOWN: return "txm-unknown";
  case JIT_STATUS_PREPARE_FAILED: return "prepare-failed";
  case JIT_STATUS_MAP_FAILED: return "map-failed";
  case JIT_STATUS_ALREADY_INIT: return "already-initialized";
  }
  return "?";
}

jit_status jit_init(const jit_backend* be, size_t size, int txm_override, char* reason, size_t n) {
  pthread_mutex_lock(&J.lock);
  jit_status st = JIT_STATUS_READY;
  if (J.rx) {
    st = fail(JIT_STATUS_ALREADY_INIT, reason, n, "the JIT region is reserved once, at startup");
    goto out;
  }
  J.be = be;

  // Step 2: a breakpoint call without the debugger script attached crashes the process, so
  // nothing below runs until CS_DEBUGGED is set.
  if (!be->is_debugged(be->ctx)) {
    st = fail(JIT_STATUS_NOT_DEBUGGED, reason, n,
              "JIT is not enabled. Launch the app through StikDebug.");
    goto out;
  }
  jit_txm txm = txm_override >= 0 ? (jit_txm)txm_override : be->txm_presence(be->ctx);
  logf_(be, "jit: debugger attached; TXM %s%s", txm == JIT_TXM_PRESENT ? "present" : txm == JIT_TXM_ABSENT ? "absent" : "unknown",
        txm_override >= 0 ? " (forced by config)" : "");
  if (txm == JIT_TXM_UNKNOWN) {
    // StikJIT INTEGRATION.md: resolve TXM presence rather than treating unknown as absent.
    st = fail(JIT_STATUS_TXM_UNKNOWN, reason, n,
              "Could not tell whether this device uses TXM. Set jit_txm = true or false in port.toml.");
    goto out;
  }

  void* rx = NULL;
  void* rw = NULL;
  if (txm == JIT_TXM_PRESENT) {
    // Steps 3-5: prepare the RX region, create the RW alias, then detach.
    rx = be->prepare_region(be->ctx, NULL, size);
    logf_(be, "jit: prepare_region(NULL, %zu) -> %p", size, rx);
    if (rx) {
      rw = be->make_rw_alias(be->ctx, rx, size);
      logf_(be, "jit: rw alias -> %p", rw);
    }
    be->detach(be->ctx);
    logf_(be, "jit: detached");
    if (!rx) {
      st = fail(JIT_STATUS_PREPARE_FAILED, reason, n,
                "StikDebug did not prepare a JIT region. Check that the app's script is selected in StikDebug.");
      goto out;
    }
    if (!rw) {
      st = fail(JIT_STATUS_MAP_FAILED, reason, n, "Could not create a writable alias of the JIT region.");
      goto out;
    }
  } else {
    if (be->map_split(be->ctx, size, &rw, &rx) != 0) {
      st = fail(JIT_STATUS_MAP_FAILED, reason, n, "Could not map the JIT region.");
      goto out;
    }
    logf_(be, "jit: split mapping rw=%p rx=%p", rw, rx);
  }

  J.rw = rw;
  J.rx = rx;
  J.size = size;
  J.in_use = 0;
  J.nfree = 1;
  J.free[0] = (range) {0, size};
  J.nlive = 0;
  logf_(be, "jit: ready, %zu bytes, rw - rx = %td", size, (uint8_t*)rw - (uint8_t*)rx);
  if (reason && n) reason[0] = 0;
out:
  pthread_mutex_unlock(&J.lock);
  return st;
}

void jit_reset_for_tests(void) {
  pthread_mutex_lock(&J.lock);
  J.rw = J.rx = NULL;
  J.size = J.in_use = J.nfree = J.nlive = 0;
  pthread_mutex_unlock(&J.lock);
}

static void insert_at(range* a, size_t* n, size_t i, range r) {
  memmove(&a[i + 1], &a[i], (*n - i) * sizeof(range));
  a[i] = r;
  (*n)++;
}

static void remove_at(range* a, size_t* n, size_t i) {
  memmove(&a[i], &a[i + 1], (*n - i - 1) * sizeof(range));
  (*n)--;
}

int jit_alloc(size_t size, size_t align, jit_block* out) {
  if (size == 0 || align < 16 || (align & (align - 1))) return -1;
  size = (size + 15) & ~(size_t)15;
  pthread_mutex_lock(&J.lock);
  int rc = -1;
  if (!J.rx || J.nlive == MAX_RANGES || J.nfree + 1 >= MAX_RANGES) goto out;
  for (size_t i = 0; i < J.nfree; i++) {
    range f = J.free[i];
    // Alignment is of the RX address; rw - rx is page-aligned so RW is aligned the same way.
    uintptr_t base = (uintptr_t)J.rx + f.off;
    size_t pad = (size_t)((align - (base & (align - 1))) & (align - 1));
    if (f.len < pad + size) continue;
    size_t off = f.off + pad;
    // Split the free range into [f.off, off) and [off + size, f.off + f.len).
    remove_at(J.free, &J.nfree, i);
    size_t tail = f.off + f.len - (off + size);
    if (tail) insert_at(J.free, &J.nfree, i, (range) {off + size, tail});
    if (pad) insert_at(J.free, &J.nfree, i, (range) {f.off, pad});
    size_t j = 0;
    while (j < J.nlive && J.live[j].off < off) j++;
    insert_at(J.live, &J.nlive, j, (range) {off, size});
    J.in_use += size;
    out->rx = J.rx + off;
    out->rw = J.rw + off;
    out->size = size;
    rc = 0;
    break;
  }
out:
  pthread_mutex_unlock(&J.lock);
  return rc;
}

int jit_free(const jit_block* b) {
  pthread_mutex_lock(&J.lock);
  int rc = -1;
  if (!J.rx || b->rx < J.rx || b->rx >= J.rx + J.size) goto out;
  size_t off = (size_t)(b->rx - J.rx);
  size_t j = 0;
  while (j < J.nlive && J.live[j].off != off) j++;
  if (j == J.nlive) {
    logf_(J.be, "jit: free of %p which is not a live allocation (double free?)", (void*)b->rx);
    goto out;
  }
  range r = J.live[j];
  remove_at(J.live, &J.nlive, j);
  J.in_use -= r.len;
  // Insert into the free list and coalesce with neighbours.
  size_t i = 0;
  while (i < J.nfree && J.free[i].off < r.off) i++;
  insert_at(J.free, &J.nfree, i, r);
  if (i + 1 < J.nfree && J.free[i].off + J.free[i].len == J.free[i + 1].off) {
    J.free[i].len += J.free[i + 1].len;
    remove_at(J.free, &J.nfree, i + 1);
  }
  if (i > 0 && J.free[i - 1].off + J.free[i - 1].len == J.free[i].off) {
    J.free[i - 1].len += J.free[i].len;
    remove_at(J.free, &J.nfree, i);
  }
  rc = 0;
out:
  pthread_mutex_unlock(&J.lock);
  return rc;
}

void jit_flush(void* rx, size_t len) {
#if defined(__APPLE__)
  // On Darwin __builtin___clear_cache emits a call to __clear_cache, which is not linked on
  // iOS (found by runtime-ios-compile.yml); the system call is sys_icache_invalidate.
  sys_icache_invalidate(rx, len);
#else
  __builtin___clear_cache((char*)rx, (char*)rx + len);
#endif
}

size_t jit_region_size(void) {
  return J.size;
}

size_t jit_bytes_in_use(void) {
  pthread_mutex_lock(&J.lock);
  size_t n = J.in_use;
  pthread_mutex_unlock(&J.lock);
  return n;
}

intptr_t jit_rw_minus_rx(void) {
  return (intptr_t)(J.rw - J.rx);
}
