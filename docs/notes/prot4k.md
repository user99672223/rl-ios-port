# prot4k — 4 KB Windows page protections over 16 KB host pages

Status: library, tests and Wine patches done and verified on this Debian x86-64 host (4 KB pages)
with a mock 16 KB host. Not yet run on Apple silicon or in the iOS Simulator: see section 9.

Files: `runtime/prot4k/` (library), `tests/prot4k/` (tests), `patches/wine/0100..0105-*.patch`.
All Wine citations are `external/wine` at the pinned MR 11638 head `de56343b` (the *pinned tree*)
unless marked *upstream*, which means `wine/wine` master at `66caeb46` (2026-10-28, Wine 11.20).

## 1. What the study found

### 1.1 The pinned Wine already keeps 4 KB state over larger host pages

Stock behaviour when the host page is larger than 4 KB, on `__aarch64__`:

| Piece | Where (pinned tree) |
|---|---|
| `host_page_size` / `host_page_mask` are variables on aarch64, constants (4096) elsewhere; the host size is read with `sysconf(_SC_PAGESIZE)` in `virtual_init` | `virtual.c:176-182`, `virtual.c:3757-3760` |
| Mach's `host_page_size()` function is renamed out of the way on macOS | `virtual.c:66-74` |
| Windows page size is the constant 4096 | `unix_private.h:171` |
| One protection byte per 4 KB page (`pages_vprot`, 2-level table); flags `VPROT_READ/WRITE/EXEC/WRITECOPY/GUARD/COMMITTED/WRITEWATCH` | `virtual.c:134-140`, `232-239`, `1107-1118` |
| Union of the 4 parts of a host page: `get_host_page_vprot` ORs the bytes | `virtual.c:1126-1142` |
| Byte to `PROT_*`: `get_unix_prot` (a GUARD bit anywhere makes the page `PROT_NONE`; a WRITEWATCH bit removes `PROT_WRITE`) | `virtual.c:1381-1394` |
| `mprotect_range` walks host pages, coalesces equal protections | `virtual.c:1974-1996` |
| Queries come from the 4 KB table: `get_vprot_range_size` (runs of equal bytes), `get_committed_size`, `fill_basic_memory_info` | `virtual.c:1153-1197`, `2462-2496`, `5796-5852` |
| `NtProtectVirtualMemory` returns the old protection of the first 4 KB page and requires all pages committed | `virtual.c:5694-5703` |
| Allocation sizes/bases rounded to host pages (`map_view`), 64 KB base granularity | `virtual.c:2288`, `2332` |
| `decommit_pages` replaces only whole host pages | `virtual.c:2505-2520` |
| Stack guard areas are whole host pages | `virtual.c:4529-4543`, `4690-4701`, `4800` |
| Fault handler clears guard / write watch on the **whole host page** | `virtual.c:4737-4769` |
| Write watch (software): a clean page keeps `VPROT_WRITEWATCH`, which removes host write; a fault clears it; `NtGetWriteWatch` reports pages whose bit is clear | `virtual.c:1390`, `4750-4762`, `6831-6849` |

The MR (11638) adds, on top of upstream, a two-pass image protection under `#ifdef __APPLE__`:
`set_vprot_track` records every section's final protection, then `mprotect_range` applies them
(`virtual.c:2008-2021`, `3277-3325`). Its own comment (`3284-3293`) states the limit: it works only
when executable and writable sections never share a host page, "such binaries simply cannot be
loaded on this platform". That is exactly the x86-64 case: PE sections of x86-64 images are 4 KB
aligned (Wine's `get_section_alignment` gives 0x10000 only for ARM64/ARM64EC,
`tools/winebuild/utils.c:702-712`).

So the pinned Wine does **not** do the job for us: it has the data structure and the union, but
(a) the union of an RX and an RW 4 KB part is RWX, which iOS/macOS refuse (spec section 5, W^X row),
(b) guard and write watch are fault-driven, which spec 6.8 forbids, (c) several 4 KB semantics leak
(section 2).

### 1.2 Upstream Wine master

