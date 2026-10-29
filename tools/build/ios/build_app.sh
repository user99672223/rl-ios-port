#!/usr/bin/env bash
# Builds the RLPort app bundle directly with swiftc/clang (no Xcode project) and packages it.
#   build_app.sh device     -> out/ios/RLPortRuntime.ipa   (unsigned; the owner signs it)
#   build_app.sh simulator  -> out/ios/sim/RLPort.app      (ad-hoc signed for simctl install)
# Env: RLPORT_BUNDLE_ID (default dev.rlport.app), RLPORT_VERSION, RLPORT_BUILD, IOS_MIN (17.0)
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
KIND=${1:?device|simulator}
case "$(uname -s)" in Darwin) ;; *) echo "build_app.sh needs macOS with Xcode" >&2; exit 1 ;; esac

BUNDLE_ID=${RLPORT_BUNDLE_ID:-dev.rlport.app}
VERSION=${RLPORT_VERSION:-0.1.0}
BUILD=${RLPORT_BUILD:-$(git -C "$ROOT" rev-list --count HEAD 2>/dev/null || echo 1)}
IOS_MIN=${IOS_MIN:-17.0}
NAME=RLPort
if [ "$KIND" = device ]; then
  SDKNAME=iphoneos; TRIPLE=arm64-apple-ios$IOS_MIN; OUT="$ROOT/out/ios"
else
  SDKNAME=iphonesimulator; TRIPLE=arm64-apple-ios$IOS_MIN-simulator; OUT="$ROOT/out/ios/sim"
fi
SDK=$(xcrun --sdk "$SDKNAME" --show-sdk-path)
OBJ="$OUT/obj"; APP="$OUT/$NAME.app"
rm -rf "$OBJ" "$APP"; mkdir -p "$OBJ" "$APP"

CFLAGS=(-target "$TRIPLE" -isysroot "$SDK" -std=c11 -O2 -Wall -Wextra -Werror)
for f in runtime/jit/jit.c runtime/jit/jit_darwin.c ios/App/Sources/SimulatorJITProbe.c; do
  xcrun --sdk "$SDKNAME" clang "${CFLAGS[@]}" -c "$ROOT/$f" -o "$OBJ/$(basename "$f").o"
done

xcrun --sdk "$SDKNAME" swiftc -target "$TRIPLE" -sdk "$SDK" -O -parse-as-library \
  -module-name "$NAME" -import-objc-header "$ROOT/ios/App/Sources/Bridging.h" \
  "$ROOT"/ios/App/Sources/*.swift "$OBJ"/*.o \
  -framework CoreFoundation -framework IOKit \
  -Xlinker -rpath -Xlinker @executable_path/Frameworks \
  -o "$APP/$NAME"

# Fill the Info.plist template's build variables, then convert to binary.
sed -e "s/\$(EXECUTABLE_NAME)/$NAME/" -e "s/\$(PRODUCT_NAME)/$NAME/" -e "s/\$(PRODUCT_MODULE_NAME)/$NAME/g" \
    -e "s/\$(RLPORT_BUNDLE_ID)/$BUNDLE_ID/" -e "s/\$(MARKETING_VERSION)/$VERSION/" \
    -e "s/\$(CURRENT_PROJECT_VERSION)/$BUILD/" -e "s/\$(IPHONEOS_DEPLOYMENT_TARGET)/$IOS_MIN/" \
    "$ROOT/ios/App/Info.plist" > "$APP/Info.plist"
# shellcheck disable=SC2016  # literal "$(" on purpose
if grep -q '\$(' "$APP/Info.plist"; then echo "unfilled build variable in Info.plist" >&2; exit 1; fi
plutil -lint "$APP/Info.plist"
if [ "$KIND" = simulator ]; then
  plutil -replace CFBundleSupportedPlatforms -json '["iPhoneSimulator"]' "$APP/Info.plist"
else
  plutil -replace CFBundleSupportedPlatforms -json '["iPhoneOS"]' "$APP/Info.plist"
fi
plutil -convert binary1 "$APP/Info.plist"
cp "$ROOT/ios/App/RLPort.entitlements" "$OUT/"

if [ "$KIND" = simulator ]; then
  codesign --force --sign - --entitlements "$ROOT/ios/App/RLPort.entitlements" "$APP"
  echo "built $APP"
else
  rm -rf "$OUT/Payload" "$OUT/RLPortRuntime.ipa"
  mkdir "$OUT/Payload" && cp -R "$APP" "$OUT/Payload/"
  (cd "$OUT" && zip -qry -X RLPortRuntime.ipa Payload)
  rm -rf "$OUT/Payload"
  echo "built $OUT/RLPortRuntime.ipa (unsigned; sign with entitlements from $OUT/RLPort.entitlements)"
fi
