#!/usr/bin/env bash
# Homebrew packages needed to configure and build Wine on a macos-15 runner.
. "$(dirname "$0")/lib.sh"
[ "$IS_DARWIN" = 1 ] || die "macOS only"
group "brew install"
# bison/flex: Apple's are too old for Wine; gettext: msgfmt for .po (optional); pkgconf: configure probes;
# ccache: compile cache for the host and PE compilers.
HOMEBREW_NO_AUTO_UPDATE=1 HOMEBREW_NO_INSTALL_CLEANUP=1 brew install --quiet bison flex pkgconf gettext ccache cmake ninja
endgroup
if [ -n "${GITHUB_PATH:-}" ]; then
  echo "$(brew --prefix bison)/bin" >> "$GITHUB_PATH"
  echo "$(brew --prefix flex)/bin" >> "$GITHUB_PATH"
  echo "$(brew --prefix gettext)/bin" >> "$GITHUB_PATH"
fi
