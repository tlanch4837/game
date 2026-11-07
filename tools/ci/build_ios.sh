#!/usr/bin/env bash
set -euo pipefail

root="$(cd -- "$(dirname -- "$0")/../.." && pwd)"
build="$root/build-ios"
artifacts="$build/artifacts"
mkdir -p "$artifacts"
# Remove prior outputs from a reused local build directory.
rm -f "$artifacts/cityengine.ipa" "$artifacts/default.metallib"

bash "$root/tools/ci/prepare_metal.sh"

cmake_version="$(cmake --version 2>/dev/null | awk 'NR == 1 {print $3}')" || cmake_version=
if [[ ! "$cmake_version" =~ ^([0-9]+)\.([0-9]+) ]] ||
   (( BASH_REMATCH[1] < 3 || (BASH_REMATCH[1] == 3 && BASH_REMATCH[2] < 28) )); then
    if brew list --versions cmake >/dev/null; then
        brew upgrade cmake
    else
        brew install cmake
    fi
    export PATH="$(brew --prefix cmake)/bin:$PATH"
fi
cmake --version

cmake -S "$root" -B "$build" -G Xcode \
    -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos \
    -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0 -DTBD_TEAM_ID=

xcodebuild -project "$build/cityengine.xcodeproj" -scheme cityengine \
    -configuration Release -sdk iphoneos -destination 'generic/platform=iOS' \
    CONFIGURATION_BUILD_DIR="$build/products" \
    CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO CODE_SIGN_IDENTITY= build

app="$build/products/cityengine.app"
test -x "$app/cityengine"
xcrun lipo -verify_arch arm64 "$app/cityengine"
plutil -lint "$app/Info.plist"
test "$(plutil -extract MinimumOSVersion raw -o - "$app/Info.plist")" = 16.0
test -s "$app/default.metallib"
cmp "$build/shaders/iphoneos-16.0/default.metallib" "$app/default.metallib"
for shader in "$root"/src/shaders/msl/*.metal; do
    cmp "$shader" "$app/msl/${shader##*/}"
done

stage="$(mktemp -d "$build/package.XXXXXX")"
trap 'rm -rf -- "$stage"' EXIT
mkdir "$stage/Payload"
ditto "$app" "$stage/Payload/cityengine.app"
if codesign --display "$stage/Payload/cityengine.app" >/dev/null 2>&1; then
    codesign --remove-signature "$stage/Payload/cityengine.app"
fi
ditto -c -k --keepParent "$stage/Payload" "$stage/cityengine.ipa"
unzip -t "$stage/cityengine.ipa"
cp "$app/default.metallib" "$artifacts/default.metallib"
mv "$stage/cityengine.ipa" "$artifacts/cityengine.ipa"
