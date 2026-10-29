# FEX W^X split (RISKS R-007)

Patch series `patches/fex/0200..0206`, applied by `tools/apply_patches.sh` onto FEX-2610 (`14c9268`).
Local only (D-008): FEX's `CLAUDE.md` forbids AI-generated contributions, so none of this goes upstream.
File:line references below are in the patched FEX tree.

## Design

Every code allocation has two views of the same bytes: **RX** (what executes) and **RW** (what is written).
For any byte inside one allocation `rw = rx + diff`, `diff` constant per allocation (iOS: `jit_rw_minus_rx()`).

* Every address that ends up in generated code or in FEX data is the **RX** address: branch offsets, `adr`
  targets, literals, return addresses, `DispatchPtr` and the other dispatcher entry points, lookup-cache
  `HostCode`, `ExitFunctionLinkData` records, `InlineJITBlockHeader`, signal PCs.
* Code is only ever stored through the RW view; instruction-cache maintenance names the RX address and the
  data-cache clean uses the RW address (`FlushCodeMemory`, `CodeMemory.cpp`).
* With no hook installed both views are the same RWX mapping and nothing changes (`ActiveCodeMemoryHooks == nullptr`,
  `CodeMemoryToRW` is the identity, `FlushCodeMemory` is `__builtin___clear_cache`). The emitter bias is 0.

### Hook interface (0200) — `FEXCore/include/FEXCore/Utils/CodeMemory.h`

```cpp
struct CodeMemoryHooks {
  bool  (*Allocate)(size_t Size, CodeMemory* Out);   // Out->RX / Out->RW, zero filled
  void  (*Free)(void* RX, size_t Size);
  void* (*ToRW)(const void* RX);                     // lock free, async-signal-safe (signal handlers use it)
  void  (*Flush)(void* RX, size_t Size);             // optional; default dc cvau (RW) + ic ivau (RX)
};
SetCodeMemoryHooks(&Hooks);  AllocateCodeMemory(Size);  FreeCodeMemory(RX, Size);
CodeMemoryToRW(ptr);         FlushCodeMemory(RX, Size);  IsCodeMemorySplit();
```

iOS binding: `Allocate` = `jit_alloc` (`rw`, `rx`), `Free` = `jit_free`, `ToRW` = `rx + jit_rw_minus_rx()`,
`Flush` = `jit_flush(rx, len)` (or leave null for the default). The guard page of a code buffer is protected through the
**RW** view only (`SharedCodeBufferManager.cpp:30`); the RX region provided by the debugger cannot be `mprotect`ed, and
naming / THP `madvise` are skipped when split (`SharedCodeBufferManager.cpp:36-43`, `Dispatcher.cpp:49`).

### Emitter (0201)

`ARMEmitter::Buffer` keeps the cursor in the **written** (RW) space so the hot emission path (`dcn`, `Align`) is
unchanged, and carries `RXBias = rx - rw` (`CodeEmitter/CodeEmitter/Buffer.h:20-28,120`).
`GetCursorAddress`, `GetBufferBase`, `GetCursorOffsetFromAddress` are in RX space (`Buffer.h:69,90,112`), so label
locations, `adr`/branch offsets and anything that records an address are RX. `Emitter::Bind(ForwardLabel::Reference)`
reads the referenced instruction at its RX location and patches it through `ToWritable` (`Emitter.h:665,682,702,720,739`);
`LONG_ADDRESS_GEN` re-emits through the cursor, so it goes through the RW view already.
FEX does not use vixl's assembler: vixl is only the simulator/disassembler (`External/vixl`), nothing to change there.

### Write sites changed

| Site | Where (patched tree) | Change |
|---|---|---|
| Code buffer alloc/free/guard | `SharedCodeBufferManager.cpp:22,30,52` | `AllocateCodeMemory`, `RWMinusRX`, guard via RW |
| Writable alias accessor | `SharedCodeBufferManager.h:97,102` | `ToWritable`, `GetBufferBaseWritable` |
| Dispatcher alloc, emission, free, flush | `Dispatcher.cpp:40,49,57,675` | emitted via RW, RX recorded, `FlushCodeMemory` |
| Dispatcher forward-label patching | `Emitter.h:665-739` | patch through RW |
| Block copy temp buffer -> code buffer | `JIT.cpp:1166` | `memcpy` to `ToWritable(BufferAllocationOffset)` |
| Block icache maintenance | `JIT.cpp:1171` | `FlushCodeMemory(RX)` |
| Disk-cache hit copy (D-007) | `JIT.cpp:1211-1212` (`LoadCachedCode`) | RW copy, flush RX |
| Block link / relink | `JIT.cpp:629-630,634,643-644` | branch and `ldr` stores via RW; `Record->HostCode` via RW |
| Block delink | `JIT.cpp:551-552,562-563` | stores via RW |
| Unaligned-atomic backpatch (signal) | `Arm64.cpp:2105-2160` | stores via RW; `SpinLockFutex` in the block tail via RW |
| Code cache (`ENABLECODECACHINGWIP`) alloc/free | `CodeCache.cpp:52,60,713-719,734-743` | split-aware load, RX span + RW span |
| Code cache finalize | `CodeCache.cpp:867,972-981,1040` | eager, relocations via RW, flush RX |
| Code cache store/validate (relocate live buffer in place) | `CodeCache.cpp:360-361,430` | writable alias |

