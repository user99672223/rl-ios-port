# Architecture

Initial version. It restates the layering from `PROJECT_SPEC.md` section 6 as amended by
decisions D-001 to D-009, lists each component with the files that own it, and leaves marked
placeholders for facts only the implementing agents can supply. Anything that has not been
built or run yet is marked **planned**.

## 1. Layering

```
RocketLeague.exe + game DLLs (x86-64, untouched, in app Documents/game/)    D-005: RocketLeague.exe is
        |                                                                    started directly, in-process
FEX-Emu (ARM64EC emulator DLL) --- JIT (StikDebug-enabled executable memory)
        |        \---- FEX disk cache (read-only prebuilt DB from `rlaot` + RW DB written on device)   D-007
        |        \---- code maps written while playing (the "miss log")                                 D-007
Wine (ARM64EC, base = MR 11638 at de56343b, no 16 KB-page support)         D-009
        |        \---- 4 KB software protection layer (ours)
        |        \---- DXMT (D3D11/DXGI -> Metal)
Presenter (MetalFX spatial, always on) --- phone screen or external display scene
        |
iOS: UIKit, Metal, MetalFX, SDL3/GameController, CoreAudio
Auth/launch: embedded CPython + Legendary, `-noeac` offline launch option only       D-001
```

What changed from spec section 6, in order of the decision numbers:

| Decision | Effect on the layering |
|---|---|
| D-001 | Launch uses Epic's published "Launch without Anti-Cheat" argument (`-noeac`). Offline play only. Nothing in this repo emulates or bypasses EAC; if the game refuses to run that way the project stops (`STATUS.md`, R-001). |
| D-002 / D-005 | `Launcher.exe` (a .NET assembly) never runs. The runtime starts `RocketLeague.exe` directly with Legendary's arguments minus `-noeac`. `RocketLeague_EAC.exe`, `EasyAntiCheat/` and `Launcher.exe` are never loaded, so `tools/device/push_game.sh` does not copy them. |
| D-006 | CPU feature set is FEX config (`HostFeatures=disableavx`), not new code. |
| D-007 | There is no signed `RLGameCode.framework`. The "AOT" is FEX's own disk cache plus code maps. `rlaot` is a thin layer over `FEXOfflineCompiler`. Translated code is copied into the JIT region, so **JIT via StikDebug is always required**. |
| D-008 | Patches to FEX (and other upstreams) stay local. Never upstream them. |
| D-009 | The Wine base has no 16 KB-page handling; the 4 KB protection layer is ours to write. |

## 2. Data flow on the device

```
Documents/
  game/                    pushed by tools/device/push_game.sh (minus the EAC files)
  prefix/                  prebuilt Wine prefix (tools/prefix/build_prefix.sh), pushed
  config/port.toml         written with defaults on first run; edited in the settings screen
  cache/
    textures/<sha>.astc    pushed (offline encode) and written on device
    textures/bc/<sha>.bc   BC blobs the runtime saved on a texture-cache miss -> pulled
    shaders/dxbc/<h>.dxbc  DXBC blobs the runtime saved on a shader-bundle miss -> pulled
    pipelines.archive      MTLBinaryArchive
    fex/                   FEX_APP_CACHE_LOCATION   (TODO(lead): final path; one variable in tools/device/lib.sh)
      codemap/new/         code maps written while playing -> pulled
      codemap/ready/       aggregated code maps (FEXOfflineCompiler process-all) -> pulled
      cache/               compiled-block caches: prebuilt pushed here, device-written ones too
  logs/                    cpu_miss.log, cpu_profile.log, faults.log, frametimes.csv, ... -> pulled
```

