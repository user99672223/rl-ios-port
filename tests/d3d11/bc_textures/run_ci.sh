#!/usr/bin/env bash
# Runs bc_textures.exe under Wine on macOS against a DXMT build (CI: .github/workflows/dxmt-bc.yml).
#
#   run_ci.sh --wine DIR --dxmt DIR --exe FILE --out DIR [--astcenc FILE] [--rltex FILE]
#
#   --wine     a Wine install (bin/wine64, lib/wine/{x86_64-windows,x86_64-unix}); modified in place
#   --dxmt     meson install prefix of DXMT (x86_64-windows/*.dll, x86_64-unix/winemetal.so)
#   --exe      bc_textures.exe
#   --out      work + log directory (prefix, caches, logs)
#   --astcenc / --rltex   offline encoder tools for the cache-hit run (tools/textures/encode.py)
#
# Runs, each with its own log under <out>/logs and a verdict line in <out>/summary.txt:
#   1 native            DXMT as built, native BC on the GPU
#   2 forced-no-bc      DXMT_DEBUG_FORCE_NO_BC=1: all BC goes through the emulation, DXMT_BC_SHADOW=1
#   3 cache-populate    --flat content, emulation + DXMT_BC_CACHE_DIR: misses write .bc blobs + miss log
#   -  encode           tools/textures/encode.py turns the .bc blobs into .astc files
#   4 cache-hit         same program again: initial data must come from the ASTC files
# Exit status: number of failed runs / assertions.
set -uo pipefail

wine_dir= dxmt_dir= exe= out= astcenc= rltex_bin=
while [ $# -gt 0 ]; do
  case "$1" in
    --wine) wine_dir=$2; shift 2;;
    --dxmt) dxmt_dir=$2; shift 2;;
    --exe) exe=$2; shift 2;;
    --out) out=$2; shift 2;;
    --astcenc) astcenc=$2; shift 2;;
    --rltex) rltex_bin=$2; shift 2;;
    *) echo "unknown argument $1" >&2; exit 2;;
  esac
done
for v in wine_dir dxmt_dir exe out; do [ -n "${!v}" ] || { echo "missing --${v//_/-}" >&2; exit 2; }; done
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/../../.." && pwd)"
mkdir -p "$out/logs" "$out/dxmt-logs"
out="$(cd "$out" && pwd)"
summary="$out/summary.txt"
: > "$summary"
failures=0

say() { echo "$*" | tee -a "$summary"; }
fail() { say "FAILED: $*"; failures=$((failures + 1)); }

# macOS has no timeout(1)
with_timeout() { local secs=$1; shift; perl -e 'alarm shift; exec @ARGV' "$secs" "$@"; }