Audited and unchanged (no code writes): block emission itself runs into a per-thread temp buffer (`JIT.cpp:925`
region, `RXBias` 0) and is position independent, as `Arm64Emitter.cpp:524-529` notes; DiskCache relocation patching
works on a heap staging buffer (`DiskCache.cpp:850`); `CodeBuffer::Reset` only moves the cursor
(`CodeCache.cpp` `NewCodeBuffer->Reset()`); invalidation goes through the delinkers above; `BPFEmitter` emits seccomp BPF, not
host code; `atomic_segmented_bitmap_allocator.h:63` has an executable allocation but no users; guest-visible RWX
mappings (`Thunks.cpp:325`, `TestHarnessRunner.cpp:315,390`, `Module.cpp:695`) are x86 guest memory, not host JIT code.

## Test mode (Linux): `FEX_TEST_SPLITWX=1`

`CodeMemory.cpp` `MaybeEnableTestMode` installs hooks at the first code allocation: each allocation is a `memfd`
mapped twice with `MAP_SHARED`, RX view `PROT_READ|PROT_EXEC`, RW view `PROT_READ|PROT_WRITE`. A write through an RX
address is `SEGV_ACCERR`; on ARM64 a fetch from the RW view faults too. Ranges are tracked in a lock-free table for `ToRW`.

vixl simulator: `Simulator::RunFrom(first)` sets the pc (`simulator-aarch64.cc:840`) and `ExecuteInstruction` decodes
the instruction at `pc_` straight from host memory (`simulator-aarch64.h:1398`, `decoder-aarch64.cc:39`). FEX passes
`DispatchPtr`/`CallbackPtr` (`Dispatcher.cpp` `ExecuteDispatch`), which are RX addresses, and linked blocks branch to RX
addresses, so the simulator really fetches from the RX view.

## Evidence (x86-64 host, vixl simulator, `-DENABLE_VIXL_SIMULATOR=ON`)

Private copy `build/fex-src`, configure line from the task. ASM suite = `ctest -R '.*64Bit.*.asm$'` (what
`64bit_asm_tests` runs), 6810 tests.

| Run | Result |
|---|---|
| unpatched FEX-2610 | 6525 pass, 285 skipped (Known_Failures/Disabled), 0 fail |
| patched, no switch | identical per-test set (0 differing) |
| patched, `FEX_TEST_SPLITWX=1` | identical per-test set (0 differing) |

Differential (`diff.py gen --count 200`, then `run`): in the simulator 159 of 200 cases fail on the unpatched build in both
`jit_1` and `jit_500_m` (R14, the sticky MXCSR flags, which the simulator does not model). The failing set is identical
unpatched, patched, and patched+split. A stricter comparison of the complete GPR/XMM dump for all 200 cases across the
three variants also has 0 differences.

Negative self-checks (split mode, 6810 tests): writing the block copy through RX (`JIT.cpp:1166`) gives 6519 failures
(`SEGV_ACCERR` at an RX address) while the same binary without the switch passes 100%; writing the block link patch through
RX (`JIT.cpp:629`) gives 2333 failures; making `Emitter::Bind` patch through RX gives 6519 failures. All restored; final
split run is 0 differing tests again.

## ARM64EC / Windows executable-allocation inventory (iOS: nothing may use `VirtualAlloc(..., Execute=true)`)

Under the prot4k iOS policy an executable `VirtualAlloc`/`NtAllocateVirtualMemory` is treated as emulated guest memory
(no host X) unless it is an EC code range. FEX's host-executed ARM64 code therefore has to come from the hook
(`runtime/jit`), which Wine never sees. Every site that allocates executable memory in the Windows tree:

| Allocation | Where | Host-executed ARM64? | Status |
|---|---|---|---|
| JIT code buffers | `SharedCodeBufferManager.cpp:22` (`AllocateCodeMemory`) | yes | through the hook |
| Dispatcher | `Dispatcher.cpp:40` (`AllocateCodeMemory`) | yes | through the hook |
| Mapped code cache (`ENABLECODECACHINGWIP`), ARM64EC | `CodeCache.cpp:734-743` | yes | through the hook (was `VirtualAlloc(Size, true)`); finalize writes via the RW alias (`CodeCache.cpp:972-981`), freed with `FreeCodeMemory` (`CodeCache.cpp:60`) |
| Default `AllocateCodeMemory` when no hook is installed | `CodeMemory.cpp` `AllocateCodeMemory` -> `AllocatorHooks.h:49-61` (`VirtualAlloc2` with `MEM_EXTENDED_PARAMETER_EC_CODE`, `PAGE_EXECUTE_READWRITE`) | yes | upstream behaviour; on iOS a hook is mandatory |
| `X64ReturnInstr` | `Source/Windows/ARM64EC/Module.cpp:695-698` | **no**: one x86-64 `ret` (0xC3) byte, used as a return address from native ARM64 code (`Module.S:95-96`) and executed by FEX as *guest* code, then registered with the InvalidationTracker as `PAGE_EXECUTE_READ` | **left on `VirtualAlloc` on purpose**: it must be emulated guest-executable memory, which is what the Wine policy gives an executable VirtualAlloc; moving it into hook memory would hide it from guest-code tracking |
| WoW64 32-bit address space reservation | `Source/Windows/WOW64/Module.cpp:550` (`NtAllocateVirtualMemory`, `PAGE_EXECUTE_READWRITE`) | no (guest 32-bit memory) | left, guest memory |
| `atomic_segmented_bitmap_allocator.h:63` | `VirtualAlloc(arena_size, true)` | would be, but it has no users | left |
| Staging buffer for lazy code-cache finalize | `CodeCache.cpp:1000` (non-Windows only) | no (copied then remapped) | non-split only; not reached when split |

Nothing else in `Source/Windows` allocates executable memory (`WinAPI/Alloc.cpp` forwards the guest's own calls;
`InvalidationTracker`/`ImageTracker` only change protections of guest images).

Open for the ARM64EC FEX module: `SetCodeMemoryHooks` must run before the first `Context` is created. The module is a PE DLL
inside Wine and cannot link `runtime/jit` directly, so it needs a way to get the three function pointers (for example an
export that `runtime/jit` or the Wine unixlib fills in before `libarm64ecfex.dll` attaches). Not designed here.

## Not exercised / left

* ARM64-only paths cannot run in the simulator (no host SIGBUS): the unaligned-atomic backpatch
  `Arm64.cpp:2105-2160` and the real `dc cvau`/`ic ivau` flush (`CodeMemory.cpp` `SplitFlushARM64`). They run in the
  native ARM64 CI workflow `fex-splitwx.yml`, but only if the ASM tests hit them (some `Atomics` tests are meant to).
* `ENABLECODECACHINGWIP` mapped code cache in split mode (`CodeCache.cpp:713-719,972-981`): written, builds, not run
  (it needs generated caches). Non-split behaviour is untouched, including the RWX staging buffer + `mremap`
  (`CodeCache.cpp:1000,1020`), which needs RWX by design and stays upstream-only.
* Windows / ARM64EC / WoW64 code cannot be built or run on this host. The `#ifdef _M_ARM64EC` edits
  (`CodeCache.cpp:60,734-743,972-981`) were written by reading and are unexercised. See the inventory below.
* `JIT.cpp:637` `dc cvau` is given `Record->HostCode` (the value, a code address) rather than its address; this is
  upstream behaviour and harmless (data caches are physically tagged), left as is.
* No hook implementation for the app side yet: `runtime/jit` needs a small adapter that fills `CodeMemoryHooks` and calls
  `SetCodeMemoryHooks` before the first `Context` is created.
* Performance: the only per-instruction cost on the default path is none (cursor stays RW); `GetCursorAddress` adds one
  load + add per label use. Not measured.

## Proposed DECISIONS / RISKS text

**D-011 — W^X via a code-memory hook in FEXCore, RX addresses everywhere, writes through an RW alias.**
FEX gets `FEXCore/Utils/CodeMemory.h` (patches 0200-0206). Code buffers and the dispatcher are allocated through
`AllocateCodeMemory`, which returns an RX and an RW view; all stores go through the RW alias and every address embedded in
code or FEX data is the RX address, as in QEMU's `tcg_splitwx_diff`. Default (no hook) is the upstream RWX mapping.
Completeness is proven on Linux by `FEX_TEST_SPLITWX=1` (memfd, RX view not writable, RW view not executable); the ASM suite
and the differential cases give identical results with and without it, and three deliberately reverted write sites fail.

**R-007 — update:** status "implemented, verified in simulator and on ARM64 CI; open: app-side hook adapter in
`runtime/jit`, Windows/ARM64EC paths, mapped code cache in split mode (file:line in `docs/notes/fex-splitwx.md`)".