The host-page-size series is upstream, all by Alexandre Julliard, 2025-03-25, all titles only
(the commits have no message body): `6e1c906fa` "Retrieve the host page size on the Unix side on
ARM64", `d813ffc35`/`71d51a760` "Align virtual memory allocations/deallocations to the host page
size", `102a49bbc` "Apply virtual page protections to the entire host page", `d9d012965` "Align
file mappings", `7085166fd` "Align stack guard pages", `431cd9b0d` mlock/munlock/msync,
`ee8a2bc88` "server: Support the host page size being different from the Windows page size".
Later `ed091c47` (2026-05-07, Jacek Caban) "server: Store the client page size in the process
struct". Master's `virtual.c` has the same `host_page_size` (168-172), `get_host_page_vprot`
(828-846), `mprotect_range` (1618-1643), `decommit_pages` (2087-2102), `sysconf` (3257).
It has **no** `set_vprot_track`, no two-pass image protection, no W^X handling, no zeroing of
partially decommitted host pages. The pinned tree = upstream + MR 11638's Apple silicon work, and
the 4 KB layer in both is "union of bytes, then mprotect".

### 1.3 Asahi Linux

Asahi does not emulate 4 KB pages inside Wine. It runs the whole x86 stack in a microVM with a 4 KB
page guest kernel:
- Fedora Asahi Remix x86 docs: "we are using a microVM to run an entirely separate guest Linux
  kernel in 4K page size mode" (muvm, FEX, Proton inside it):
  https://docs.fedoraproject.org/en-US/fedora-asahi-remix/x86-support/
- Fedora change proposal "Integrate FEX in Fedora Linux": FEX only supports AArch64 hosts with a
  4 KB page-size kernel, hence muvm: https://www.fedoraproject.org/wiki/Changes/FEX
- muvm: https://github.com/AsahiLinux/muvm ("run programs designed for 4K-page systems on systems
  with a different page size", Asahi named as the prime example). The README does not go into how.
- FEX itself pins `FEX_PAGE_SIZE = 4096` and says it is there "to work around build systems that
  build on a 16k/64k page size" (`external/fex/FEXCore/include/FEXCore/Utils/TypeDefines.h:6-9`),
  i.e. FEX *computes* in 4 KB units but needs a 4 KB host kernel for its own mappings.
