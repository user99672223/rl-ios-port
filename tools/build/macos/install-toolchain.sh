#!/usr/bin/env bash
# Download + verify + unpack the pinned llvm-mingw into $TOOLCHAIN. Idempotent.
. "$(dirname "$0")/lib.sh"

case "$(uname -s)-$(uname -m)" in
  Darwin-*)       file=$LLVM_MINGW_MACOS_FILE;         sha=$LLVM_MINGW_MACOS_SHA256 ;;
  Linux-x86_64)   file=$LLVM_MINGW_LINUX_X86_64_FILE;  sha=$LLVM_MINGW_LINUX_X86_64_SHA256 ;;
  Linux-aarch64)  file=$LLVM_MINGW_LINUX_AARCH64_FILE; sha=$LLVM_MINGW_LINUX_AARCH64_SHA256 ;;
  *) die "no pinned llvm-mingw build for $(uname -s)-$(uname -m)" ;;
esac

stamp=$TOOLCHAIN/.installed-$LLVM_MINGW_TAG
if [ -f "$stamp" ]; then log "llvm-mingw $LLVM_MINGW_TAG already installed in $TOOLCHAIN"; exit 0; fi

mkdir -p "$WORK/dl"
tarball=$WORK/dl/$file
if [ ! -f "$tarball" ]; then
  group "download $file"
  curl -fL --retry 5 --retry-delay 5 -o "$tarball.part" "$LLVM_MINGW_URL_BASE/$file"
  mv "$tarball.part" "$tarball"
  endgroup
fi
got=$(sha256_of "$tarball")
[ "$got" = "$sha" ] || die "SHA-256 mismatch for $file: got $got expected $sha"
log "SHA-256 ok: $got"

rm -rf "$TOOLCHAIN"; mkdir -p "$TOOLCHAIN"
tar -C "$TOOLCHAIN" --strip-components=1 -xf "$tarball"
touch "$stamp"
toolchain_path
for t in x86_64 aarch64 arm64ec; do
  command -v "$t-w64-mingw32-clang" >/dev/null || die "$t-w64-mingw32-clang missing from llvm-mingw $LLVM_MINGW_TAG"
done
clang --version | head -1
