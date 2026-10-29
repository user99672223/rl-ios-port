// JIT region for FEX's code buffers on iOS, enabled through StikDebug (spec 6.3, 6.4, 9).
//
// One region is reserved at startup and never grows: on TXM/SPTM devices a region added later
// cannot be prepared once the debugger script has detached (StikJIT INTEGRATION.md, "Preallocate
// executable regions before detaching", StikDebug/StikJIT@3228726). The region has an RX view
// (executed) and an RW alias (written) at a constant offset; the host never maps W+X.
//
// Startup order on TXM devices follows StikJIT INTEGRATION.md steps 1-6:
//   wait for CS_DEBUGGED -> prepare every RX region -> create RW aliases -> detach -> ready.
// Without TXM, debugger attachment alone allows the split mapping (QEMU tcg/region.c,
// alloc_code_gen_buffer_splitwx_vmremap).
#ifndef RL_JIT_H
#define RL_JIT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  JIT_TXM_UNKNOWN = -1,
  JIT_TXM_ABSENT = 0,
  JIT_TXM_PRESENT = 1,
} jit_txm;

typedef enum {
  JIT_STATUS_READY = 0,
  JIT_STATUS_NOT_DEBUGGED,    // no debugger attached: show "Launch through StikDebug"
  JIT_STATUS_TXM_UNKNOWN,     // can't tell whether the breakpoint protocol is needed
  JIT_STATUS_PREPARE_FAILED,  // debugger script did not return an RX region
  JIT_STATUS_MAP_FAILED,      // RX region or RW alias could not be created
  JIT_STATUS_ALREADY_INIT,
} jit_status;

// Platform operations. jit_darwin.c provides the real ones; tests provide fakes.
typedef struct jit_backend {
  void* ctx;
  int (*is_debugged)(void* ctx);
  jit_txm (*txm_presence)(void* ctx);
  // TXM path: breakpoint protocol. prepare_region(NULL, len) asks the debugger for an RX
  // region and returns its address (NULL on failure). Must only be called while debugged.
  void* (*prepare_region)(void* ctx, void* addr, size_t len);
  void (*detach)(void* ctx);
  // Creates a writable alias of an existing RX region; NULL on failure.
  void* (*make_rw_alias)(void* ctx, void* rx, size_t len);
  // Non-TXM path: create both views of a fresh region. Returns 0 on success.
  int (*map_split)(void* ctx, size_t len, void** rw, void** rx);
  void (*log)(void* ctx, const char* msg);
} jit_backend;

// The platform's backend (Darwin on Apple targets, memfd dual mapping on Linux).
const jit_backend* jit_platform_backend(void);

// Reserve the region. txm_override: -1 = detect, 0 = force absent, 1 = force present
// (port.toml `jit_txm`). On failure, reason holds a sentence for the user and the log.
jit_status jit_init(const jit_backend* backend, size_t region_size, int txm_override, char* reason, size_t reason_len);
const char* jit_status_str(jit_status status);
// Only for tests: forget the region (does not unmap).
void jit_reset_for_tests(void);

typedef struct {
  uint8_t* rw;  // write here
  uint8_t* rx;  // execute here
  size_t size;
} jit_block;

// Allocate `size` bytes aligned to `align` (power of two, >= 16). Returns 0 on success.
int jit_alloc(size_t size, size_t align, jit_block* out);
// Returns 0 on success, -1 if the block is not a live allocation (double free, bad pointer).
int jit_free(const jit_block* block);
// Make newly written code visible to instruction fetch at `rx`.
void jit_flush(void* rx, size_t len);

size_t jit_region_size(void);
size_t jit_bytes_in_use(void);
// rw - rx, constant for the whole region; FEX's split-W^X patch uses it (RISKS.md R-007).
intptr_t jit_rw_minus_rx(void);

#ifdef __cplusplus
}
#endif
#endif