- Community packaging `wine-asahi-git` (AUR, https://aur.archlinux.org/packages/wine-asahi-git,
  description "Wine with ARM64EC support for Asahi Linux (16k pages)"): the PKGBUILD builds Wine
  10.16 + wine-staging + one patch, `2025.08.22_bylaws-wine_upstream-arm64ec_hack_v2.patch`
  (1473 lines, 39 files). Fetched and grepped: it contains no page-size, `0x4000`, `pages_vprot`
  or protection code. The "16k" in the name relies on upstream Wine's host-page-size support
  (1.2). Its dependency `fex-emu-wine-git` (https://aur.archlinux.org/packages/fex-emu-wine-git)
  builds stock FEX with no patches.
  (The spec names `fex-emu-wine`; that exact AUR name returns 404, the `-git` one exists.)

Conclusion: nothing from Asahi is portable for this problem. The reusable pieces are upstream
Wine's host-page series (already in our tree) and MR 11638's two-pass. The layer for W^X and for
fault-free guard / write watch is ours.

## 2. Gaps in the stock layer that matter here

1. **W^X.** RX + RW 4 KB parts in one host page give `PROT_READ|WRITE|EXEC`. Every x86-64 module
   in the game has this (recon: 71 modules, none with W+X *sections*, but 4 KB aligned RX/R/RW
   sections share 16 KB pages). The pinned tree refuses to load them on macOS (comment at 3284).
2. **Guard / write watch arm the host.** `get_unix_prot` turns a GUARD part into `PROT_NONE` for
   the whole host page (1384), WRITEWATCH into read-only (1390): deliberate faults.
3. **Stale data after decommit.** `decommit_pages` replaces whole host pages only (2516). A 4 KB
   page decommitted inside a host page that stays mapped keeps its bytes, and `MEM_COMMIT` later
   hands them back; Windows guarantees zero pages.
4. **Whole-view `MEM_DECOMMIT` of a view that is not a multiple of the host page** passes a 4 KB
   multiple to `anon_mmap_fixed`, which asserts host alignment (2511-2512 with 276-277).
5. **Query regions split on hidden bits.** Two uncommitted pages that remember different
   protections (VirtualProtect then MEM_DECOMMIT) are two regions in Wine and one on Windows.
6. **`force_exec_prot`** (`virtual_set_force_exec`, 5110) would put host X back on emulated pages.

## 3. Design

### 3.1 Layers

```
guest Win32 calls (4 KB semantics)
   NtAllocateVirtualMemory / NtProtectVirtualMemory / NtFreeVirtualMemory / NtQueryVirtualMemory /
   NtGetWriteWatch                                        (virtual.c, Wine's views and table)
        |  table: one byte per 4 KB page (pages_vprot), bit layout shared with the library
        v
   prot4k core (runtime/prot4k/prot4k.[ch], mirrored byte-for-byte into dlls/ntdll/unix/)
        |  union -> coalesced runs -> mprotect()
        v
   host (16 KB pages): mprotect / mmap
```

`runtime/prot4k` has two parts:
- **core** (`prot4k.h/.c`): the part byte, `p4k_part_prot` (contribution of one 4 KB part),
  `p4k_host_prot` (union of a host page), `p4k_apply_range` (runs of equal protection to the host),
  `p4k_zero_partial`, `p4k_run_length`, `p4k_vprot_from_win32` / `p4k_win32_from_vprot`. No Wine
  types, only `<stdint.h>/<string.h>`; parameterised by `host_page_size` (4 KB to 64 KB).
- **space** (`prot4k_space.h/.c`): a self-contained allocator/table (alloc, commit, protect,
  decommit, release, query, two-pass image map, write watch) built only from the core, following
  the control flow of Wine's `allocate_virtual_memory`, `set_protection`, `decommit_pages`,
  `fill_basic_memory_info`, `map_image_into_view`. It exists to make the core testable end to end
  and to serve any embedder without Wine's views. Not mirrored into Wine.

### 3.2 The rules

- **Part byte** = Wine's `VPROT_*` plus `P4K_NATIVE` (0x80, a free bit). `C_ASSERT`s in the patch
  pin the equalities.
- **Contribution of a part**: 0 if not committed; a GUARD part contributes 0 unless guards are
  soft; R from READ, R|W from WRITE or WRITECOPY, R from EXEC (the emulator reads guest code with
  ordinary loads, also for `PAGE_EXECUTE`), and X only if the policy allows it.
- **Host page = OR of its parts' contributions** ("most permissive union").
- **Policy flags** (`WINE_PROT4K`, default `ios` on `__APPLE__ && __aarch64__`, `none` elsewhere):
  `dropx` guest X is not passed to the host unless the part is `P4K_NATIVE`; `wx` never emit W|X;
  `softguard`, `softww` guard pages / write watches are bookkeeping only.
- **Empty policy = stock Wine, exactly.** `test_equals_stock_wine` transcribes
  `get_host_page_vprot`+`get_unix_prot` and compares 1.2 million random part combinations.
- **Fault-armed mode** (no `softguard`/`softww`, as on Linux or macOS CI without a debugger) keeps
  stock behaviour: a guard on any committed part makes the whole host page inaccessible, a watched
  part makes it read-only, the fault handler fixes the whole host page. The union is *not* "most
  permissive" there on purpose: a missed write would break write watch, a missed guard would break
  stack probing; over-reporting for the neighbours is the safe direction.

### 3.3 W^X, and why dropping X is sound

The host never maps W+X: `ENFORCE_WX` drops X from a host page whose union has both, and
the `conflict` hook reports it (`ERR` in Wine, at most 16 times). Dropping X for emulated pages is
justified because:
- Guest code is x86-64. FEX translates it into its own code buffer (JIT region) and only *reads* the
  guest bytes (decode, hashing for the disk cache). Executing guest bytes natively would be a bug,
  not a feature, so X on a guest page is never needed.
- FEX learns what is executable from the **guest-visible** protection (`VirtualQuery`'s
  `Protect`, `external/fex/Source/Windows/Common/InvalidationTracker.cpp:46-85`). The layer keeps
  that exact (X stays in the 4 KB table and in query results); only the host copy loses X.

What stays executable ("native"): pages of builtin views (`VPROT_SYSTEM`), of native ARM64 images
(`VPROT_NATIVE_IMAGE`, a new view flag), and pages set in the ARM64EC code bitmap
(`arm64ec_view`; includes allocations with `MEM_EXTENDED_PARAMETER_EC_CODE`). This is Windows'
own definition of "native code" in an ARM64EC process. `winebuild` gives ARM64/ARM64EC output 64 KB
section alignment (`tools/winebuild/utils.c:702-712`), and the MR comment (`virtual.c:3284-3287`)
says ARM64 binaries must be aligned to the host page, so native code should not share host pages
with writable data. I read this from source and did not inspect built DLLs. If it ever happens, the
conflict is reported and X is dropped on that page.

**Interface requirement for the JIT agent:** native executable memory must not be requested as a
plain `PAGE_EXECUTE_*` `VirtualAlloc`: that is treated as emulated and gets no host X. FEX's JIT
buffers must come from the JIT-region allocator (R-007 in `RISKS.md`), or be allocated with
`MEM_EXTENDED_PARAMETER_EC_CODE`; even then iOS forbids W+X, so the RW alias of the JIT region is
needed regardless.

### 3.4 Calls into the host

- **Allocation** (`map_view`, `create_view`): unchanged. The host mapping is the whole range rounded to
  host pages and starts RW (the pinned tree already strips X at `virtual.c:2316`); the table says
  which 4 KB parts are committed; `mprotect_range` applies the union.
- **VirtualProtect / commit** (`set_protection` -> `set_vprot` -> `mprotect_range`): one `mprotect`
  per run of host pages with equal union; a protection change of one 4 KB page is one call over its
  host page, possibly a no-op change (iOS pays the syscall; a shadow of the current host protection
  would avoid it, not implemented).
- **Two-pass image protections** (patch 0101): all sections are recorded in the table before the first
  `mprotect`, on every host, so no intermediate union is ever applied. Pass 1 is the existing RW
  mapping used for relocation (`virtual.c:2316`, relocations at 3253-3273).
- **Decommit** (patch 0105): whole host pages are replaced (zero, `PROT_NONE`, as before); the 4 KB parts
  in partially covered host pages are zeroed (host page made RW for that, never X), the table bit is
  cleared, and the final union is applied. Invariant: *every uncommitted 4 KB page reads zero*.
- **Query** (patch 0104): from the table only, never from the host. Regions are runs of pages whose
  Win32-visible state is equal; the native bit is invisible; uncommitted pages compare by
  committed-ness only. `AllocationBase`, `AllocationProtect`, `State`, `Type` come from the view as before.

### 3.5 Guard pages and write watch

- **Soft guard**: `VirtualProtect(..., PAGE_GUARD)` stores the bit; `VirtualQuery` reports
  `PAGE_xxx|PAGE_GUARD`; the host page stays accessible. Removed only by `VirtualProtect`.
- **Soft write watch**: `MEM_WRITE_WATCH` allocations are marked as before, but the per-page "clean"
  bit is never set, so `NtGetWriteWatch` (existing non-kernel branch, `virtual.c:6835-6839`) returns
  every page of the range, and reset is a no-op. Granularity stays 4096.
- Both reuse existing Wine mechanisms (`use_kernel_writewatch` already masks the bit at
  `virtual.c:1878`); the patch adds the `soft_writewatch()` test next to it.

## 4. What is not reproduced (exactly)

Everything below is about accesses that Windows would fault on and the host does not:

1. **Any access violation on a 4 KB page whose host page has a more permissive neighbour.** Examples:
   a write to a `PAGE_READONLY` page next to a `PAGE_READWRITE` one; a read of a `PAGE_NOACCESS`,
   decommitted or reserved page next to a committed one (it reads zero, see 3.4); execute
   permission is not checked at all for emulated code. Guest code that *catches* such faults (SEH
   probes, `IsBadReadPtr`-style code, copy-on-write tricks) behaves differently.
2. **One-shot `PAGE_GUARD` semantics**: no `STATUS_GUARD_PAGE_VIOLATION` on first access, and the guard
   bit is not cleared by the access (it stays until `VirtualProtect`). Guest stack growth does not
   need it: Wine commits the whole thread stack (`virtual_alloc_thread_stack`, 4520), only the bottom
   host pages are really `PROT_NONE`. Recon: zero constant `PAGE_GUARD` in the game modules.
3. **`MEM_WRITE_WATCH` / `GetWriteWatch`**: results are a superset (all pages of the range, each call);
   `WRITE_WATCH_FLAG_RESET` and `ResetWriteWatch` do nothing. A consumer that relies on getting an
   *empty* list after a reset (to know nothing changed) would loop or rescan everything. Recon: zero
   constant `MEM_WRITE_WATCH`.
4. **Self-modifying code detection through write faults.** FEX's `InvalidationTracker` write-protects
   guest RWX regions with `NtProtectVirtualMemory` and waits for the fault
   (`InvalidationTracker.cpp:239, 308, 366`). With the union, the protection only holds if all four 4 KB
   parts of the host page are non-writable; a writable neighbour defeats it silently, and so does
   iOS's no-fault rule (spec 6.8, which already says to disable fault-based SMC detection for AOT
   modules). Recon: no W+X sections; two non-constant `VirtualProtect/VirtualAlloc` sites each in
   `RocketLeague.exe` and EOSSDK. See risk R-P2.