Names and layout of the FEX entries come from FEX-2610 source (read-only checkout in `external/fex`):
`FEX::Config::GetCacheDirectory()` (`Source/Common/Config.cpp:711-716`, honours
`FEX_APP_CACHE_LOCATION`; on Windows it falls back to `%LOCALAPPDATA%\fex-emu\`),
code maps `<cache>codemap\new\<Name>-<fileid:016x>.<FILETIME>.bin`
(`Source/Windows/Common/ImageTracker.cpp:158-164`, opened at `:225`), aggregated maps `<cache>codemap/ready/<Name>-<fileid:016x>`
(`Source/Tools/FEXOfflineCompiler/Main.cpp:731,757-758`), compiled caches `<cache>cache/<Name>-<fileid:016x>-<cfgid:016x>`
(`Main.cpp:527`, `ImageTracker.cpp:238`). Code-map writing is gated on the FEX option
`ENABLECODECACHINGWIP` (`ImageTracker.cpp:154`, `FEXCore/Source/Interface/Context/Context.h:82`).

The coverage loop (`tools/device/loop.sh`) closes the circle:
play (JIT misses are recorded) -> `pull_logs.sh` -> `rlaot build` (code maps -> cache DBs)
-> shader bundle -> ASTC textures -> `push_game.sh --only textures,aot` -> `package_ipa.sh`.

## 3. Components and owner paths

| Component | What it does | Owner paths |
|---|---|---|
| FEX-Emu | x86-64 decode, IR, ARM64 backend, JIT, TSO, ARM64EC emulator DLL, disk cache, code maps, `FEXOfflineCompiler` | `external/fex` (pinned submodule), `patches/fex/` |
| `rlaot` | Seeds code maps statically (entry, exports, `.pdata`, TLS callbacks), drives `FEXOfflineCompiler`, writes the prebuilt cache; `build --codemaps ... --out work/aot` | `aot/` |
| Wine (ARM64EC) | Windows loader, SEH, threads, kernel32/ntdll, audio/input drivers; one process, `wineserver` as a thread | `external/wine`, `patches/wine/` |
| 4 KB protection layer | Software 4 KB page table behind `NtProtectVirtualMemory`, `NtQueryVirtualMemory`, allocation APIs on 16 KB pages | `runtime/` (and `patches/wine/` hooks) **planned** |
| Runtime glue | JIT-availability check and "Launch through StikDebug" gate, JIT region allocator, logging, config | `runtime/` **planned** |
| DXMT | D3D11/DXGI -> Metal, DXBC translation, MTLBinaryArchive | `external/dxmt`, `patches/dxmt/` |
| Shader pipeline | Offline `build_bundle.py`: DXBC blobs -> `RLShaders.bundle` | `tools/shaders/` **planned** |
| Texture pipeline | Offline `encode.py`: BC blobs -> ASTC (`astc-encoder`, `bcdec`) | `tools/textures/`, `external/astc-encoder`, `external/bcdec` |
| Presenter and app shell | UIKit, Metal, MetalFX, external display scene, thermal scaling, lifecycle, SwiftUI settings, `wineuikit.drv` | `ios/` (`ios/App/Info.plist` is the template written here) |
| Auth and launch | Embedded CPython, Legendary, WKWebView login, `legendary import`, launch arguments | `ios/legendary/`, `external/legendary`, `external/cpython` |
| Wine prefix | Prebuilt on Debian, pushed with the game | `tools/prefix/` **planned** |
| Recon | Static scan of the install; sanitized summary | `tools/recon/`, `docs/RECON_SUMMARY.md` |
| Differential tests | Native x86-64 expectations checked by FEX `TestHarnessRunner` | `tests/diff/`, `.github/workflows/linux-arm64.yml` |
| Device tools | Push game/caches, pull logs/blobs/code maps, coverage loop | `tools/device/` (`push_game.sh`, `pull_logs.sh`, `loop.sh`, `lib.sh`), `tests/device/run.sh` |
| Packaging | Injects caches/bundles into the runtime IPA, writes `out/RLPort.ipa` (unsigned) | `tools/package_ipa.sh` **planned** |
| CI | Linux ARM64, macOS integration, iOS build and Simulator | `.github/workflows/` |
| Docs | | `USER_GUIDE.md`, `docs/`, `DECISIONS.md`, `RISKS.md`, `STATUS.md` |

## 4. Device tools and their interfaces

All device scripts share `tools/device/lib.sh` and `tools/lib/env.sh` (`GAME_DIR`, `WORK_DIR`, `OUT_DIR`, `GH_REPO`).

- Bundle id: `RLPORT_BUNDLE_ID`, default `dev.rlport.app`. Must equal the id your signing tool gives the installed app.
- Access to the app's Documents: `ifuse <mountpoint> --documents <bundle-id>` (from `ifuse --help`); needs
  `UIFileSharingEnabled` in `Info.plist`. Always unmounted by a trap.
- Free space: `ideviceinfo -q com.apple.disk_usage -k AmountDataAvailable` (domain listed by `ideviceinfo --help`;
  key and meaning from the community lockdownd schema, hack-different/apple-knowledge `_data/lockdownd.yaml`).
  Not yet run against a phone.
- Copy: `rsync -r --size-only --inplace --partial ...` onto the ifuse mount. Resume works because files whose size already matches are skipped.
  Verified against a local directory only; **not yet verified over a real AFC mount** (e.g. whether ifuse accepts `--inplace` writes for every file size).
- Fake mode for tests: `RLPORT_FAKE_DEVICE_DIR=<dir>` replaces the mount; `tests/device/run.sh` builds a synthetic tree and checks push, pull and loop.
- Interfaces the loop calls (fixed by the project brief):
  `python3 aot/rlaot.py build --codemaps work/device/latest/codemaps --out work/aot`,
  `python3 tools/shaders/build_bundle.py --in work/device/latest/dxbc --out work/shaders`,
  `python3 tools/textures/encode.py --in work/device/latest/textures_bc --out work/textures/out`,
  `tools/device/push_game.sh --only textures,aot`, `tools/package_ipa.sh`.
  `work/device/latest/codemaps/` keeps FEX's own `new/` and `ready/` subdirectories.

## 5. Address-space layout

**TODO (owner: Wine / 4 KB-layer agent).** Fill in once the Wine iOS port runs:
which regions the process reserves and where (no low 4 GB, no `extended-virtual-addressing`),
where the TEB, PEB and shared user data (`KUSER_SHARED_DATA`, normally `0x7ffe0000`) live,
the placement of the game image and relocated DLLs relative to the dyld shared cache
(around `0x180000000`), the JIT region, thread stacks, and the 4 KB protection table. Include the numbers
logged by the first device run.

## 6. StikDebug exception behavior (debugger-safe operation)

**TODO (owner: runtime / JIT agent).** Spec section 6.8 asks for what StikDebug's iOS 26 flow does with
exceptions while the debugger is attached, from StikDebug's source. Fill in: which signals and `brk`
immediates the script handles (the universal script handles `brk #0xf00d` JIT commands and
passes other stops back to the process), whether a deliberate fault (guard page, write watch,
self-modifying-code trap) is forwarded to the app or swallowed, and what `faults.log` is expected to show.

Starting points for that agent, not yet analysed here:
`StikDebug/Scripts/universal.js` and `StikDebug/Support/ScriptStore.swift` in the StikDebug repository
(per-app script assignment by bundle id, with automatic assignment by app name). Notably, StikDebug picks a
script by app name or by a manual assignment, so the port may need to be assigned a script by hand
(unverified; see `docs/notes/device.md`).

## 7. Open items

- Final FEX cache directory on the device (`RLPORT_DEV_FEX_CACHE_DIR`, lead).
- Final executable name for `idevicesyslog -p` filtering (`RLPORT_PROCESS_NAME`, default `RLPort`).
- Whether AFC accepts what rsync does at 40 GB scale (first device run).
