#!/usr/bin/env bash
# Negative self-check: deliberately break the library in throw-away copies (the real sources
# are never modified) and require that the tests FAIL for every break.  Then rebuild the
# untouched library and require that the tests pass again.
set -uo pipefail
cd "$(dirname "$0")"
LIB=../../runtime/prot4k
NEG=build/neg
rm -rf "$NEG"; mkdir -p "$NEG"

# Each mutant is a directory name plus a python snippet (mutate.py reads mutants.py).
python3 -I mutants.py list > "$NEG/list.txt"

fail=0
while read -r name; do
  d="$NEG/$name"; mkdir -p "$d"; cp $LIB/*.c $LIB/*.h "$d/"
  if ! python3 -I mutants.py apply "$name" "$d"; then echo "mutant $name: could not apply"; fail=1; continue; fi
  if ! make -s LIBDIR_OVERRIDE="$d" BUILD_OVERRIDE="$d/out" "$d/out/test_unit" "$d/out/test_random" >"$d/build.log" 2>&1; then
    echo "mutant $name: DID NOT BUILD (see $d/build.log)"; tail -5 "$d/build.log"; fail=1; continue
  fi
  u=0; r=0
  "$d/out/test_unit" >"$d/unit.log" 2>&1 || u=1
  "$d/out/test_random" 60 >"$d/random.log" 2>&1 || r=1
  if [ $u -eq 1 ] || [ $r -eq 1 ]; then
    first=$(grep -h -m1 -E "FAIL|FAILURE" "$d/unit.log" "$d/random.log" | head -1 | cut -c1-110)
    printf 'mutant %-30s CAUGHT  unit:%-4s random:%-4s %s\n' "$name" "$([ $u -eq 1 ] && echo fail || echo pass)" "$([ $r -eq 1 ] && echo fail || echo pass)" "$first"
  else
    printf 'mutant %-30s *** SURVIVED: tests still pass ***\n' "$name"; fail=1
  fi
done < "$NEG/list.txt"

echo "--- restore: pristine library must pass again"
make -s clean >/dev/null
make -s build/test_unit build/test_random >/dev/null 2>&1 || { echo "pristine build failed"; exit 1; }
if ./build/test_unit >/dev/null && ./build/test_random 60 >/dev/null; then echo "pristine library: tests pass"
else echo "pristine library: TESTS FAIL"; fail=1; fi
exit $fail