5. **Executing a page without X**: not applicable for emulated pages; for native pages that share a host
   page with writable data X is dropped (reported by `ERR`).
6. **Stale data written into an uncommitted 4 KB page by a stray write** (the host allows it, Windows
   faults) would reappear at the next commit; the invariant covers every legitimate path only.
7. **A concurrent thread** running native code from a host page that is being partially decommitted sees the
   page RW (not X) for the duration of the zeroing. Native code does not share host pages with
   decommittable memory in practice (3.3).

## 5. Patches (`patches/wine/`)

Apply in order with `tools/apply_patches.sh`; each applies on the pinned tree plus its predecessors.
Authored in a scratch repository (`git format-patch`) and verified as below. `0100` embeds
`prot4k.h/.c` exactly; `tests/prot4k/check_wine_mirror.sh` extracts them from the patch and diffs them
against `runtime/prot4k/` (part of `make -C tests/prot4k`). After changing the library, regenerate
`0100` (copy the two files into a scratch tree at the baseline, commit, `git am` the others, then
`git format-patch`); the check fails until you do.

| Patch | Content |
|---|---|
| 0100 | `dlls/ntdll/unix/prot4k.{h,c}` + `Makefile.in` entry. No behaviour change. |
| 0101 | Policy (`WINE_PROT4K`, default per platform); `get_unix_prot` and `mprotect_range` use the library; image protections are two-pass on every host (the `__APPLE__` branch becomes the only branch; "write implies read" for sections); `force_exec_prot` does not add X under dropx; W^X conflict report. |
| 0102 | `VPROT_NATIVE` page bit, `VPROT_NATIVE_IMAGE` view flag, `update_native_bits()`; ARM64 images, builtin views and ARM64EC bitmap pages keep X. |
| 0103 | Soft guard and soft write watch. |
| 0104 | `get_vprot_range_size`: native bit invisible, uncommitted pages compare by committed-ness. |
| 0105 | Zero partially decommitted host pages; fix the whole-view decommit size. |

