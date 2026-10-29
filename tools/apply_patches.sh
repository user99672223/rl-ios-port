#!/usr/bin/env bash
# Apply patches/<project>/NNNN-*.patch onto external/<project>, in order.
# Idempotent: a patch that already reverse-applies is skipped. Fails on the first patch
# that neither applies nor is already applied.
#   tools/apply_patches.sh [project...]     (default: every directory under patches/)
#   tools/apply_patches.sh --check          (dry run; exit non-zero if anything would fail)
set -euo pipefail
cd "$(dirname "$0")/.."
check=0; [ "${1:-}" = "--check" ] && { check=1; shift; }
projects=("$@"); [ ${#projects[@]} -eq 0 ] && projects=($(ls patches 2>/dev/null || true))
status=0
for proj in "${projects[@]}"; do
  dir="external/$proj"
  [ -d "$dir" ] || { echo "error: $dir missing (git submodule update --init $dir)"; exit 1; }
  shopt -s nullglob; patches=(patches/"$proj"/[0-9][0-9][0-9][0-9]-*.patch); shopt -u nullglob
  for p in "${patches[@]}"; do
    rel="../../$p"
    if git -C "$dir" apply --reverse --check "$rel" 2>/dev/null; then
      echo "already  $p"
    elif git -C "$dir" apply --check "$rel" 2>/dev/null; then
      if [ $check -eq 1 ]; then echo "ok       $p"; else git -C "$dir" apply "$rel"; echo "applied  $p"; fi
    else
      echo "FAILED   $p"; status=1; [ $check -eq 1 ] || exit 1
    fi
  done
done
exit $status
