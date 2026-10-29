# Risks

Each entry: what could block us, approaches tried, current fallback (spec section 9).

## R-001 — Game refuses to run without EAC
**Status:** open, not tested yet. **Severity:** project-ending if it fires.
The only route is Epic's official `-noeac` option (D-001). If the game, or its login, refuses
to start in that mode, the spec's hard boundary applies: record it in `STATUS.md`, stop that
path. Cheapest early check: run `RocketLeague.exe -noeac` under plain x86-64 Wine on this laptop
(no Proton EAC runtime) before investing in the iOS stack.

## R-002 — `Launcher.exe` spawns child processes
**Status:** open. iOS has no `fork`/`exec`. Mitigation: recon identifies the child exe and
arguments; the runtime starts it directly (D-002).

## R-003 — Ungated AVX reached at runtime
**Status:** open. With `disableavx` (D-006), any AVX instruction run without a `cpuid` check
raises #UD under FEX. 13 functions in `RocketLeague.exe` contain only 1–3 VEX instructions,
which might mean code compiled for AVX that is reached unconditionally.
**Fallback:** FEX `HostFeatures=enableavx` (FEX emulates 256-bit AVX on 128-bit NEON). Faults
are logged with the guest RIP in `faults.log`, so the first device run shows if it fires.

## R-004 — Generating the AOT cache on an x86-64 laptop
**Status:** open. `FEXOfflineCompiler` is an ARM64 Windows program run under Wine (D-007), and
game-derived code cannot go to CI. Approaches: (1) aarch64 Wine + FEX under `qemu-aarch64` on
Debian; (2) let the phone build its own cache while playing (FEX RW disk cache) and only pull
code maps. Fallback: (2) only, with no prebuilt cache on the first run.

## R-005 — Unreal Engine 3 fibers
**Status:** open. `RocketLeague.exe` imports `CreateFiberEx`/`SwitchToFiber`, and EOSSDK imports
`ConvertThreadToFiber`/`CreateFiber`/`SwitchToFiber`. Fiber switches change the stack and
TEB fiber data, which interacts with the x18/TEB handling (D-009). Needs a test in the smoke exe.

## R-006 — SSE exception flags and rounding modes under FEX
**Status:** open. On FEX-2610 in the vixl simulator (x86 host), 200 differential cases showed:
(1) MXCSR sticky exception flags (bits 0-5) are never set (159/200 cases); (2) results under
non-default MXCSR rounding (RC=1/2/3) are wrong in ~45% of cases; with RC=0 only 3/45 cases
differ beyond MXCSR. Real ARM64 results from `linux-arm64.yml` decide whether this is the
simulator or FEX. Games rarely read MXCSR flags or change RC; FTZ/DAZ matter more.

## R-007 — FEX writes and executes JIT code at the same address (no W^X split)
**Status:** open. **Severity:** blocks JIT on iOS until fixed.
FEX's code buffers come from `FEXCore::Allocator::VirtualAlloc(Size, Execute=true)`, which is
`PAGE_EXECUTE_READWRITE` on Windows/ARM64EC (`FEXCore/include/FEXCore/Utils/AllocatorHooks.h:49-61`),
and `CodeBuffer` (`FEXCore/Source/Interface/Core/SharedCodeBufferManager.h`) is a single linear
buffer that the emitter writes into and executes from. iOS never allows W+X, and on TXM devices
the executable region comes from the debugger (D-010) with a separate RW alias. FEX has no
split-mapping support (no MAP_JIT, no write/exec offset anywhere in FEXCore).
**Needed:** a local FEX patch series (D-008) that allocates code buffers from `runtime/jit`
and writes through `rx + jit_rw_minus_rx()` everywhere code is emitted or patched: block
emission, block linking/backpatching, dispatcher and thunk generation, and copying disk-cache
hits (D-007). QEMU's TCG did the same (`tcg_splitwx_diff`). Branch targets and literals must
be computed from the RX address.
**Fallback:** none that preserves JIT. This is next in line after the macOS integration work.

## R-008 — rsync over AFC at 40 GB scale is unverified
Tested only against a local directory. Unknown: USB throughput, `--inplace` on multi-GB files through
ifuse, phone sleep mid-copy (the script resumes by size). Fallbacks: plain `cp -r` over ifuse, the Files app.

## R-009 — StikDebug may not auto-assign the universal script to this app
StikDebug picks its iOS 26 script per bundle id or from a fixed list of app names
(`StikDebug/Support/AutoScriptAssignments.swift`). The port isn't on the list, so the universal script
may need assigning by hand once. If it isn't, the app's prepare call fails and the app says so (D-010).

## R-010 — pycryptodomex has no iOS build
Legendary imports it at module level (`chunk.py:8`, `manifest.py:13`) but only needs it for encrypted
manifests/chunks. The bridge installs a placeholder whose `AES.new` raises "cannot decrypt". Auth, import,
and launch don't need it. Fix if ever needed: cross-build it and patch its `.so` loader for `.fwork`.

## R-011 — `import legendary.core` on iOS is unproven
It imports `multiprocessing`/`shared_memory` at module level (`downloader/mp/manager.py:9-11`), which
CPython documents as unusable on iOS. Fallback: stub those imports in the bridge; the downloader is unused.

## R-012 — Epic login inside WKWebView may be blocked
Captcha or identity-provider flows may refuse an embedded web view. Fallback: open `login_url()` in Safari
and paste the code; `complete_login()` accepts the bare code or the pasted JSON.

## R-013 — The exchange code in argv is a secret
`-AUTH_PASSWORD=` is a short-lived one-time code: fetch it right before start, never log or persist argv
(use the bridge's `redacted_argv`). Locale: on iOS `LANG` is unset, so Legendary uses `en-US` unless
`[Legendary] locale` is set.
