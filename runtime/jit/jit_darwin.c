// Darwin backend: StikDebug breakpoint protocol, CS_DEBUGGED check, TXM detection, split mapping.
// Sources for every system interface used here:
//   csops / CS_OPS_STATUS          xnu bsd/sys/codesign.h:41,72
//   CS_DEBUGGED                    xnu osfmk/kern/cs_blobs.h:71
//   mach_vm_remap                  xnu osfmk/mach/mach_vm.defs:390-404
//   IORegistryEntryFromPath etc.   IOKitUser IOKitLib.h:266-268, 989-992, 1186-1191;
//                                  kIOMainPortDefault is documented as a synonym for NULL (:107-113)
//   kIORegistryEntryPropertyKeysKey xnu iokit/IOKit/IOKitKeys.h:59
//   TXM detection method           StikDebug/StikJIT@3228726 Sources/ProcessInfo+TXM.swift
//   breakpoint calls (x16, brk)    StikDebug/StikDebug@17b1e1d Scripts/universal.js header,
//                                  StikDebug/StikJIT@3228726 INTEGRATION.md (Part 1)
//   split mapping                  QEMU tcg/region.c alloc_code_gen_buffer_splitwx_vmremap
#if defined(__APPLE__)
#include "jit.h"

#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <stdio.h>
#include <sys/mman.h>
#include <unistd.h>

// Not in the public iOS SDK headers; declared as in xnu bsd/sys/codesign.h.
extern int csops(pid_t pid, unsigned int ops, void* useraddr, size_t usersize);
#define RL_CS_OPS_STATUS 0
#define RL_CS_DEBUGGED 0x10000000

// IOKit headers are not public in the iOS SDK; declared as in IOKitUser IOKitLib.h.
typedef mach_port_t io_object_t;
typedef io_object_t io_registry_entry_t;
typedef char io_string_t[512];
typedef uint32_t IOOptionBits;
extern io_registry_entry_t IORegistryEntryFromPath(mach_port_t mainPort, const io_string_t path);
extern CFTypeRef IORegistryEntryCreateCFProperty(io_registry_entry_t entry, CFStringRef key, CFAllocatorRef allocator, IOOptionBits options);
extern kern_return_t IOObjectRelease(io_object_t object);

// StikDebug "JIT syscalls": command number in x16, then brk #0xf00d; the script advances pc
// past the brk and returns the result in x0. Only valid while the script is attached.
__attribute__((noinline, naked)) static void rl_jit26_detach(void) {
  __asm__ volatile("mov x16, #0\n"
                   "brk #0xf00d\n"
                   "ret");
}

__attribute__((noinline, naked)) static void* rl_jit26_prepare_region(void* addr, size_t len) {
  __asm__ volatile("mov x16, #1\n"
                   "brk #0xf00d\n"
                   "ret");
}

static int darwin_is_debugged(void* ctx) {
  (void)ctx;
  uint32_t flags = 0;
  if (csops(getpid(), RL_CS_OPS_STATUS, &flags, sizeof(flags)) != 0) return 0;
  return (flags & RL_CS_DEBUGGED) != 0;
}

static jit_txm darwin_txm_presence(void* ctx) {
  (void)ctx;
  io_registry_entry_t e = IORegistryEntryFromPath(MACH_PORT_NULL, "IODeviceTree:/chosen/memory-map");
  if (e == MACH_PORT_NULL) return JIT_TXM_UNKNOWN;
  CFTypeRef keys = IORegistryEntryCreateCFProperty(e, CFSTR("IORegistryEntryPropertyKeys"), kCFAllocatorDefault, 0);
  IOObjectRelease(e);
  if (!keys) return JIT_TXM_UNKNOWN;
  jit_txm r = JIT_TXM_UNKNOWN;
  if (CFGetTypeID(keys) == CFArrayGetTypeID()) {
    CFArrayRef a = (CFArrayRef)keys;
    r = CFArrayContainsValue(a, CFRangeMake(0, CFArrayGetCount(a)), CFSTR("TXM")) ? JIT_TXM_PRESENT : JIT_TXM_ABSENT;
  }
  CFRelease(keys);
  return r;
}

static void* darwin_prepare_region(void* ctx, void* addr, size_t len) {
  (void)ctx;
  return rl_jit26_prepare_region(addr, len);
}

static void darwin_detach(void* ctx) {
  (void)ctx;
  rl_jit26_detach();
}

static void* remap_alias(void* src, size_t len) {
  mach_vm_address_t dst = 0;
  vm_prot_t cur = 0, max = 0;
  kern_return_t kr = mach_vm_remap(mach_task_self(), &dst, len, 0, VM_FLAGS_ANYWHERE, mach_task_self(),
                                   (mach_vm_address_t)(uintptr_t)src, FALSE, &cur, &max, VM_INHERIT_NONE);
  if (kr != KERN_SUCCESS) {
    fprintf(stderr, "jit: mach_vm_remap failed: %d\n", kr);
    return NULL;
  }
  return (void*)(uintptr_t)dst;
}

static void* darwin_make_rw_alias(void* ctx, void* rx, size_t len) {
  (void)ctx;
  void* rw = remap_alias(rx, len);
  if (rw && mprotect(rw, len, PROT_READ | PROT_WRITE) != 0) {
    perror("jit: mprotect rw alias");
    return NULL;
  }
  return rw;
}

static int darwin_map_split(void* ctx, size_t len, void** rw, void** rx) {
  (void)ctx;
  void* w = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (w == MAP_FAILED) return -1;
  void* x = remap_alias(w, len);
  if (!x || mprotect(x, len, PROT_READ | PROT_EXEC) != 0) return -1;
  *rw = w;
  *rx = x;
  return 0;
}

static void darwin_log(void* ctx, const char* msg) {
  (void)ctx;
  fprintf(stderr, "%s\n", msg);
}

static const jit_backend darwin_backend = {
  .ctx = NULL,
  .is_debugged = darwin_is_debugged,
  .txm_presence = darwin_txm_presence,
  .prepare_region = darwin_prepare_region,
  .detach = darwin_detach,
  .make_rw_alias = darwin_make_rw_alias,
  .map_split = darwin_map_split,
  .log = darwin_log,
};

const jit_backend* jit_platform_backend(void) {
  return &darwin_backend;
}
#endif
