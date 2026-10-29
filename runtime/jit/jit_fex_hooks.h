// runtime/jit as FEX's code-memory hooks (patches/fex/0200: FEXCore/Utils/CodeMemory.h).
// The structs mirror FEXCore::Allocator::CodeMemory and ::CodeMemoryHooks field for field, so a
// pointer to rl_fex_code_memory_hooks can be passed to FEXCore::Allocator::SetCodeMemoryHooks.
#ifndef RL_JIT_FEX_HOOKS_H
#define RL_JIT_FEX_HOOKS_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  void* RX;
  void* RW;
} rl_fex_code_memory;

typedef struct {
  bool (*Allocate)(size_t size, rl_fex_code_memory* out);
  void (*Free)(void* rx, size_t size);
  void* (*ToRW)(const void* rx);
  void (*Flush)(void* rx, size_t size);  // NULL: FEX's default dc cvau (RW) + ic ivau (RX)
} rl_fex_code_memory_hooks;

// Hooks backed by the region jit_init reserved. jit_init must have returned JIT_STATUS_READY.
const rl_fex_code_memory_hooks* rl_jit_fex_hooks(void);

#ifdef __cplusplus
}
#endif
#endif
