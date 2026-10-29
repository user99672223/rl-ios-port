#!/usr/bin/env bash
# Builds out/RLPort.ipa: the runtime IPA from the latest successful `ios` CI run on main, plus
# game-derived content injected locally (never uploaded): RLShaders.bundle when present.
# FEX caches and textures are not in the IPA; push_game.sh puts them in Documents.
#   package_ipa.sh [--runtime-ipa PATH] [--shaders DIR] [--out PATH]
set -euo pipefail
# shellcheck source=tools/lib/env.sh
. "$(dirname "$0")/lib/env.sh"

RUNTIME_IPA=""; SHADERS="$WORK_DIR/shaders/RLShaders.bundle"; OUT="$OUT_DIR/RLPort.ipa"
while [ $# -gt 0 ]; do
  case "$1" in
    --runtime-ipa) RUNTIME_IPA=$2; shift 2 ;;
    --shaders) SHADERS=$2; shift 2 ;;
    --out) OUT=$2; shift 2 ;;
    -h|--help) sed -n '2,6p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
if [ -z "$RUNTIME_IPA" ]; then
  RUN=$(gh run list -R "$GH_REPO" --workflow ios.yml --branch main --status success --limit 1 --json databaseId --jq '.[0].databaseId')
  [ -n "$RUN" ] || { echo "no successful ios run on main" >&2; exit 1; }
  echo "runtime IPA from ios run $RUN"
  gh run download "$RUN" -R "$GH_REPO" -n RLPortRuntime-ipa -D "$T/dl"
  RUNTIME_IPA="$T/dl/RLPortRuntime.ipa"
fi
[ -f "$RUNTIME_IPA" ] || { echo "missing $RUNTIME_IPA" >&2; exit 1; }

mkdir "$T/x" && (cd "$T/x" && unzip -q "$RUNTIME_IPA")
APP=$(find "$T/x/Payload" -maxdepth 1 -name '*.app' | head -1)
[ -n "$APP" ] || { echo "no .app in $RUNTIME_IPA" >&2; exit 1; }
if [ -d "$SHADERS" ]; then
  rm -rf "$APP/RLShaders.bundle"; cp -R "$SHADERS" "$APP/RLShaders.bundle"
  echo "injected $(find "$SHADERS" -type f | wc -l) shader files"
else
  echo "no shader bundle at $SHADERS (runtime translates shaders on first use)"
fi
mkdir -p "$(dirname "$OUT")"; rm -f "$OUT"
(cd "$T/x" && zip -qry -X "$OUT" Payload)
echo "wrote $OUT ($(du -h "$OUT" | cut -f1)), unsigned"
