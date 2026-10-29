# Notes from the device-tooling agent (spec 6.10, step 10)

Proposed entries for files this agent may not edit, plus findings other agents need.

## Proposed DECISIONS.md entries

### D-0xx — Device tools use rsync over ifuse, fake-device mode for tests
`tools/device/push_game.sh` and `pull_logs.sh` mount the app's Documents with
`ifuse <mountpoint> --documents <bundle-id>` (syntax from `ifuse --help`) and copy with
`rsync -r --size-only --inplace --partial` (no `--delete`, no `-L`). Rationale: AFC timestamps
and permissions are not reliable, and a Wine prefix contains `dosdevices/z: -> /`, which following
links would turn into a copy of the laptop's filesystem. Symlinks are skipped by default
(`RLPORT_RSYNC_LINKS=1` copies them as links). `RLPORT_FAKE_DEVICE_DIR=<dir>` replaces the mount for tests.
Bundle id is `RLPORT_BUNDLE_ID` (default `dev.rlport.app`).

### D-0xx — Free-space check reads `com.apple.disk_usage` / `AmountDataAvailable`
`ideviceinfo -q com.apple.disk_usage -k AmountDataAvailable`. The domain is in `ideviceinfo --help`.
The key is documented as "number of free bytes for user storage" in the community lockdownd schema
(hack-different/apple-knowledge, `_data/lockdownd.yaml`) and read the same way by UFADE
(`lockdown.get_value("com.apple.disk_usage","AmountDataAvailable")`) and SDMMobileDevice (`kAmountDataAvailable`).
It was not run against a phone; the script fails with a clear message and offers `--skip-space-check` if the value is not numeric.

### D-0xx — Pulled FEX code maps keep FEX's `new/` and `ready/` layout
`work/device/<ts>/codemaps/{new,ready}/`. These are exactly the directories FEXOfflineCompiler reads
and writes (`Source/Tools/FEXOfflineCompiler/Main.cpp:757-758`). `rlaot build --codemaps <dir>` therefore
receives a directory with these two subdirectories.

## Proposed RISKS.md entries

### R-0xx — rsync over AFC at 40 GB scale is unverified
Tested only against a local directory. Unknown: sustained USB throughput, whether ifuse tolerates `--inplace`
for multi-GB files, behaviour when the phone locks or sleeps mid-copy (the script resumes by size, so a
partially written file is re-sent; `--append-partial` uses `--append-verify`). Fallback: copy through Finder-less
tools such as `ifuse` + `cp -r`, or the Files app; or an HTTP/Wi-Fi pull inside the app.

### R-0xx — StikDebug assigns its iOS 26 script by app name or manual assignment
From StikDebug `StikDebug/Support/AutoScriptAssignments.swift` and `ScriptStore.swift`: a script is chosen by an
explicit per-bundle-id assignment, else an automatic assignment by the app's display name from a fixed list
(for example "MeloNX", "Amethyst", "UTM"). The port is not on that list, so the owner may have to assign the
universal script to it by hand. The universal script handles `brk #0xf00d` commands 0 (detach), 1
(prepare region: `x0` address or 0 plus `x1` length, returns the address in `x0`) and 2 (new breakpoints/script),
and legacy `brk` immediates `0x68`, `0x69`. Not yet verified on iOS 26 hardware. The runtime/JIT agent should
read `universal.js` before writing the JIT region allocator (https://github.com/StikDebug/StikDebug,
`StikDebug/Scripts/universal.js`, header "last updated 2026-29-03").

## Findings

- **FEX cache location is the lead's call.** The device scripts read it from `RLPORT_DEV_FEX_CACHE_DIR`
  (default `cache/fex`, relative to Documents). Derived defaults: compiled caches `$DIR/cache`, code maps
  `$DIR/codemap/new` and `$DIR/codemap/ready`. Change one default in `tools/device/lib.sh` when the runtime
  settles. Code maps are only written when FEX's `ENABLECODECACHINGWIP` is on
  (`Source/Windows/Common/ImageTracker.cpp:154`); the runtime must turn that on or the pull will find nothing.
- **D-007 vs. the spec's miss log.** `Documents/logs/cpu_miss.log` (spec 6.4) and FEX code maps are two
  records of the same thing. `loop.sh` feeds `rlaot` the code maps, per D-007. `cpu_miss.log` is still pulled for humans.
- **Prefix symlinks.** `push_game.sh` skips symlinks, so `prefix/dosdevices/c:` and `z:` are not copied.
  The runtime (or `tools/prefix/`) must recreate them at startup, or the prefix agent should avoid
  relying on them. If AFC can create symlinks, `RLPORT_RSYNC_LINKS=1` will copy them as links (unverified).
- **Textures flat layout assumed.** `push_game.sh --only textures` copies `work/textures/out/` into `Documents/cache/textures/`
  without deleting, so the on-device `bc/` blob directory is never touched. It assumes `encode.py` writes `<sha256>.astc` files flat in `out/`.
- **`package_ipa.sh` and the loop order.** The loop pushes caches and then packages the IPA. Shader bundles
  travel inside the IPA (`RLShaders.bundle`, spec 6.5), so a shader update needs a reinstall with the new IPA.
- **Process name for `idevicesyslog -p`.** Default `RLPort`; change `RLPORT_PROCESS_NAME` if the executable is named otherwise.
- **`ifuse --list-apps` output format** was not observed (no phone). The check only looks for the bundle id as a substring.
- **Scope change received during this task:** the owner does not want StikDebug/GetMoreRam walkthroughs, so
  `USER_GUIDE.md` has one line for each and nothing more.

## Info.plist facts (Apple documentation, checked)

- `UIWindowSceneSessionRoleExternalDisplayNonInteractive`: iOS/iPadOS 16.0+, key under `UISceneConfigurations`, array of dictionaries.
- `UIWindowSceneSessionRoleApplication`: iOS 13.0+. `UISceneClassName` for both roles is `UIWindowScene`
  (the docs page for the key says to specify it for scenes meant for the app or an external display).
- `GCSupportsControllerUserInteraction`: boolean; the page lists availability as iOS 2.0+ (as published).
- `UIRequiresFullScreen`: iOS 9.0+, concerns iPad multitasking; kept because the spec asks for it, harmless on iPhone.
- Scene delegate class names in the template are placeholders (`PhoneSceneDelegate`, `ExternalDisplaySceneDelegate`)
  for the ios/ agent to rename. `CFBundleIdentifier` is `$(RLPORT_BUNDLE_ID)`.
