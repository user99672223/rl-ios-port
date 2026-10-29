#!/usr/bin/env bash
# Collect Legendary and its pure-Python dependencies (plus certifi) for embedding in the
# iOS app bundle.  Runs on macOS or Linux; needs network (PyPI) and python3 >= 3.10 with venv.
#
# 1. Download the pinned wheels in requirements-ios.txt with `pip download --require-hashes`
#    (every wheel is checked against its sha256; only py3-none-any wheels are accepted).
# 2. Build the Legendary wheel from the pinned submodule (external/legendary, tag 0.21.1;
#    its build backend is uv_build, fetched by pip's build isolation).
# 3. Unpack all wheels into $OUT/app_packages with `pip install --no-index --no-deps
#    --target`.  The Xcode project adds app_packages to the app bundle and to PYTHONPATH
#    (Doc/using/ios.rst, "Adding Python to an iOS project", steps 7-8: pass the folder to
#    install_python so any .so inside is converted to frameworks).
# 4. Refuse to continue if any compiled module slipped in (iOS needs those as frameworks),
#    check the CA bundle, and smoke-import what can be imported without pycryptodomex.
#
# Usage:  tools/cpython/vendor_deps.sh
# Env:    VENDOR_OUT  output dir (default: build/ios-python-deps); PYTHON (default python3)
# Output: $OUT/wheels/*.whl, $OUT/app_packages/, $OUT/cacert.pem, $OUT/MANIFEST.sha256
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
HERE="$REPO_ROOT/tools/cpython"
OUT="${VENDOR_OUT:-$REPO_ROOT/build/ios-python-deps}"
LEGENDARY_SRC="${LEGENDARY_SRC:-$REPO_ROOT/external/legendary}"
PYTHON="${PYTHON:-python3}"

die() { echo "error: $*" >&2; exit 1; }

command -v "$PYTHON" >/dev/null || die "$PYTHON not found"
"$PYTHON" -c 'import sys; sys.exit(0 if sys.version_info >= (3, 10) else 1)' || die "need Python >= 3.10"
[ -f "$LEGENDARY_SRC/pyproject.toml" ] || die "$LEGENDARY_SRC missing (git submodule update --init external/legendary)"
grep -q '^version = "0.21.1"' "$LEGENDARY_SRC/pyproject.toml" || die "external/legendary is not the pinned 0.21.1"

rm -rf "$OUT"
mkdir -p "$OUT/wheels" "$OUT/app_packages"

# isolated tool venv so the host's site-packages cannot leak in
"$PYTHON" -m venv "$OUT/.tools"
PIP="$OUT/.tools/bin/pip"
"$PIP" --disable-pip-version-check install --quiet --upgrade pip

echo "== 1/4 pinned dependency wheels (hash-checked)"
"$PIP" --disable-pip-version-check download --quiet --require-hashes --no-deps \
  --only-binary=:all: -r "$HERE/requirements-ios.txt" -d "$OUT/wheels"
for w in "$OUT"/wheels/*.whl; do
  case "$w" in *-py3-none-any.whl|*-py2.py3-none-any.whl) ;; *) die "not a pure-Python wheel: $w";; esac
done

echo "== 2/4 Legendary wheel from $LEGENDARY_SRC"
"$PIP" --disable-pip-version-check wheel --quiet --no-deps -w "$OUT/wheels" "$LEGENDARY_SRC"
ls "$OUT"/wheels/legendary_gl-0.21.1-*.whl >/dev/null

echo "== 3/4 unpack into app_packages"
"$PIP" --disable-pip-version-check install --quiet --no-index --no-deps --no-compile \
  --find-links "$OUT/wheels" --target "$OUT/app_packages" "$OUT"/wheels/*.whl
rm -rf "$OUT/app_packages/bin"          # console scripts are useless in an app bundle

echo "== 4/4 checks"
bad="$(find "$OUT/app_packages" \( -name '*.so' -o -name '*.dylib' -o -name '*.pyd' -o -name '*.a' \) -print)"
[ -z "$bad" ] || die "compiled modules found (would need framework conversion):
$bad"
CA="$OUT/app_packages/certifi/cacert.pem"
[ -s "$CA" ] || die "certifi/cacert.pem missing"
grep -c 'BEGIN CERTIFICATE' "$CA" | xargs echo "CA certificates in bundle:"
cp "$CA" "$OUT/cacert.pem"

# Imports that do not need pycryptodomex (legendary.core does; ios/legendary/bridge.py
# supplies a placeholder when it is absent).  -I: ignore the host environment entirely.
"$OUT/.tools/bin/python" -I - "$OUT/app_packages" <<'PY'
import sys
sys.path.insert(0, sys.argv[1])
import certifi, charset_normalizer, filelock, idna, requests, urllib3
import legendary
from legendary.api.egs import EPCAPI
from importlib.metadata import version
print("requests", requests.__version__, "| legendary", legendary.__version__,
      "| dist legendary-gl", version("legendary-gl"))
assert legendary.__version__ == "0.21.1"
assert certifi.where().startswith(sys.argv[1]), certifi.where()   # loaded from app_packages
print("login url ok:", EPCAPI().get_auth_url().startswith("https://www.epicgames.com/id/login?"))
PY

( cd "$OUT/wheels" && { shasum -a 256 ./*.whl 2>/dev/null || sha256sum ./*.whl; } ) > "$OUT/MANIFEST.sha256"
rm -rf "$OUT/.tools"
echo "done:"
cat "$OUT/MANIFEST.sha256"
echo "app_packages: $OUT/app_packages ($(du -sh "$OUT/app_packages" | cut -f1))"
