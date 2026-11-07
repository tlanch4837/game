#!/usr/bin/env bash
set -euo pipefail

xcodebuild -version
xcrun --sdk iphoneos --show-sdk-version
if ! xcrun --sdk iphoneos metal --version; then
    xcodebuild -downloadComponent metalToolchain
    xcrun --sdk iphoneos metal --version
fi
