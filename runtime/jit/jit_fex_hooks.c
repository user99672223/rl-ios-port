#include "jit_fex_hooks.h"

#include "jit.h"

#include <stdint.h>
#include <string.h>

// FEX allocates code memory in page-size multiples; align to the 16 KB Apple page.
#define RL_CODE_ALIGN 16384

static bool hook_allocate(size_t size, rl_fex_code_memory* out) {
  jit_block b;
  if (jit_alloc(size, RL_CODE_ALIGN, &b) != 0) return false;
  memset(b.rw, 0, b.size);  // the hook contract requires zero-filled memory
  out->RX = b.rx;
  out->RW = b.rw;
  return true;
}

static void hook_free(void* rx, size_t size) {
  jit_block b = {.rw = NULL, .rx = (uint8_t*)rx, .size = size};
  jit_free(&b);
}

// One constant offset for the whole region: lock free and async-signal-safe, as required.
static void* hook_to_rw(const void* rx) {
  return (void*)((uintptr_t)rx + (uintptr_t)jit_rw_minus_rx());
}

static const rl_fex_code_memory_hooks hooks = {
  .Allocate = hook_allocate,
  .Free = hook_free,
  .ToRW = hook_to_rw,
  .Flush = NULL,
};

const rl_fex_code_memory_hooks* rl_jit_fex_hooks(void) {
  return &hooks;
}
