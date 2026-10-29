#!/usr/bin/env bash
# Runs the whole texture-pipeline test suite on a Linux host (Debian x86-64 tested).
#   tests/textures/run.sh
# Builds astc-encoder + rltex with clang -Wall -Wextra -Werror, creates build/venv with the
# pinned test packages, then runs: C core tests, C cache/queue tests, the Apple-branch syntax
# check, the Python decoder/ASTC cross-checks and the encode.py end-to-end tests.
# Synthetic data only; nothing is read from the game directory.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
BUILD="$ROOT/build"
mkdir -p "$BUILD/tmp"
export TMPDIR="$BUILD/tmp"

"$HERE/build.sh"

if [[ ! -x "$BUILD/venv/bin/python" ]]; then
	echo "[venv] creating $BUILD/venv"
	python3 -m venv "$BUILD/venv"
fi
if ! "$BUILD/venv/bin/python" -c "import numpy, PIL, texture2ddecoder, etcpak, ispc_texcomp" 2>/dev/null; then
	echo "[venv] installing test packages"
	"$BUILD/venv/bin/pip" install -q -r "$HERE/requirements.txt"
fi
PY="$BUILD/venv/bin/python"
export RLTEX="$BUILD/textures/rltex/rltex"
export ASTCENC="$BUILD/astcenc/Source/astcenc-avx2"

echo; echo "=== C: core (SHA-256, keys, formats, hand-built blocks, config, mapping, headers)"
"$BUILD/textures/test_core"
echo; echo "=== C: cache hit/miss, miss log, background encode queue, worker priority"
"$BUILD/textures/test_cache"
echo; echo "=== C: Apple QoS branch type-check against declaration stubs"
clang -std=c11 -Wall -Wextra -Werror -fsyntax-only -D__APPLE__ -I "$HERE/apple_stubs" \
	-I "$ROOT/runtime/textures/include" \
	"$HERE/apple_syntax_check.c"
echo "apple branch: syntax ok"
echo; echo "=== Python: decoder cross-checks and ASTC round trips"
mkdir -p "$BUILD/test-results"
"$PY" "$HERE/test_crosscheck.py" --results "$BUILD/test-results/crosscheck.tsv"
echo; echo "=== Python: tools/textures/encode.py end to end"
"$PY" "$HERE/test_encode.py"
echo; echo "=== C: tests again under ASan+UBSan and TSan"
"$HERE/sanitize.sh"
echo; echo "ALL TEXTURE TESTS PASSED"
