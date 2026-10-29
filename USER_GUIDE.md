# User guide

For the owner of the iPhone 15 (iOS 26) and the Debian laptop that holds the Windows install of
Rocket League. Nothing here has been run on the phone yet; **[UNVERIFIED]** marks every step or
claim that depends on device behavior. Where a step relies on a third-party tool, the source of
the claim is cited inline.

Scope: offline play only. Online matchmaking is out of scope. The port launches through Epic's own
"Launch without Anti-Cheat" option (decision D-001) and nothing in this project emulates, bypasses
or spoofs anti-cheat. If the game refuses to start or log in that way, the project stops there and
`STATUS.md` records it.

## 1. Prerequisites

On the laptop:

- The game install at `GAME_DIR` (default in `tools/lib/env.sh`: `/home/familybadgujarwilly/Games/Heroic/rocketleague`). The scripts only read it.
- Free space for the game on the phone: about 42 GB (`STATUS.md`) plus the prefix, caches and a safety margin. `push_game.sh` checks this before copying.
- `ifuse`, `libimobiledevice` tools (`idevice_id`, `ideviceinfo`, `idevicesyslog`, `idevicepair`), `usbmuxd`, `rsync`, `shellcheck` (for tests only):
  `sudo apt-get install ifuse libimobiledevice-utils usbmuxd rsync`.
  `ifuse --documents APPID` mounts an app's Documents folder; `ifuse --list-apps` lists apps with file sharing enabled (both from `ifuse --help`, homepage <https://libimobiledevice.org>).
- A USB cable. Pair the phone once: unlock it, run `idevicepair pair` (command list from `idevicepair --help`), tap **Trust** on the phone, then check `idevice_id -l` prints a UDID.

On the phone:

- iPhone 15, iOS 26, a free Apple ID, your own signing tool.
- StikDebug and the GetMoreRam tool already set up the way you normally do (sections 3 and 4).

Python 3 is needed for the loop stages (`aot/rlaot.py`, `tools/shaders/build_bundle.py`, `tools/textures/encode.py`).

## 2. Install the IPA

`out/RLPort.ipa` is **unsigned**. Sign and install it with your own tool. Two things matter for the rest of this guide:

1. **Bundle ID.** The device scripts default to `dev.rlport.app`. If your signing tool rewrites the bundle ID (free Apple IDs often need a unique one), set it for every script:
   `export RLPORT_BUNDLE_ID=<the id your tool gave the app>`
   [UNVERIFIED: whether your tool rewrites it. `ifuse --list-apps` shows what the phone offers.]
2. **File sharing.** The app's `Info.plist` has `UIFileSharingEnabled` (Apple documents this key as letting the app share the files in its Documents folder), which is what lets `ifuse --documents` see the folder. `ios/App/Info.plist` is the template.

## 3. Increased memory limit (GetMoreRam)

Apply GetMoreRam's increased-memory-limit entitlement as you normally do.

## 4. JIT (StikDebug)

Enable JIT for the app with StikDebug as you normally do.

## 5. First launch

Open the app. If JIT is not active, it shows a **"Launch through StikDebug"** screen and does not start the game (spec 6.4; the `allow_aot_only` setting is unsupported under D-007). Enable JIT as in section 4 and open the app again. [UNVERIFIED on device.]

On first launch the app writes `Documents/config/port.toml` with defaults and shows the settings screen before launching (spec 6.9).

## 6. Push the game data

Connect and unlock the phone. Start with a dry run, which mounts the phone, prints what would be copied and how much space it needs, and changes nothing:

```
tools/device/push_game.sh --dry-run
```

Then copy for real:

```
tools/device/push_game.sh
```