`WINE_PROT4K` is read once in `virtual_init`; the iOS app must `setenv` it before starting Wine if the
default is not wanted (`none,dropx,wx,softguard,softww,ios`).

## 6. Tests

`make -C tests/prot4k` builds with `clang -std=c11 -Wall -Wextra -Werror` and runs:
- `test_unit`: policy and conversions, the union table, equality with transcribed stock Wine,
  `mprotect` coalescing, 4 KB regions on a 16 KB host, decommit zero-fill, guard/write watch (soft
  and fault mode), two-pass image call sequences (never W|X), native sections and the conflict
  report, 4 KB hosts, error codes.
- `test_random`: for 4 policy configurations x host page 4/16/64 KB, thousands of random
  VirtualAlloc/Commit/Protect/Decommit/Free/Query/image/write/GetWriteWatch sequences against an
  independent model of Windows 4 KB semantics. After every operation it compares results and a full
  `VirtualQuery` walk, recomputes each host page's expected union from the model, checks that no legit
  access would fault, that contents survive, that uncommitted pages are zero, and that the mock host saw
  no unaligned/unmapped call and never W|X.
- `check_wine_mirror.sh`: the copy in patch 0100 equals the library.
- `negative_check.sh` (`make negative`): applies 16 deliberate breaks (union takes the first part /
  ANDs, W^X not enforced, guest X not dropped, native bit ignored, guards armed in soft mode, run
  coalescing broken, range too short, no zeroing, region splitting on hidden bits, old protection
  wrong, ...) to *copies* of the library and requires the tests to fail for every one.