# ---- install DXMT into the Wine tree ------------------------------------------------------------
export PATH="$wine_dir/bin:$PATH"
winedll="$wine_dir/lib/wine/x86_64-windows"
wineunix="$wine_dir/lib/wine/x86_64-unix"
[ -d "$winedll" ] && [ -d "$wineunix" ] || { echo "no lib/wine/x86_64-{windows,unix} in $wine_dir" >&2; ls "$wine_dir/lib/wine" >&2; exit 2; }
say "== installing DXMT from $dxmt_dir"
ls -la "$dxmt_dir"/x86_64-windows "$dxmt_dir"/x86_64-unix | tee -a "$summary"
cp "$dxmt_dir"/x86_64-windows/*.dll "$winedll/"
cp "$dxmt_dir"/x86_64-unix/winemetal.so "$wineunix/"
otool -L "$wineunix/winemetal.so" > "$out/logs/winemetal-otool.txt" 2>&1 || true
file "$wineunix/winemetal.so" | tee -a "$summary"

export WINEPREFIX="$out/prefix"
export WINEDEBUG=-all
export WINEARCH=win64
export WINEDLLOVERRIDES="${WINEDLLOVERRIDES:-mscoree,mshtml=}"
say "== wineboot"
rm -rf "$WINEPREFIX"
with_timeout 900 wine64 wineboot -i > "$out/logs/wineboot.log" 2>&1
say "wineboot rc=$?"
wineserver -w 2>/dev/null || true

# ---- one run --------------------------------------------------------------------------------------
# run NAME "ENV=v ENV2=v2" args...  -> logs/NAME.log, returns the program's exit status
run() {
  local name=$1 envs=$2; shift 2
  mkdir -p "$out/dxmt-logs/$name"
  say "== run $name: env [$envs] args [$*]"
  # shellcheck disable=SC2086
  env DXMT_LOG_LEVEL=info DXMT_LOG_PATH="$out/dxmt-logs/$name" $envs \
    perl -e 'alarm shift; exec @ARGV' 1500 wine64 "$exe" "$@" > "$out/logs/$name.log" 2>&1
  local rc=$?
  wineserver -w 2>/dev/null || true
  grep -E "^(INFO|SUMMARY|FAIL)" "$out/logs/$name.log" | tee -a "$summary"
  say "$name: exit status $rc, $(grep -c '^PASS' "$out/logs/$name.log") PASS, $(grep -c '^FAIL' "$out/logs/$name.log") FAIL"
  return $rc
}

expect_ok() { # NAME
  local name=$1
  if ! grep -q '^SUMMARY' "$out/logs/$name.log"; then fail "$name: the test program did not reach its summary line (crash or hang), tail:"; tail -25 "$out/logs/$name.log" | tee -a "$summary"; return; fi
  [ "$2" = "0" ] || fail "$name: exit status $2"
}

# 0. diagnostics (informational, never counted): the pipeline self-test alone, with the Metal API
#    validation layer and DXMT's debug log, with and without waiting for the GPU before each readback
run diag-selftest "MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 DXMT_LOG_LEVEL=debug" --filter nomatch || true
run diag-selftest-sync "MTL_DEBUG_LAYER=1 DXMT_LOG_LEVEL=debug" --filter nomatch --sync || true

# 1. native BC on the GPU
run native "" ; expect_ok native $?

# 2. emulation forced on a device that has native BC
run forced-no-bc "DXMT_DEBUG_FORCE_NO_BC=1 DXMT_BC_SHADOW=1" ; expect_ok forced-no-bc $?
if ! grep -rqs "BC emulation active" "$out/dxmt-logs/forced-no-bc"; then
  fail "forced-no-bc: DXMT never logged 'BC emulation active' (the switch had no effect?)"
fi

# 3. cache populate: misses save .bc blobs and write the miss log
cache="$out/bccache"
rm -rf "$cache"
emul="DXMT_DEBUG_FORCE_NO_BC=1 DXMT_BC_SHADOW=1 DXMT_BC_CACHE_DIR=$cache DXMT_BC_LOG=1"
run cache-populate "$emul" --flat ; expect_ok cache-populate $?
nbc=$(find "$cache/cache/textures/bc" -name '*.bc' 2>/dev/null | wc -l | tr -d ' ')
nmiss=$(grep -c '^bc-emul: MISS' "$out/logs/cache-populate.log" || true)
say "cache-populate: $nbc .bc blobs saved, $nmiss MISS lines, miss log: $(wc -l < "$cache/logs/texture_miss.log" 2>/dev/null || echo 0) lines"
[ "${nbc:-0}" -gt 0 ] || fail "cache-populate: no .bc blob was saved"
[ "${nmiss:-0}" -gt 0 ] || fail "cache-populate: no MISS was logged"
[ -s "$cache/logs/texture_miss.log" ] || fail "cache-populate: the miss log is empty"

# encode the blobs offline
if [ -n "$astcenc" ] && [ -n "$rltex_bin" ]; then
  say "== encode.py"
  if python3 "$repo/tools/textures/encode.py" --in "$cache/cache/textures/bc" --out "$cache/cache/textures" \
       --rltex "$rltex_bin" --astcenc "$astcenc" --jobs 2 > "$out/logs/encode.log" 2>&1; then
    say "encode.py ok"
  else
    fail "encode.py exited with an error"; tail -20 "$out/logs/encode.log" | tee -a "$summary"
  fi
  nastc=$(find "$cache/cache/textures" -maxdepth 1 -name '*.astc' | wc -l | tr -d ' ')
  say "encode: $nastc .astc files"
  [ "${nastc:-0}" -gt 0 ] || fail "encode: no .astc file produced"

  # 4. the same run again: textures come from ASTC
  run cache-hit "$emul" --flat ; expect_ok cache-hit $?
  nhit=$(grep -c '^bc-emul: HIT' "$out/logs/cache-hit.log" || true)
  nmiss2=$(grep -c '^bc-emul: MISS' "$out/logs/cache-hit.log" || true)
  say "cache-hit: $nhit HIT, $nmiss2 MISS"
  [ "${nhit:-0}" -gt 0 ] || fail "cache-hit: no texture was served from the ASTC cache"
  [ "${nmiss2:-0}" -eq 0 ] || fail "cache-hit: $nmiss2 textures still missed"
else
  say "no astcenc / rltex given: skipping the encode + cache-hit runs"
fi

say "== done, $failures failure(s)"
exit "$failures"
