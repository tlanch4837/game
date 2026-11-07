# CityEngine iOS build

This is the native Metal smoke test from the `ios-metal` branch of `cityengine-ios`. The iPhone app creates a UIKit scene, renders a colored triangle through the existing Metal backend, and pauses its display link while inactive. The build uses the 40 Metal sources committed under `src/shaders/msl` and targets arm64 devices running iOS 16.0 or later.

## Run in Codemagic

1. Commit the iOS changes and push the `ios-metal` branch to the repository connected in Codemagic. Add that repository as an iOS project. Codemagic reads the root `codemagic.yaml`.
2. Run **CityEngine Metal shader check** (`ios-shader-check`). It attempts every Metal source with the iPhone SDK and reports all failures together. Download `metal-check.log` or open the step log.
3. Run **CityEngine unsigned iPhone IPA** (`ios-sideload-ipa`). It builds on an M2 worker with Codemagic's latest release Xcode. This workflow uses unsigned device settings and needs no Apple credentials in Codemagic.
4. Download `cityengine.ipa`, `default.metallib`, and `xcodebuild.log` from the build artifacts. The full build and packaging output is also in the Codemagic step log. [Codemagic YAML](https://docs.codemagic.io/yaml-basic-configuration/yaml-getting-started/) · [Xcode version selection](https://docs.codemagic.io/specs/xcode-update-policy/)

For email, uncomment the `publishing.email` block under `ios-sideload-ipa` and replace the example address with your single recipient. Codemagic asks external recipients to verify the address. [Codemagic email settings](https://docs.codemagic.io/yaml-notification/email/)

## Build on a Mac

From the root of `cityengine-ios`, run:

```bash
mkdir -p build-ios/artifacts
set -o pipefail
bash tools/ci/build_ios.sh 2>&1 | tee build-ios/artifacts/xcodebuild.log
```

The helper checks CMake 3.28 or newer and the Metal toolchain, configures the Xcode project for `iphoneos`, builds the `cityengine` scheme, and finds the app at `build-ios/products/cityengine.app`. It compares the compiled library and fallback source files against the app bundle, then packages `Payload/cityengine.app` in the unsigned IPA. Use `bash tools/ci/prepare_metal.sh && bash tools/ci/check_metal.sh` for the shader-only pass. [Apple Metal toolchain installation](https://developer.apple.com/documentation/Xcode/downloading-and-installing-additional-xcode-components) · [Apple shader library build](https://developer.apple.com/documentation/metal/building-a-shader-library-by-precompiling-source-files)

### Pre-flight review

- CMake compiles each `.metal` for the configured SDK and deployment target. The shader output path includes both, and the generated `default.metallib` is a bundle resource. The original sources are explicitly typed as text and copied to the app's `msl/` resource directory for fallback compilation. Reconfigure CMake after adding or removing shader files.
- CMake enables Objective-C and Objective-C++, passes `-fno-objc-arc` to the Metal backend and `-fobjc-arc` to the UIKit shell. The Metal backend is the only translation unit that defines the Foundation, Metal and QuartzCore metal-cpp implementation macros.
- The plist declares the UIKit scene, a launch screen, landscape orientations, and the Metal device capability. Xcode derives the packaged `MinimumOSVersion` from the deployment target. The packaging step verifies it.

## Install on the iPhone 15

Connect your unlocked iPhone to the Windows PC and trust the connection. Use Sideloadly to select `cityengine.ipa`, sign it with your free Apple account, and install it. If prompted, enable **Developer Mode** in **Settings → Privacy & Security**, restart, and confirm. A free provisioning profile expires after seven days. Refresh or reinstall the app when needed. AltStore Classic also supports sideloading on Windows. [Sideloadly](https://sideloadly.io/) · [AltStore Classic on Windows](https://faq.altstore.io/altstore-classic/how-to-install-altstore-windows) · [Apple Developer Mode](https://developer.apple.com/documentation/xcode/enabling-developer-mode-on-a-device) · [Free Apple account limits](https://developer.apple.com/help/account/basics/about-your-developer-account)

Launch CityEngine, look for the colored triangle, then background and resume the app. Record your iOS version and whether the triangle reappears. If it closes or displays a blank screen, capture the first `Metal:` or `iOS:` messages and any device crash report.

## First-run risks and what to paste

| Order | Suspected failure | What to send |
| --- | --- | --- |
| 1 | SPIRV-Cross `spvDepthCast` and shadow comparison sampling | The `Compiling <file>.metal` line and the first diagnostic with its source location, caret, and any instantiation notes. |
| 2 | Argument buffer member types or IDs | The first error and notes near `[[id(...)]]` or the argument-buffer parameter. Include the filename and full diagnostic. |
| 3 | Fragment discard and return handling | The diagnostic for `discard_fragment`, a return, or a fragment output in the affected mesh, water, or probe shader. Include the filename and surrounding diagnostic lines. |
| 4 | SDK setup or Objective-C++ compilation | The Xcode and SDK versions at the top of `xcodebuild.log`, the failing command, and the first `error:` with all immediately following notes. |
| 5 | Metal library bundling or pipeline creation | The failing packaging command and the preceding Xcode output. If installation succeeded, send the first device-console line mentioning `default.metallib`, the shader function, the render pipeline, or renderer startup. |

## Verification and scope

Local checks passed for the Codemagic YAML schema, Bash syntax, plist structure and metal-cpp macro placement. With a mock compiler, the shader-only script attempted all 40 files and reported two injected failures in one run. The Apple Metal compiler, Xcode build, IPA output, signing, and iPhone launch still need their first real run. The app target currently covers renderer bring-up and procedural geometry.

For a later content build, use the cooked asset layout in `src/cooked_asset.h` and the existing LOD path in `src/mesh_lod.h`. Start with one city district, set a texture and geometry budget from `docs/city_draw_budget_plan.md`, and measure package size, GPU memory and frame time on the iPhone 15 before adding more districts.