- `win/vm4k.c`: a Windows program (no game code) that checks the same 4 KB semantics from the guest
  side, plus the protections of its own image page by page. Meant for the macOS CI `16 KB protection
  layer tests` (spec 6.10) and for any Wine build.

## 7. Verified and not verified

Verified (this Debian x86-64 host, 4 KB kernel pages):
- Library and tests (section 6): `make -C tests/prot4k` passes; `make -C tests/prot4k negative` shows all
  16 deliberate breaks failing and the pristine library passing again. The union logic was also broken
  in place once (`|=` turned into `&=`), which failed 75 unit checks and the random test, and was restored
  byte-identical.
- Patches 0100-0105 apply in sequence to the pinned tree (check-only apply, then apply, on a copy of
  `external/wine`; the touched files are byte-identical to the submodule's).
- Patched `ntdll.so` builds for x86-64 Linux with `./configure --enable-win64 --without-x ...` and a full
  `make` (no warnings in `prot4k.c` / `virtual.c`); `virtual.c` and `prot4k.c` also compile for
  aarch64-linux with `aarch64-linux-gnu-gcc` (`__aarch64__` code paths included, no warnings).
- **Real Wine with a simulated 16 KB host page.** Scratch-only hook (not a deliverable, not in the
  patches): the host page size is taken from `WINE_FAKE_HOST_PAGE=16384` on x86-64, so every
  `mmap`/`mprotect` Wine issues is 16 KB aligned, which the 4 KB kernel accepts; plus shrinking the
  x86-64 preloader's reserved areas to 16 KB, one `mmap` in `signal_x86_64.c:2814` (the syscall thunk page
  next to the shared user data) and `WINE_NO_UFFD` to switch off kernel write watches. `vm4k.exe`
  (288 checks) results:

  | ntdll | host page | result |
  |---|---|---|
  | stock MR 11638 | 4 KB | pass (control) |
  | stock MR 11638 | simulated 16 KB | **fails**: bytes written before a partial `MEM_DECOMMIT` reappear after `MEM_COMMIT` (gap 3 of section 2); a whole-view decommit of a 12 KB allocation aborts on `anon_mmap_fixed`'s alignment `assert` (gap 4) |
  | patched 0100-0105 | 4 KB | pass |
  | patched, `WINE_PROT4K=none` | simulated 16 KB | pass; write watch fault-armed (0 pages before any write, whole 16 KB host pages after) |
  | patched, `softguard,softww` | simulated 16 KB | pass; write watch conservative (8 of 8 pages from the start) |

  This exercises the two-pass image mapping of every DLL Wine loads (the mingw DLLs have 4 KB aligned
  sections, so RX / R / RW parts really share 16 KB host pages), `mprotect_range`, the new decommit path,
  query coalescing and the soft modes, in real Wine flows. It cannot exercise `dropx`/`wx` (x86-64
  Wine would stop executing its own code) or any of the Apple-only code.

Not verified: any run on Apple silicon, macOS or iOS; the `__APPLE__ && __aarch64__` default policy
(compiles only on a Mac); `dropx` with real ARM64EC modules (the native-page classification is read from
source: EC bitmap, ARM64 images, builtin views); interaction with FEX's ARM64EC emulator and with the
JIT region; `WINE_PROT4K` plumbing from the app; performance (`mprotect` count).

