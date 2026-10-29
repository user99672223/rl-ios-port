// Native x86-64 reference runner for tests/diff.
// Usage: runner CASE.bin MEMDATA.hex
//   CASE.bin     flat binary assembled with -DNATIVE (body ends in `ret`)
//   MEMDATA.hex  hex bytes preloaded at SCRATCH_BASE (same bytes FEX gets via MemoryData)
// Prints the final register state as one JSON object, in FEX RegData format.
#define _GNU_SOURCE
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define SCRATCH_BASE 0x100000000ULL
#define SCRATCH_SIZE 4096

struct state {
  uint64_t gpr[16];
  uint64_t xmm[16][2];
};

void run_case(void *code, struct state *out);

static const char *gpr_names[16] = {"RAX", "RBX", "RCX", "RDX", "RSI", "RDI", "RBP", "RSP",
                                    "R8",  "R9",  "R10", "R11", "R12", "R13", "R14", "R15"};

static void *read_file(const char *path, size_t *len) {
  FILE *f = fopen(path, "rb");
  if (!f) { perror(path); exit(2); }
  fseek(f, 0, SEEK_END);
  *len = (size_t)ftell(f);
  fseek(f, 0, SEEK_SET);
  void *buf = malloc(*len + 1);
  if (fread(buf, 1, *len, f) != *len) { perror(path); exit(2); }
  ((char *)buf)[*len] = 0;
  fclose(f);
  return buf;
}

int main(int argc, char **argv) {
  if (argc != 3) { fprintf(stderr, "usage: %s CASE.bin MEMDATA.hex\n", argv[0]); return 2; }

  uint8_t *scratch = mmap((void *)SCRATCH_BASE, SCRATCH_SIZE, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
  if (scratch != (void *)SCRATCH_BASE) { perror("mmap scratch"); return 2; }
  size_t hexlen;
  char *hex = read_file(argv[2], &hexlen);
  size_t n = 0;
  for (char *p = hex; *p && n < SCRATCH_SIZE;) {
    unsigned v;
    int used;
    if (sscanf(p, " %2x%n", &v, &used) != 1) break;
    scratch[n++] = (uint8_t)v;
    p += used;
  }

  size_t codelen;
  void *bin = read_file(argv[1], &codelen);
  void *code = mmap(NULL, codelen, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  memcpy(code, bin, codelen);
  mprotect(code, codelen, PROT_READ | PROT_EXEC);

  struct state st = {0};
  run_case(code, &st);

  printf("{");
  for (int i = 0; i < 16; i++) {
    if (i == 7) continue;  // RSP differs by construction between harnesses
    printf("\"%s\": \"0x%" PRIx64 "\", ", gpr_names[i], st.gpr[i]);
  }
  for (int i = 0; i < 16; i++)
    printf("\"XMM%d\": [\"0x%" PRIx64 "\", \"0x%" PRIx64 "\"]%s", i, st.xmm[i][0], st.xmm[i][1],
           i == 15 ? "" : ", ");
  printf("}\n");
  return 0;
}