What goes where (under the app's `Documents/`):

| Source | Destination |
|---|---|
| `GAME_DIR` (never written) | `game/` (skipping `Binaries/Win64/RocketLeague_EAC.exe`, `Binaries/Win64/EasyAntiCheat/`, `Binaries/Win64/Launcher.exe`; D-005) |
| `work/prefix/` | `prefix/` |
| `work/textures/out/` | `cache/textures/` |
| `work/aot/cache/` | the FEX cache directory (path owned by the runtime; see `RLPORT_DEV_FEX_CACHE_DIR` in `tools/device/lib.sh`) |

Useful options (run `tools/device/push_game.sh --help` for all):

- `--only game|prefix|textures|aot`, comma-separated: `--only textures,aot`.
- Resume: just run it again. Files whose size already matches on the phone are skipped. For a very large file cut off mid-copy, `--append-partial` resumes it by appending after checksumming what is already there.
- Change what is skipped: `RLPORT_GAME_EXCLUDES` (newline- or colon-separated paths relative to `GAME_DIR`).
- `--skip-space-check` if the free-space query fails.

How it works, so you can judge the risks: `ifuse <tmp> --documents <bundle-id>` mounts the folder, `rsync -r --size-only --inplace --partial` copies, and a trap always unmounts. Symlinks are skipped, never followed. Free space comes from `ideviceinfo -q com.apple.disk_usage -k AmountDataAvailable`.

**[UNVERIFIED on a phone]:** transfer speed, whether rsync's in-place writes behave over the real AFC mount for multi-GB files, and what `AmountDataAvailable` returns on iOS 26. All were tested only against a local directory.

If it says *no iOS device found* or *app not found*: unlock the phone, tap Trust, check `idevice_id -l`, make sure the IPA is installed, `RLPORT_BUNDLE_ID` matches, and that `ifuse --list-apps` lists the app.

## 7. First login

In the app, start the Epic login. It opens Epic's login page in an in-app web view, captures the authorization code and hands it to Legendary (spec 6.7). Log in with your own Epic account. The app then registers the game at `Documents/game/` with Legendary. [UNVERIFIED: the auth flow is not built yet.]

## 8. Play (offline only)

Start the game from the app. It launches `RocketLeague.exe` directly with the arguments Legendary builds, including Epic's own "Launch without Anti-Cheat" option (D-001, D-005). Use **offline modes only**: Exhibition, Season, offline Tournaments, Training, Freeplay, Replays, Splitscreen. Online matchmaking will not work and is not a goal.

If the game quits or refuses to log in without anti-cheat, stop. Record it in `STATUS.md` (risk R-001). Do not work around it.

First runs are slow: the game starts with no translated code cached, so everything is compiled by the JIT as it is hit. Speed improves as the loop (section 11) feeds caches back.

## 9. External display

Plug in an external display (or AirPlay to one). The app shows the game there through a non-interactive external-display scene (`UIWindowSceneSessionRoleExternalDisplayNonInteractive`, iOS 16+ per Apple's documentation) and the phone shows a black "Playing on external display" screen. Unplug and the game returns to the phone without restarting (spec 6.6). Controllers are used for input. [UNVERIFIED on device.]

## 10. Settings (`Documents/config/port.toml`)

Edited in the settings screen, or by hand in the Files app. Defaults (spec 6.9):

| Key | Default | Meaning |
|---|---|---|
| `render_scale` | `0.67` | Fraction of the screen resolution the game renders at; MetalFX upscales it |
| `drawable_mode` | `"auto"` | Drawable size: the largest clean integer divisor of the screen resolution that is not below the render resolution |
| `max_fps` | `60` | Frame-rate cap |
| `thermal_scaling` | `true` | Step the render scale down when the phone gets hot and back up when it cools |
| `allow_aot_only` | `false` | **Unsupported under D-007**: translated code always needs JIT |
| `astc_bc1_block` | `"6x6"` | ASTC block size for BC1 textures |
| `astc_bc3_block` | `"4x4"` | ASTC block size for BC3 (and BC5/BC7) textures |
| `log_level` | `"warn"` | Logging verbosity |

Texture block-size changes only apply to textures encoded after the change (re-run the loop). [UNVERIFIED: the settings screen and parser are not built yet.]

## 11. The coverage loop

After a play session, with the phone connected:

```
tools/device/loop.sh
```

It runs, in order, and writes a timestamped log to `work/logs/loop-<UTC timestamp>.log`:

1. `tools/device/pull_logs.sh` - logs, DXBC blobs, BC blobs and FEX code maps into `work/device/<UTC timestamp>/`, and `work/device/latest` points at it.
2. `python3 aot/rlaot.py build --codemaps work/device/latest/codemaps --out work/aot`
3. `python3 tools/shaders/build_bundle.py --in work/device/latest/dxbc --out work/shaders`
4. `python3 tools/textures/encode.py --in work/device/latest/textures_bc --out work/textures/out`
5. `tools/device/push_game.sh --only textures,aot`
6. `tools/package_ipa.sh`

A stage whose tool does not exist yet, or whose input is empty, is skipped with a message. Skip stages yourself with `--skip-pull`, `--skip-aot`, `--skip-shaders`, `--skip-textures`, `--skip-push`, `--skip-package` (or `--skip aot,package`). `--dry-run` prints the plan.

Shader bundles ship inside the IPA, so after a loop that produced new shaders, install the newly packaged IPA again. Texture and CPU caches reach the phone through step 5.

To pull without the rest: `tools/device/pull_logs.sh`; add `--syslog 60` to capture 60 seconds of `idevicesyslog` first, filtered to the app's process name (`RLPORT_PROCESS_NAME`, default `RLPort`; `-p`, `-o` options from `idevicesyslog --help`). Start the game during that window.

## 12. Discovery-session checklist

Purpose: exercise code paths so the JIT-miss records (FEX code maps, `cpu_miss.log`), the DXBC blobs and the BC texture blobs fill up. The more of the game you touch before the loop, the less the JIT, shader translator and runtime texture decode have to do later. Run each item long enough to see it animate. Pull and run the loop after each batch. Menu and mode names are from memory of the game and may differ in your version; use what the game shows. [UNVERIFIED]

- **Offline modes**: Exhibition in every playlist type available offline (Duel, Doubles, Standard, Chaos), Extra Modes (Hoops, Rumble, Dropshot, Snow Day, Heatseeker, Gridiron if present), Season (start one, play a few matches, view standings), offline Tournaments, mutators and custom rules in Exhibition (every Mutator category at least once, for example ball, gravity, boost, demolish).
- **Every arena**: for each arena in the Exhibition arena list, play a kickoff, a goal, a boost pickup, a demolition and a goal replay. Include each day, night and weather variant the list offers.
- **Every menu**: Main menu, Play, Training, Garage (open every category and preview items so car and cosmetic textures decode), Item Shop and Blueprints screens if reachable offline, Profile, Stats, Challenges, Store, Replays, Settings.
- **Every settings page**: Gameplay, Camera, Controls (rebind a button), Video (change resolution/quality and apply), Audio, Chat, Interface. Toggle each option once to reach its code.
- **Training**: Tutorial, Rookie to Rising Star training packs, Custom Training (open the editor, make and save a shot), Freeplay, any workshop or practice map offered.
- **Replays**: save a match replay, open Replays, watch one, switch camera modes, use the timeline, export or save a highlight.
- **Splitscreen**: two local players (second controller) in Exhibition; four if the device and controllers allow.
- **Input**: a controller (the app declares game controller support), touch/keyboard if connected, hot-plug a controller during a match.
- **Audio**: play a match with music, commentary and crowd; change audio settings mid-match.
- **Lifecycle**: background the app mid-match and return, lock the phone and return, connect and disconnect an external display, let the phone get warm to trigger thermal scaling.
- **Content depth**: unlock every cosmetic type you can offline: goal explosions, boost trails, wheels, toppers, decals, player banners, avatar borders.

After the session: `tools/device/loop.sh`. Repeat until a full pass produces no new misses (`cpu_miss.log` stops growing, no new code maps, no new DXBC/BC blobs).

## 13. Troubleshooting: which log answers what

All device logs are in `Documents/logs/` on the phone and in `work/device/latest/logs/` after a pull. [UNVERIFIED: file formats until the runtime exists; formats are from spec 6.4/6.6.]

| Question | Where to look |
|---|---|
| The app shows "Launch through StikDebug" | JIT is not active. Section 4. |
| Did the game start, and how far did it get? | `idevicesyslog` capture (`pull_logs.sh --syslog 60`), then `logs/` startup output |
| Which guest code ran without a prebuilt translation? | FEX code maps in `work/device/latest/codemaps/` (these drive the next cache build, D-007) and `cpu_miss.log` (`module_name,module_sha256,offset_hex`, deduplicated) |
| What code is hot? | `cpu_profile.log` (`module_sha256,offset_hex,count`) |
| Did anything crash or fault, and where? | `faults.log` (every fault with context, including the guest RIP; empty is good). Read it first if non-empty; `pull_logs.sh` warns |
| Is it slow, and in which frames? | `frametimes.csv` (written by the presenter). Compare with `render_scale` and thermal state |
| Which shaders were translated at runtime? | `work/device/latest/dxbc/` (one `.dxbc` per miss; count is the backlog) |
| Which textures were decoded at runtime? | `work/device/latest/textures_bc/` (one `.bc` per miss) |
| Push says no device or no app | Section 6, last paragraph |
| Push says not enough space | The message shows needed vs free; free space on the phone or lower `RLPORT_MIN_FREE_MARGIN_MB` |
| `ifuse` mount left behind after a crash | `fusermount -u <mountpoint>` (the scripts also unmount on exit and on Ctrl-C) |
| The loop skipped a stage | It prints why: tool missing or no input. See section 11 |

## 14. Testing the tools without a phone

```
tests/device/run.sh
```

Builds a tiny synthetic game tree in a temp directory (never the real game), uses a local directory in place of the phone (`RLPORT_FAKE_DEVICE_DIR`), and checks push, resume, excludes, pull, the loop with stub stage tools, `Info.plist` and shellcheck.