Side findings for other agents:
- The pinned tree predates upstream `6488dddf` "preloader: Round WINEPRELOADRESERVE values to the
  granularity boundary" and `0cd4e6c5` "Align reserved areas to the allocation granularity" (both
  2026-10-06). Reserved areas that are only 4 KB aligned trip `mmap_add_reserved_area`'s asserts
  (`virtual.c:451-452`) on a larger host page. The iOS port has no preloader, but whoever lays out the
  address space (spec 6.3, "Address space") must keep every reserved area host-page aligned.
- `signal_x86_64.c:553/2814` maps a 4 KB page right after the shared user data; irrelevant for ARM64
  hosts, listed because it is the first 4 KB-granular assumption an x86-64 build hits.

## 8. Proposed DECISIONS entries

**D-P4K-1 — The 4 KB layer extends Wine's existing host-page support; nothing is ported from Asahi.**
Asahi runs a 4 KB-page microVM and its Wine package carries no page code (study above). Upstream Wine
and MR 11638 already keep 4 KB protection bytes under larger host pages and compute the union. Our
change is `runtime/prot4k` (testable core) mirrored into `dlls/ntdll/unix/` by `patches/wine/0100-0105`,
which adds W^X handling, soft guard/write watch, zero-fill and exact query regions. With an empty
policy the library equals stock Wine's logic (tested), so the patches can be rebased on upstream.

**D-P4K-2 — Emulated pages never get host X; native code is identified by the ARM64EC bitmap, ARM64
images and builtin views.** Guest x86-64 code is only read by FEX. The guest-visible protection stays
exact; only the host copy loses X. Native executable memory must come from the JIT-region allocator or
`MEM_EXTENDED_PARAMETER_EC_CODE`, never from a plain executable `VirtualAlloc`.

**D-P4K-3 — Guard pages and write watches are software-only on iOS and macOS-with-debugger.**
Queries report `PAGE_GUARD`; the first-access exception is not raised. `GetWriteWatch` reports every
page in the range, reset is a no-op. Selected by the `softguard` / `softww` policy, default on
Apple silicon, overridable with `WINE_PROT4K`. Revisit only if a game module is found to depend on
either (recon found none).

## 9. Proposed RISKS entries

**R-P1 — A 4 KB page can be more permissive on the host than on Windows.**
Status: open, by design. Faults that Windows raises for a page sharing a host page with a more
permissive page do not happen (section 4.1). Only matters for guest code that relies on catching them.
First device run: look for `STATUS_ACCESS_VIOLATION` that did not occur where the same binary faults on
Windows; nothing to log, so the check is "game behaves". Fallback: align the offending allocation to
16 KB in the allocator hook (guest `VirtualAlloc` is already 64 KB aligned), or give the specific
region its own host pages.

**R-P2 — FEX's fault-based self-modifying-code detection cannot see writes to a 4 KB page that shares a
host page with a writable page.** Status: open. `InvalidationTracker` write-protects RWX guest regions
through `NtProtectVirtualMemory` and relies on the write fault. Recon found no RWX constants, but two
non-constant sites in `RocketLeague.exe` and EOSSDK. Spec 6.8 already disables fault-based SMC for AOT
modules. Fallback: FEX validates blocks by hash for RWX regions; the layer can expose "is this 4 KB page
write-protected on the host" if FEX needs to know (not implemented).

**R-P3 — Native code sharing a 16 KB page with writable data loses X.** Status: open, expected not to
fire. Wine's ARM64/ARM64EC modules are 64 KB section aligned (`tools/winebuild/utils.c:702`), but this
is read from source, not from built DLLs. The layer logs `ERR "W^X: native code shares host page ..."`
(first 16 times). Fallback: build the ARM64EC DLLs with `-Wl,--section-alignment,0x4000` or higher.

**R-P4 — `mprotect` count.** Status: open, unmeasured. Every `VirtualProtect` of a 4 KB page issues
`mprotect` on its host page even when the union does not change (no shadow of the host protection).
Fallback: cache the current host protection per host page and skip no-op calls.

**R-P5 — `WINE_PROT4K` default is only compiled on Apple silicon.** The `__APPLE__ && __aarch64__`
default and the `getenv` plumbing were not run on a Mac. First device/macOS CI run should show
`prot4k: host page 16k flags 0xf` in the `+virtual` trace, and `vm4k.exe` must print `vm4k: PASS`.
