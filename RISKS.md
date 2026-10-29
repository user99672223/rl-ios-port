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
