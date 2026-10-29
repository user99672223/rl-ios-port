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
