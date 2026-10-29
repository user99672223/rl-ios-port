#!/usr/bin/env bash
# Build tests/smoke/smoke.exe (x86_64) and the hello.exe bisection helpers with llvm-mingw.
# Output in $SMOKE_DIR: smoke.exe  hello-x86_64.exe  hello-aarch64.exe  hello-arm64ec.exe
. "$(dirname "$0")/lib.sh"
toolchain_path
mkdir -p "$SMOKE_DIR"
src=$REPO_ROOT/tests/smoke

# -fms-extensions enables clang's __try/__except used by the frame-based SEH checks in smoke.c.
x86_64-w64-mingw32-clang -O1 -g -static -fms-extensions -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type \
  -o "$SMOKE_DIR/smoke.exe" "$src/smoke.c"
for arch in x86_64 aarch64 arm64ec; do
  "$arch-w64-mingw32-clang" -O1 -static -o "$SMOKE_DIR/hello-$arch.exe" "$src/hello.c"
done
ls -l "$SMOKE_DIR"
for f in smoke hello-x86_64 hello-aarch64 hello-arm64ec; do
  printf '%s: ' "$f"; llvm-readobj --file-headers "$SMOKE_DIR/$f.exe" | grep -m1 Machine
done
