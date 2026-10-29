#include "SimulatorJITProbe.h"

#include "../../../runtime/jit/jit.h"
#include <TargetConditionals.h>
#include <stdint.h>
#include <string.h>

#if TARGET_OS_SIMULATOR
// The Simulator is a macOS process: a split mapping needs no debugger, so the probe reports
// "debugged" and forces the non-TXM path (txm_override = 0). Everything else is the real
// Darwin backend (vm_remap alias, mprotect, sys_icache_invalidate).
static int always_debugged(void* ctx) {
  (void)ctx;
  return 1;
}
#endif

int rl_simulator_jit_probe(void) {
#if TARGET_OS_SIMULATOR
  jit_backend be = *jit_platform_backend();
  be.is_debugged = always_debugged;
  char why[256];
  if (jit_init(&be, 1 << 20, 0, why, sizeof why) != JIT_STATUS_READY) return -1;
  jit_block b;
  if (jit_alloc(16, 16, &b) != 0) return -2;
  // AArch64: movz w0, #42 ; ret
  static const uint32_t code[] = {0x52800540, 0xd65f03c0};
  memcpy(b.rw, code, sizeof code);
  jit_flush(b.rx, sizeof code);
  int (*fn)(void) = (int (*)(void))(void*)b.rx;
  int r = fn();
  jit_free(&b);
  jit_reset_for_tests();  // the app's real startup reserves the region again
  return r;
#else
  return -100;
#endif
}
