# Status

| Step (spec §7) | State | Evidence |
|---|---|---|
| 1. GitHub check and bootstrap | done | private repo `user99672223/rl-ios-port`; 11 submodules pinned shallow; `tools/apply_patches.sh --check` passes (no patches yet) |
| 2. Recon | done | `tools/recon/recon.py` over 71 PE files; `docs/RECON_SUMMARY.md`; decisions D-005..D-009, risks R-003..R-005 |
| 3. Differential test harness | not started | |
| 4. Wine ARM64EC + FEX on macOS CI | not started | |
| 5. `rlaot` | redesigned | thin layer over FEX's disk cache and `FEXOfflineCompiler` (D-007) |
| 6. iOS runtime | not started | |
| 7. Graphics | not started | |
| 8. Auth | not started | |
| 9. Game-derived build | not started | |
| 10. Device tooling and docs | not started | |

## Game install
- Path: `/home/familybadgujarwilly/Games/Heroic/rocketleague` (read-only to this project)
- Legendary app name: `Sugar`, version `++Prime+Update60.1-CL-530866`, about 42 GB
- Engine layout: `Binaries/`, `Engine/`, `TAGame/`

## Key facts from recon
- Game exe: `RocketLeague.exe`, started directly with Epic's `-noeac` path (D-001, D-005)
- 7 non-Microsoft PE modules; the CRT is imported, not static, in the game exe and EOSSDK
- No constant `PAGE_GUARD` or `MEM_WRITE_WATCH` use in game modules, and no W+X sections
- Thread naming: the MSVC `0x406D1388` exception (2 sites in the exe); EOSSDK uses `SetThreadDescription`
- Graphics through delay-loaded `d3d11`/`dxgi`; audio `xaudio2_9redist`; input `xinput1_3`, `dinput8`
