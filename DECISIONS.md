# Decisions

Choices the spec leaves open, with the reason for each. Newest last.

## D-001 — Launch with Epic's published `-noeac` option (offline play only)
**Context.** Rocket League ships Easy Anti-Cheat (`Binaries/Win64/RocketLeague_EAC.exe`,
`EasyAntiCheat/EasyAntiCheat_EOS_Setup.exe`, Legendary prereq id `EAC_Service`). On desktop
Linux it runs only through Proton's EAC runtime, a native Linux library. No such runtime can
exist inside a Wine ARM64EC process on iOS.

**Finding.** Epic's own catalog metadata for app `Sugar` (Rocket League) publishes a launch
option, which the Epic launcher offers every player:
`extraLaunchOption_001_Name = "Launch without Anti-Cheat"`, `extraLaunchOption_001_Args = "-noeac"`.

**Decision.** The port launches with that publisher-provided option, selected through
Legendary's normal launch-parameter path. No code in this repo touches, emulates, patches or
spoofs EAC. Online matchmaking stays out of scope (spec section 2). If the game refuses
to start or log in without EAC, that is recorded in `STATUS.md` and that path stops.

**Open for the owner.** This is the owner's boundary. If you don't want even the official
`-noeac` option used, remove it from `ios/legendary/launch.py`. The game will then not start on iOS.

## D-002 — Launch executable is `Launcher.exe`, not `RocketLeague.exe`
Legendary's install record for `Sugar` has `executable = Binaries/Win64/Launcher.exe`. iOS
has no `fork`/`exec`, so recon must determine what `Launcher.exe` starts (`CreateProcess`
targets, arguments). The runtime then starts the final game exe directly, in-process, with the
same arguments Legendary + Launcher would have produced. Details go in `docs/RECON_SUMMARY.md`.

## D-003 — Host is x86-64
`uname -m` = `x86_64` (Debian 13). This laptop is the native x86 reference for differential
tests; the ARM64 side runs under qemu-user locally and natively on the Linux ARM64 CI runner.

## D-004 — GitHub CLI config location
The agent's shell runs with a different `$HOME` from the owner's. Tools invoke `gh` with
`GH_CONFIG_DIR=/home/familybadgujarwilly/.config/gh` (see `tools/lib/env.sh`) so they reuse the
owner's login rather than needing a second one.

## D-005 — Start `RocketLeague.exe` directly; skip the .NET `Launcher.exe`
Recon disassembled `Launcher.exe` (a .NET assembly) into `work/launcher/` (not committed).
It holds a table of switch → exe: `-eac` → `RocketLeague_EAC.exe`, `-noeac` → `RocketLeague.exe`,
`-offline` → `RocketLeague.exe`, with `-eac` as the default. It matches the switch ignoring case,
removes it from the arguments, and starts the exe with everything else. Running it would need a .NET runtime (Wine Mono) and a second
process, neither of which fits iOS. The runtime reproduces that table's `-noeac` row:
it starts `RocketLeague.exe` in-process with Legendary's arguments minus `-noeac`.
`RocketLeague_EAC.exe`, `EasyAntiCheat/` and `Launcher.exe` are never loaded.

## D-006 — CPU feature set: FEX `HostFeatures=disableavx`
Recon: only 132 of 114,386 functions in `RocketLeague.exe` (0.12%) contain VEX/EVEX
instructions, in two clusters, with 24 `cpuid` sites. This looks like feature-gated library
code. FEX-2610 already has `CPU.HostFeatures` with `{enable,disable}avx`, so the spec's
"SSE4.2, no AVX" is FEX config, not new code. Fallback in R-003.

## D-007 — `rlaot` is built on FEX's own offline compiler and disk cache, not a new AOT
**Spec says:** `rlaot` links FEX's frontend and backend and emits `RLGameCode.framework`, a
signed Mach-O with relocatable code, an index, and miss/profile logs.

**Upstream already has this** (FEX-2610, checked in source):
- `FEXCore/Source/Interface/Core/DiskCache.{h,cpp}` caches compiled blocks in Fossilize DBs.
  The lookup key is the guest code's offset inside its module (`MakeLookupKey(…, CodeKey, …)`)
  plus an XXH128 hash of the guest bytes. Relocations are recorded per blob (RIP literals,
  RIP moves, named symbols, and named thunks by SHA-256). This is module-relative, hash-validated
  and relocatable by construction.
- `CPU.DiskCacheRODBNames` loads extra read-only DBs: a prebuilt cache we ship.
- At runtime FEX writes code maps (blocks actually executed), which is the spec's miss log.
- `Source/Tools/FEXOfflineCompiler` (`generate <codemap>`, `process-all`) maps PE images through
  Wine for ARM64EC/WoW64, applies PE relocations, compiles every code-map entry, and writes caches.
- `Source/Windows/ARM64EC/Module.cpp` already integrates the disk cache in ARM64EC mode.

**Decision (spec §3 rules 1 and 5):** `rlaot` is a thin layer over FEX:
1. `rlaot seed`: static discovery from recon (entry, exports, `.pdata`, TLS callbacks) written as
   a FEX code map, so the first run already has broad coverage.
2. `FEXOfflineCompiler generate`: produces the read-only cache DB.
3. Device runs write new code maps and their own RW cache; `loop.sh` pulls code maps, regenerates.

**Consequences, stated plainly:**
- The AOT artifact is a FEX cache DB in `Documents/`, not a signed `RLGameCode.framework`. FEX
  copies hits into the JIT region, so **the game always needs StikDebug JIT**. That was already
  the default (`allow_aot_only = false`); the `allow_aot_only = true` mode is dropped, and the
  setting is kept but reported as unsupported.
- `FEXOfflineCompiler` runs as an ARM64 Windows program under Wine. Game-derived code may not
  leave the laptop (spec §2), so cache generation cannot run on CI. It runs on Debian under
  `qemu-aarch64` (slow, local), or on the phone itself as a side effect of playing.
- The spec's native CRT replacement is mostly moot: `RocketLeague.exe` and `EOSSDK` import the
  CRT (`ucrtbase`/`vcruntime140`/`api-ms-win-crt-*`), and Wine provides those as native ARM64EC
  builtins. Only statically linked CRT in `discord_game_sdk.dll` would remain; skipped for now.

## D-008 — Patches to FEX stay local and are never sent upstream
`external/fex/CLAUDE.md` states that AI must not be used to generate code for contributions to
FEX. Everything under `patches/fex/` was written with AI help for this private port. It must
not be submitted to FEX-Emu. The same courtesy applies to the other upstreams unless their own
policy says otherwise.

## D-009 — Wine base is MR 11638 at `de56343b`, which does not cover 16 KB pages
`!11638` ("Mac: Support Apple silicon mac for running native windows on arm softwares") is
open and unmerged: one commit, 22 files, about 700 changed lines, mostly x18/TEB handling in
`dlls/ntdll/unix/signal_arm64.c` and some changes in `virtual.c`. It does not handle 16 KB pages,
so the 4 KB protection layer (spec §6.3) is ours to write.
