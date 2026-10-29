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
