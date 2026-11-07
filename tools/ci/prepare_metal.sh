#!/usr/bin/env bash
# Makes `xcrun --sdk iphoneos metal` work on a CI Mac.
#
# Xcode 26 ships the Metal compiler as a separate component. On Codemagic the component that
# `xcodebuild -downloadComponent` fetches can carry a different build number from the installed Xcode
# (seen: Xcode 17F113, toolchain 17F109), and xcrun then still reports "missing Metal Toolchain" right
# after a successful download. The workaround is the documented export, edit, import sequence:
# https://support.circleci.com/hc/en-us/articles/40865586354971-Resolving-Metal-Toolchain-Execution-Missing-Error-in-Xcode-26
set -euo pipefail

xcodebuild -version
xcrun --sdk iphoneos --show-sdk-version

have_metal() { xcrun --sdk iphoneos metal --version >/dev/null 2>&1; }

if have_metal; then
    xcrun --sdk iphoneos metal --version
    exit 0
fi

echo "Metal Toolchain is missing; downloading it."
xcode_build="$(xcodebuild -version | awk '/^Build version/ {print $3}')"
export_dir="$(mktemp -d)"
xcodebuild -downloadComponent metalToolchain -exportPath "$export_dir"

bundle="$(find "$export_dir" -maxdepth 1 -name 'MetalToolchain-*.exportedBundle' | head -n 1)"
if [[ -z "$bundle" ]]; then
    echo "No exported Metal Toolchain bundle found in $export_dir:" >&2
    ls -la "$export_dir" >&2
    exit 1
fi
bundle_build="${bundle##*/MetalToolchain-}"
bundle_build="${bundle_build%.exportedBundle}"
echo "Xcode build: $xcode_build. Toolchain build: $bundle_build."

if [[ -n "$xcode_build" && "$bundle_build" != "$xcode_build" ]]; then
    echo "Builds differ; rewriting the bundle metadata to the Xcode build."
    sed -i '' -e "s/$bundle_build/$xcode_build/g" "$bundle/ExportMetadata.plist"
fi
xcodebuild -importComponent metalToolchain -importPath "$bundle"

if ! have_metal; then
    echo "xcrun still cannot run the Metal compiler after the import. Diagnostics:" >&2
    xcrun --sdk iphoneos --find metal >&2 || true
    xcrun --sdk iphoneos metal --version >&2 || true
    xcodebuild -showComponent metalToolchain >&2 || true
    exit 1
fi
xcrun --sdk iphoneos metal --version
