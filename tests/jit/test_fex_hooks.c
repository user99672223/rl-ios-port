// The runtime/jit adapter against FEX's CodeMemoryHooks contract (patches/fex/0200).
#define _GNU_SOURCE
#include "../../runtime/jit/jit.h"
#include "../../runtime/jit/jit_fex_hooks.h"

#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c, m) do { if (!(c)) { fails++; printf("FAIL: %s\n", m); } } while (0)

int main(void) {
  char why[256];
  jit_reset_for_tests();
  CHECK(jit_init(jit_platform_backend(), 4 << 20, -1, why, sizeof why) == JIT_STATUS_READY, "init");
  const rl_fex_code_memory_hooks* h = rl_jit_fex_hooks();
  CHECK(h->Allocate && h->Free && h->ToRW && !h->Flush, "hook table shape");

  rl_fex_code_memory a, b;
  CHECK(h->Allocate(65536, &a), "allocate 64 KB");
  CHECK(((uintptr_t)a.RX & 16383) == 0, "16 KB aligned");
  int zero = 1;
  for (size_t i = 0; i < 65536; i++) zero &= ((unsigned char*)a.RX)[i] == 0;
  CHECK(zero, "zero-filled");
  memset(a.RW, 0xAA, 65536);  // dirty it, free it, reallocate: must come back zeroed
  h->Free(a.RX, 65536);
  CHECK(h->Allocate(65536, &b), "reallocate");
  zero = 1;
  for (size_t i = 0; i < 65536; i++) zero &= ((unsigned char*)b.RX)[i] == 0;
  CHECK(zero, "reallocation zero-filled again");
  CHECK(h->ToRW(b.RX) == b.RW, "ToRW(base) == RW base");
  CHECK(h->ToRW((char*)b.RX + 12345) == (char*)b.RW + 12345, "ToRW(interior) keeps the offset");
  // Written through RW, visible (and executable) through RX: x86-64 mov eax, 7; ret
  static const unsigned char code[] = {0xb8, 0x07, 0x00, 0x00, 0x00, 0xc3};
  memcpy(h->ToRW((char*)b.RX + 128), code, sizeof code);
  jit_flush((char*)b.RX + 128, sizeof code);
  CHECK(((int (*)(void))(void*)((char*)b.RX + 128))() == 7, "write via ToRW, run via RX");
  CHECK(!h->Allocate(64 << 20, &a), "oversized allocation fails cleanly");
  h->Free(b.RX, 65536);
  CHECK(jit_bytes_in_use() == 0, "all freed");
  printf(fails ? "FAILED (%d)\n" : "PASS\n", fails);
  return fails != 0;
}
