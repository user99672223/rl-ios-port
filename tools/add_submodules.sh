#!/usr/bin/env bash
# One-time: register upstream projects as shallow submodules pinned to the refs below.
# Re-running is safe; existing entries are skipped. Pins are recorded by git itself.
set -euo pipefail
cd "$(dirname "$0")/.."
#     name         url                                                   ref (tag, branch, or sha)
SUBS=(
  "fex          https://github.com/FEX-Emu/FEX.git                      FEX-2610"
  "wine         https://gitlab.winehq.org/trcrsired/wine.git            de56343b358617d8319b8510356169ad96555e17"
  "dxmt         https://github.com/3Shain/dxmt.git                      v0.80"
  "sdl          https://github.com/libsdl-org/SDL.git                   release-3.4.18"
  "freetype     https://gitlab.freedesktop.org/freetype/freetype.git    VER-2-15-0"
  "gnutls       https://gitlab.com/gnutls/gnutls.git                    3.8.13"
  "nettle       https://git.lysator.liu.se/nettle/nettle.git            nettle_3.10.2_release_20250626"
  "cpython      https://github.com/python/cpython.git                   v3.14.10"
  "legendary    https://github.com/derrod/legendary.git                 0.21.1"
  "bcdec        https://github.com/iOrange/bcdec.git                    80859ed3b7afb1c527a2a99d70c61457bea72d0c"
  "astc-encoder https://github.com/ARM-software/astc-encoder.git        5.7.0"
)
for line in "${SUBS[@]}"; do
  read -r name url ref <<<"$line"
  path="external/$name"
  if git config -f .gitmodules --get "submodule.$path.url" >/dev/null 2>&1; then echo "skip $name"; continue; fi
  rm -rf "$path"; git init -q "$path"
  git -C "$path" remote add origin "$url"
  git -C "$path" fetch -q --depth 1 origin "$ref"
  git -C "$path" -c advice.detachedHead=false checkout -q FETCH_HEAD
  git submodule add -q "$url" "$path"
  git config -f .gitmodules "submodule.$path.shallow" true
  echo "$name $(git -C "$path" rev-parse HEAD) ($ref)"
done
