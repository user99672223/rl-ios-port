# Rocket League (Windows x64) → iPhone 15 — complete build spec for Claude Code

You are building, in **one continuous run**, a compatibility runtime that runs the unmodified Windows x86-64 build of Rocket League on an iPhone 15 (A16, 6 GB RAM, iOS 26), sideloaded with a free Apple ID.

Read this entire file before starting. It is the source of truth. If upstream reality contradicts it, adapt, record why in `DECISIONS.md`, and keep going.

---

## 1. The job

This is a coding job. Do **everything that can be done without my iPhone** in one go: write all code, build all of it, run every test that can run on Debian, in CI, or in the iOS Simulator, and produce a ready-to-sideload IPA. I will test on the iPhone afterward and come back with logs.

Do not stop between parts of the work. The only reasons to stop early:
- `gh` cannot be installed or authenticated (section 4, step 0).
- A hard boundary in section 2 is reached.

### Definition of done
1. `out/RLPort.ipa`: unsigned, ready for my sideloading tool, with the game-derived code and caches injected locally.
2. All source, patches, build scripts, and CI workflows committed and pushed to a private GitHub repo.
3. CI green: Linux ARM64 (FEX tests), macOS ARM64 (Wine + FEX integration on Apple's 16 KB-page OS), iOS build, iOS Simulator smoke tests.
4. Device tools ready: push game data and prefix, pull logs and captures, one-command coverage loop.
5. Docs: `USER_GUIDE.md` (exact steps for me: install, GetMoreRam, StikDebug, push data, first login, play, run the loop), `ARCHITECTURE.md`, `DECISIONS.md`, `RISKS.md`, `STATUS.md`, `docs/RECON_SUMMARY.md`.

---

## 2. Hard boundaries (never cross)

- Online matchmaking is out of scope. Do not write anything that bypasses, emulates, disables, or spoofs Easy Anti-Cheat, DRM, or ownership/entitlement checks. Auth uses Legendary's normal launch handshake with my own Epic account. If the game refuses to run for such a reason, record it in `STATUS.md` and stop work on that path.
- Never modify anything in `GAME_DIR`.
- Never commit or upload game files or anything derived from them (translated code, DXBC blobs, textures, logs containing game bytes). They live under `work/` and `out/`, both gitignored, and never leave the laptop except to the phone over USB.
- Never give time estimates.

---

## 3. Working rules

1. **Reuse first.** Use stable upstream projects. Vendor as git submodules pinned to exact commits. Our changes live as minimal patch series in `patches/<project>/NNNN-*.patch`, applied by `tools/apply_patches.sh`. Never reimplement what an upstream project already does.
2. **No invented APIs.** Before using any function, type, flag, URL, file format, or CLI option from upstream code or Apple frameworks, open the real source, header, or docs and confirm it. If you can't confirm it, find something you can.
3. **Verify by running.** Something is done only when it builds and a test or log proves it works in the closest environment available. Never mark work done from reading code alone.
4. **Device-only behavior** (StikDebug JIT, real memory limits, A16 GPU, external display, thermal): implement fully against confirmed APIs and working reference implementations, cover it in the Simulator where possible, and add clear logging so my first device run shows exactly what happened.
5. **Decide and log.** When this spec doesn't settle a choice, pick the option that maximizes reuse and correctness, record it in `DECISIONS.md`, continue.
6. **Technical blockers:** try at least two approaches, record them in `RISKS.md`, apply the fallback in section 9, continue with everything else.
7. **Track progress** in a task list and in `STATUS.md`. Commit after each verified piece of work.

---

## 4. Environment

### Step 0 — confirm GitHub access (do this first)
1. `gh --version`. If missing, install it (official GitHub CLI apt repository).
2. `gh auth status` and `gh api user --jq .login`. If not authenticated, stop and ask me to run `gh auth login`. This is the only setup step you may stop for.
3. Confirm the token can create repos and run workflows (`repo` and `workflow` scopes).
4. Create a private repo `rl-ios-port` under my account (or reuse it if it exists), set it as `origin`.

### Host and tools
- Debian laptop. Detect architecture with `uname -m`.
  - x86-64: this machine is the native reference for x86 behavior in differential tests.
  - ARM64: use an x86-64 Actions runner as the reference.
- Install what's needed: clang/LLVM (including `lld` and `ld64.lld`), mingw/llvm-mingw for Wine's PE side, Python 3 with `pefile`/LIEF, libimobiledevice + ifuse, qemu-user.
- `GAME_DIR=<PATH I WILL GIVE YOU>` (Windows install, kept updated).
- CI runners: Linux ARM64 (FEX's own tests), **macOS ARM64 (primary integration environment: same 16 KB pages, x18 behavior, W^X, and 4 GB page zero as iOS)**, macOS for the iOS build and Simulator tests.
- Phone over USB from Debian: `idevice_id`, `idevicesyslog`, `ifuse --documents <bundle-id>`.
- Sideloading and signing: my own tool. You produce unsigned IPAs only.

---

## 5. Platform facts the design must respect

| Fact | Consequence |
|---|---|
| Only signed code executes; JIT memory only via StikDebug's iOS 26 script protocol | All native code ships signed in the app. JIT follows a working reference (Flycast-iOS, DolphiniOS, MeloNX) exactly. |
| Debugger is attached while JIT is in use | Avoid deliberate faults in normal operation (section 6.8). |
| **16 KB memory pages**; Windows ABI assumes 4 KB | Software 4 KB page-protection layer (section 6.3). |
| x18 is not preserved by Apple's kernel; Windows ARM ABI uses it for the TEB | Use Wine's Apple Silicon fixes (section 6.3). |
| Low 4 GB of address space unmappable | Shared user data and TEB at OS-chosen addresses; AOT redirects constant accesses. |
| dyld shared cache occupies the region around `0x180000000`, the default x64 DLL base | Game DLLs get relocated; AOT code must be relocatable. |
| W^X: never writable and executable at once | Two-pass PE protections; JIT writes through the protocol's mapping. |
| Entitlements: `get-task-allow`, `increased-memory-limit` (via GetMoreRam); **no** `extended-virtual-addressing` | Compact address space; read budget with `os_proc_available_memory()`. |
| No `fork`/`exec` | Everything in one process. |
| A16 GPU has no BC texture support | BC → ASTC. |
| Launch watchdog kills apps with a blocked main thread | All heavy startup work off the main thread. |
| Background apps are frozen; no Metal in background | Pause cleanly. |
| External display always receives 4K60 | Render any size; compositor scales. |
| No system CA store exposed to Unix-style TLS | Ship a CA bundle. |

---

## 6. Architecture

```
RocketLeague.exe + game DLLs (x86-64, untouched, in app Documents)
        │
FEX-Emu (ARM64EC) ── AOT code (signed framework) → JIT on miss → miss log
        │                 └─ direct-bound imports, native CRT replacements
Wine (ARM64EC, Apple Silicon base, iOS port)
        │                 └─ DXMT (D3D11 → Metal)
Presenter (MetalFX spatial, always on) ── phone or external display
        │
iOS: UIKit, Metal, MetalFX, SDL3/GameController, CoreAudio
```

### 6.1 Reuse map
| Component | Upstream |
|---|---|
| x86-64 decoder, IR, ARM64 backend, JIT, TSO handling, ARM64EC emulator DLL | FEX-Emu |
| Windows APIs, loader, SEH, threads, XAudio2 (FAudio), audio/input drivers | Wine, starting from its Apple Silicon native ARM64 work (merge request 11638 and related: 16 KB pages, x18/TEB handling, W^X two-pass protections, page zero) |
| 16 KB-page ARM64EC + FEX experience | Asahi community builds (`wine-asahi-git`, `fex-emu-wine`): study their patches |
| D3D11/DXGI → Metal, DXBC → AIR | DXMT |
| Input | SDL3 (via Wine's SDL backend) |
| Fonts / TLS | FreeType / GnuTLS + CA bundle (Mozilla's, e.g. from certifi) |
| BC decode / ASTC encode | bcdec / ARM astc-encoder |
| Epic auth and launch arguments | Legendary |
| Python on iOS | CPython official iOS support (PEP 730) |
| PE parsing / object writing | pefile or LIEF / LIEF or LLVM object writer |
| Device access | libimobiledevice |

### 6.2 CPU: `rlaot` (runs on Debian)
- C++ linking FEX's frontend and backend directly; Python wrapper allowed for orchestration. No new code generator.
- `rlaot scan --game-dir DIR --out work/aot/manifest.json`: every x86-64 `.exe`/`.dll`: SHA-256, preferred base, imports, exports, `.pdata`, TLS callbacks, relocations, sections and permissions.
- `rlaot build --manifest ... --misses work/logs/cpu_miss.log --profile work/logs/cpu_profile.log --out work/aot/`
- **Discovery seeds:** entry point, exports, all `.pdata` functions, TLS callbacks, relocation targets in code, jump tables, and all miss-log entries; follow direct branches recursively.
- **Relocatable output:** every guest address the translated code produces is `module_base + offset`, fixed up when the module loads. Never assume preferred bases.
- **ARM64EC register mapping:** keep guest x86 registers in the ARM64 registers ARM64EC defines for them, so calls into Wine need no shuffling. Integrate with FEX's ARM64EC mode, not its Linux mode.
- **Direct import binding:** resolve each import call site at AOT time so that, at load, it jumps straight to Wine's ARM64EC function (through the correct ARM64EC exit thunk), skipping the import table lookup and the dispatcher.
- **Native CRT replacement:** recognize statically linked MSVC CRT functions (`memcpy`, `memmove`, `memset`, `strlen`, `strcmp`, math like `sinf`/`cosf`/`powf`/`sqrt`) by byte signatures and bind them to native ARM64 implementations with identical semantics. Only replace functions verified by differential tests.
- **Constant-address redirection:** guest loads from fixed Windows addresses (e.g. `0x7ffe0000` shared user data) are rewritten to the runtime's actual location.
- **Float exactness:** no FMA contraction, no fast-math, exact x86 SSE semantics including MXCSR rounding, denormal, and NaN behavior (FEX's semantics).
- **Memory ordering:** FEX's TSO handling; plain loads/stores only for provably thread-private accesses (stack-relative, TLS).
- **CPUID:** SSE4.2, no AVX/AVX2.
- **Profile-guided:** hot functions from `cpu_profile.log` get FEX's most aggressive safe optimization settings.
- **Index:** sorted `(module_sha256, offset) → symbol` table, symbols `rl_<hash8>_<offset>`.
- **Determinism:** ordering by module hash then offset, no timestamps or random IDs, pinned toolchain. Build twice and `cmp` as a test.
- **Output:** `RLGameCode.framework` for iOS, linked on Debian with `ld64.lld` for `arm64-apple-ios` using `-undefined dynamic_lookup`, plus a macOS variant for CI integration tests built from synthetic test programs only.
- Skip: Windows system DLLs (Wine) and `d3d11`/`dxgi` (DXMT).

### 6.3 Windows layer: Wine (ARM64EC) on iOS
- Base: Wine's Apple Silicon native ARM64 support, built ARM64EC with FEX as the emulator DLL.
- **16 KB pages:** two-pass PE section protections; a software 4 KB protection table behind `NtProtectVirtualMemory`, `NtQueryVirtualMemory`, and allocation APIs. Real 16 KB pages get the most permissive protection of their four 4 KB parts. Game code pages are never executed natively, so mixed code/data protection within one page is harmless.
- **x18 / TEB:** apply Wine's Apple Silicon fixes (refresh TEB pointer on entry paths, don't rely on x18 surviving).
- **Address space:** no low 4 GB; shared user data and TEB at OS-chosen addresses; compact, lazily committed reservations (no EVA). Document the final layout in `ARCHITECTURE.md`.
- **Single process:** `wineserver` as a thread; helper processes run in-process or disabled if the game doesn't need them.
- **Builtin DLLs from the signed app image**, never mapped executable from files.
- **Executable memory requests** from FEX's JIT route to the StikDebug-compatible JIT region allocator.
- **UIKit driver** (`wineuikit.drv`): one fullscreen window handed to the presenter.
- **Audio:** Wine's CoreAudio driver adapted to iOS (RemoteIO, AVAudioSession, route changes). XAudio2 from Wine's FAudio-based implementation.
- **Input:** Wine's SDL backend + SDL3 for iOS (GameController, GCKeyboard, GCMouse).
- **TLS:** GnuTLS with a bundled CA file configured explicitly.
- **Threads and cores:** detect Unreal Engine thread names (`SetThreadDescription` and the MSVC thread-naming exception) and set QoS: game and render threads `USER_INTERACTIVE`; workers lower.
- **Prefix:** generated on Debian (`tools/prefix/build_prefix.sh`, running Wine's prefix setup), pushed with the game data. Game at `C:\Games\RocketLeague` mapped from `Documents/game/`. Case-insensitive lookup via Wine, covered by a test.
- **Hot-call fast paths** (`QueryPerformanceCounter`, critical sections, TLS), kept semantically identical and covered by tests.

### 6.4 Runtime glue (in the app)
- Dispatcher: `(module base + offset)` → host code. At module load, verify SHA-256 against the AOT index; register only on match; mismatch → log and leave the module to the JIT.
- Miss log `Documents/logs/cpu_miss.log`: `module_name,module_sha256,offset_hex`, deduplicated, flushed periodically and on background.
- Profile log `Documents/logs/cpu_profile.log`: `module_sha256,offset_hex,count` for hot entries.
- JIT code buffer: a few MB, from the StikDebug-compatible region, allocated on first miss.
- **No interpreter.** Startup JIT check; if unavailable show "Launch through StikDebug" and do not start the game, unless `allow_aot_only = true`.

### 6.5 Graphics: DXMT + caches
- DXMT Wine build; port its Metal bridge to iOS. Inventory every Metal API and feature DXMT uses, confirm each exists on A16 (Apple GPU family 8) under iOS 26, patch around gaps.
- **Textures (hybrid):** at creation, look up `Documents/cache/textures/<sha256>.astc`. Hit → load it. Miss → decode BC to RGBA8 with bcdec and use it, save the original BC blob to `Documents/cache/textures/bc/<sha256>.bc`, log it. Background on-device ASTC encode at low QoS (efficiency cores) as a stopgap. Offline, `tools/textures/encode.py` encodes pulled blobs on Debian with astc-encoder's thorough preset and pushes them back. Block sizes: BC1 → 6×6; BC3/BC5/BC7 → 4×4; BC4 → 6×6; BC6H → ASTC HDR 4×4 or RGBA16F. All configurable.
- **Shaders (hybrid):** hash DXBC; bundled `RLShaders.bundle` hit → use; miss → translate at runtime, save `Documents/cache/shaders/dxbc/<hash>.dxbc`, log. Offline `tools/shaders/build_bundle.py` translates pulled blobs with DXMT's translator on Debian. `MTLBinaryArchive` at `Documents/cache/pipelines.archive` caches compiled pipelines.
- **Bandwidth:** memoryless storage for transient attachments never read back; render-pass merging in DXMT where verifiable.

### 6.6 Presenter, display, lifecycle
- Game renders to an internal texture at the configured render scale; presenter always applies MetalFX spatial upscaling into the active `CAMetalLayer`.
- Drawable = largest clean integer divisor of the active screen's resolution that's ≥ render resolution (4K → 1080p). Configurable.
- External display scene, role `UIWindowSceneSessionRoleExternalDisplayNonInteractive`. Connect → present there; phone shows black "Playing on external display"; idle timer off. Disconnect → back to phone, no restart.
- `CADisplayLink` pacing on the active screen; default cap 60.
- **Thermal:** watch `ProcessInfo.thermalState`; step render scale down at `.serious`/`.critical`, back up when it recovers (configurable).
- **Lifecycle:** background → pause game threads at a safe point, flush logs; foreground → resume.
- **Launch:** main thread only does UI; Wine, game load, and Legendary run on background threads.
- Presenter writes a frame-time log (`Documents/logs/frametimes.csv`).

### 6.7 Auth and launch: Legendary
- Embed CPython (official iOS build procedure; verify current steps) with Legendary and pure-Python dependencies.
- First login: WKWebView with Legendary's login URL (read it from Legendary's source); capture the authorization code from the redirect; pass to Legendary's login.
- Register the game: `legendary import` (or its API equivalent) for Rocket League at `Documents/game/`. Find the app name from Legendary's metadata, don't guess.
- Each launch: refresh session, get a fresh exchange code, build arguments with Legendary's own launch-parameter code, start `RocketLeague.exe <args>` in Wine.
- Point Python's TLS at the bundled CA file.

### 6.8 Debugger-safe operation
- Normal operation must not rely on deliberate faults: disable FEX fault-based self-modifying-code detection for AOT modules (recon confirms none); commit thread stacks fully instead of guard-page growth; no fault-based write-watch.
- Install handlers that log every fault with context to `Documents/logs/faults.log`.
- Confirm from StikDebug's source how it treats exceptions in the iOS 26 flow and document it in `ARCHITECTURE.md`.

### 6.9 Configuration
`Documents/config/port.toml`, defaults on first run, editable in a SwiftUI settings screen shown before launch:

```toml
render_scale = 0.67
drawable_mode = "auto"
max_fps = 60
thermal_scaling = true
allow_aot_only = false
astc_bc1_block = "6x6"
astc_bc3_block = "4x4"
log_level = "warn"
```

### 6.10 Packaging and device tools
- `.github/workflows/linux-arm64.yml`: FEX unit and instruction tests, our differential tests (ARM64 side).
- `.github/workflows/macos-integration.yml`: Wine (Apple Silicon base, ARM64EC) + FEX + our runtime as a macOS build; runs synthetic test exes, AOT determinism, AOT + JIT miss capture, 16 KB protection layer tests, Wine conformance subset (kernel32, ntdll, ws2_32, xinput, mmdevapi).
- `.github/workflows/ios.yml`: builds `RLPortRuntime.ipa` (no game content), plus a Simulator build; boots a Simulator, installs, runs smoke tests (Wine startup, smoke exe, D3D11 cube if the runner's Metal allows it), collects logs as artifacts.
- `tools/package_ipa.sh`: download the runtime IPA with `gh`, inject `RLGameCode.framework` and `RLShaders.bundle`, write `out/RLPort.ipa`.
- `tools/device/push_game.sh`: copy game + prebuilt prefix + texture cache to Documents via `ifuse --documents`.
- `tools/device/pull_logs.sh`: logs, DXBC blobs, BC blobs → `work/`.
- `tools/device/loop.sh`: pull → `rlaot build` → shader bundle → texture encode → push caches → package IPA.
- Info.plist: `UIFileSharingEnabled`, `LSSupportsOpeningDocumentsInPlace`, scene manifest with phone and external display roles, `UIRequiresFullScreen`, landscape, controller support, one configurable bundle ID.

---

## 7. Order of work (one continuous run)

Do these in order, without stopping between them. Each ends when its builds and tests pass.

1. **GitHub check and bootstrap** (section 4 step 0). Repo, layout, `.gitignore` (`work/`, `out/`), docs files, submodules pinned, patches apply.
2. **Recon** (`tools/recon/`), producing `work/RECON.md` and sanitized `docs/RECON_SUMMARY.md`: modules with hashes and preferred bases; imports per module; runtime-loaded DLL names; `.pdata` counts; section permissions; writable+executable sections; executable `VirtualProtect`/`VirtualAlloc` use; **4 KB-granularity reliance (`PAGE_GUARD`, `MEM_WRITE_WATCH`/`GetWriteWatch`, sub-16 KB protection changes)**; static vs imported CRT; CPUID/AVX sites; direct reads of fixed Windows addresses; thread naming method; D3D11/DXGI/D3DCompiler use; audio/input/network/EOS modules; data size. Update `RISKS.md` and `DECISIONS.md` from the findings, then continue.
3. **Differential test harness**: native x86 reference vs FEX on ARM64, comparing registers, flags, memory writes; detects an injected mismatch; FEX's tests in `linux-arm64.yml`.
4. **Wine ARM64EC + FEX on macOS ARM64 (CI)** with the Apple Silicon base and the 16 KB protection layer; synthetic smoke exe (threads, events, critical sections, mixed-case file I/O, SEH, timers, TLS, `VirtualProtect`/`VirtualQuery` at 4 KB granularity); conformance subset.
5. **`rlaot`** with relocatable output, ARM64EC mapping, direct import binding, CRT replacement (differentially tested), constant-address redirection, determinism; dispatcher, JIT fallback, miss/profile logs; tested on macOS with synthetic exes (AOT-only, AOT with removed functions caught by JIT, relocation at a non-preferred base).
6. **iOS runtime**: app shell, Wine iOS port, signed builtin DLLs, JIT region allocator and startup check, debugger-safe settings, QoS, settings UI, config, logging, CA bundle; `ios.yml` + Simulator smoke tests.
7. **Graphics**: DXMT iOS port, texture and shader pipelines, presenter, MetalFX, external display scene, thermal scaling, lifecycle; D3D11 cube test exe (BC1 + BC3 textures) in the Simulator if possible.
8. **Auth**: CPython + Legendary, WKWebView login, import, launch arguments.
9. **Game-derived build on Debian**: `rlaot build` over the real game, shader and texture tools ready, prebuilt prefix, `package_ipa.sh` → `out/RLPort.ipa`. Confirm the framework's load commands and symbols are valid for iOS with `llvm-objdump`/`llvm-otool`.
10. **Device tooling and docs**: `push_game.sh`, `pull_logs.sh`, `loop.sh`, `USER_GUIDE.md` (including a discovery-session checklist: every mode, arena, menu, settings page, replays, splitscreen), `ARCHITECTURE.md`, final `STATUS.md` listing exactly what my first device run should show in the logs.

---

## 8. Repo layout

```
external/        submodules: fex, wine, dxmt, sdl, freetype, gnutls (+deps), cpython, legendary, bcdec, astc-encoder
patches/         patch series per upstream project
aot/             rlaot
runtime/         dispatcher, logs, JIT allocator + check, 4K protection layer, config
ios/             app shell, wineuikit.drv, presenter, settings UI, Legendary bridge
tools/recon/ tools/shaders/ tools/textures/ tools/prefix/ tools/device/
tools/           apply_patches.sh, package_ipa.sh
tests/           differential tests, smoke exe, d3d11 cube
docs/            ARCHITECTURE.md, RECON_SUMMARY.md
.github/         linux-arm64.yml, macos-integration.yml, ios.yml
work/ out/       gitignored
```

---

## 9. Risks and fallbacks

| Risk | Fallback |
|---|---|
| Game relies on 4 KB-granular faults (guard pages, write-watch) | Emulate those specific regions: commit/handle guard semantics in software where the API allows; document exactly which behavior can't be reproduced |
| Wine ARM64EC + FEX on 16 KB pages | Port the relevant Asahi community patches; extend the 4 KB protection layer |
| Builtin DLLs from the signed image | Embed DLL images as read-only sections of a signed Mach-O and map from there; last resort: load through the JIT region at startup |
| FEX JIT with StikDebug's iOS 26 protocol | Reserve one JIT region at startup through the handshake; FEX allocates only from it |
| Faults routed to the debugger | Eliminate remaining deliberate faults; document any left |
| DXMT Metal features missing on A16/iOS | Patch DXMT to emulate or take a slower path |
| Game writes to its own code pages | Exclude those regions from AOT; leave them to the JIT |
| `ld64.lld`-linked framework rejected by iOS | Upload object files to a private artifact with 1-day retention, link on the macOS runner, delete after; record in `DECISIONS.md` |
| Memory over budget | Lower texture pool via game ini, smaller ASTC blocks, smaller JIT buffer, purgeable caches |
| Game requires latest version when logged in | Record it; every game update means re-running the loop |
| Simulator lacks Metal on CI runners | Skip GPU smoke tests in CI, keep CPU/Wine tests, note it in `STATUS.md` |
| Performance | Unknown by design; I measure on device using the frame-time log |

---

## 10. Start

Begin with section 4, step 0 (GitHub check), then work through section 7 to the end without stopping.
