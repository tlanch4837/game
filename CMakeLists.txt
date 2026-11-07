# iOS build of the engine with the Metal backend (Windows keeps build.bat: MSVC, Direct3D 12 and Vulkan).
#
# On a Mac with Xcode (UNVERIFIED: written on Windows, not yet configured on a Mac):
#   cmake -S . -B build-ios -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0
#   cmake --build build-ios --config Release -- -sdk iphoneos CODE_SIGNING_ALLOWED=NO     # unsigned, for Sideloadly
# or open build-ios/cityengine.xcodeproj. Set TBD_TEAM_ID (-DTBD_TEAM_ID=XXXXXXXXXX) to sign in Xcode.
#
# The shaders are the committed src/shaders/msl/*.metal (made on Windows by msl.bat); the build compiles them into
# default.metallib and also copies the sources into the bundle (msl/) as a run-time fallback.
cmake_minimum_required(VERSION 3.28)

if(NOT APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    message(FATAL_ERROR "CMakeLists.txt builds the iOS (Metal) app; on Windows use build.bat")
endif()

set(CMAKE_OSX_SYSROOT iphoneos CACHE STRING "iOS SDK")
set(CMAKE_OSX_DEPLOYMENT_TARGET 16.0 CACHE STRING "Minimum iOS version")
project(cityengine VERSION 0.1.0 LANGUAGES C CXX OBJC OBJCXX)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS" OR NOT CMAKE_GENERATOR STREQUAL "Xcode")
    message(FATAL_ERROR "Configure with -G Xcode -DCMAKE_SYSTEM_NAME=iOS")
endif()
set(CMAKE_XCODE_GENERATE_SCHEME ON)

# Keep the custom shader commands on the same SDK as the generated project.
string(TOLOWER "${CMAKE_OSX_SYSROOT}" TBD_SYSROOT)
if(TBD_SYSROOT MATCHES "iphonesimulator")
    set(TBD_METAL_SDK iphonesimulator)
    set(TBD_METAL_TARGET "air64-apple-ios${CMAKE_OSX_DEPLOYMENT_TARGET}-simulator")
elseif(TBD_SYSROOT MATCHES "iphoneos")
    set(TBD_METAL_SDK iphoneos)
    set(TBD_METAL_TARGET "air64-apple-ios${CMAKE_OSX_DEPLOYMENT_TARGET}")
else()
    message(FATAL_ERROR "Select an iOS SDK with -DCMAKE_OSX_SYSROOT=iphoneos or iphonesimulator")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_STANDARD 20)
set(TBD_TEAM_ID "" CACHE STRING "Apple development team for code signing (empty: unsigned)")
set(TBD_BUNDLE_ID "com.ovoa.cityengine" CACHE STRING "Bundle identifier")

set(TBD_SOURCES
    src/render/renderer_metal.mm
    src/platform/apple/log_apple.cpp
    src/platform/ios/main_ios.mm
    src/platform/platform_ios.mm
)
# The game without the editor: game_host.cpp includes the game module (src/game/game_core.inl, which includes the city
# .inl files). Linked on Windows by tools/probe_game/link_game.bat with the same file list. The Win32 shell
# (src/main.cpp), the editor panels, face mocap, the Character Creator and the D3D12 / Vulkan backends stay Windows-only.
set(TBD_GAME_SOURCES
    src/game/game_host.cpp
    src/physics.cpp
    src/ragdoll.cpp
    src/ai/nav_mesh.cpp
    src/ai/npc_system.cpp
    src/script/luau_host.cpp
    src/script/api_registry.cpp
    src/script/api_builtin.cpp
    src/script/script_remote.cpp
    src/audio.cpp
)
# miniaudio's iOS backend uses AVAudioSession: compile its translation unit as Objective-C++.
set_source_files_properties(src/audio.cpp PROPERTIES LANGUAGE OBJCXX COMPILE_OPTIONS "-fobjc-arc")

# ---- Third-party code, compiled from source as build.bat does (no changes to it).
set(TBD_JOLT ${CMAKE_CURRENT_SOURCE_DIR}/third_party/JoltPhysics)
set(TBD_RECAST ${CMAKE_CURRENT_SOURCE_DIR}/third_party/recastnavigation)
set(TBD_LUAU ${CMAKE_CURRENT_SOURCE_DIR}/third_party/luau)
file(GLOB_RECURSE TBD_JOLT_SOURCES CONFIGURE_DEPENDS ${TBD_JOLT}/Jolt/*.cpp)
file(GLOB TBD_RECAST_SOURCES CONFIGURE_DEPENDS ${TBD_RECAST}/Recast/Source/*.cpp ${TBD_RECAST}/Detour/Source/*.cpp ${TBD_RECAST}/DetourCrowd/Source/*.cpp)
file(GLOB_RECURSE TBD_LUAU_SOURCES CONFIGURE_DEPENDS ${TBD_LUAU}/VM/src/*.cpp ${TBD_LUAU}/Compiler/src/*.cpp ${TBD_LUAU}/Ast/src/*.cpp
     ${TBD_LUAU}/Bytecode/src/*.cpp ${TBD_LUAU}/Config/src/*.cpp ${TBD_LUAU}/Common/src/*.cpp)
# Compiled straight into the app target: tools/ci/build_ios.sh moves CONFIGURATION_BUILD_DIR, which separate static
# library targets would then not be found in at link time.
set(TBD_THIRD_PARTY_SOURCES ${TBD_JOLT_SOURCES} ${TBD_RECAST_SOURCES} ${TBD_LUAU_SOURCES})
set_source_files_properties(${TBD_THIRD_PARTY_SOURCES} PROPERTIES COMPILE_OPTIONS "-w") # third-party warnings are not ours to fix

# ---- Metal shaders: one .air per .metal, linked into default.metallib.
file(GLOB TBD_MSL "${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/msl/*.metal")
if(NOT TBD_MSL)
    message(FATAL_ERROR "No shaders found in src/shaders/msl")
endif()
set(TBD_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders/${TBD_METAL_SDK}-${CMAKE_OSX_DEPLOYMENT_TARGET}")
set(TBD_AIR)
foreach(msl IN LISTS TBD_MSL)
    get_filename_component(stem "${msl}" NAME_WE)
    set(air "${TBD_SHADER_DIR}/${stem}.air")
    add_custom_command(OUTPUT "${air}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${TBD_SHADER_DIR}"
        COMMAND xcrun --sdk "${TBD_METAL_SDK}" metal -std=metal3.0 -target "${TBD_METAL_TARGET}" -c "${msl}" -o "${air}"
        DEPENDS "${msl}" VERBATIM)
    list(APPEND TBD_AIR "${air}")
endforeach()
set(TBD_METALLIB "${TBD_SHADER_DIR}/default.metallib")
add_custom_command(OUTPUT "${TBD_METALLIB}"
    COMMAND xcrun --sdk "${TBD_METAL_SDK}" metallib ${TBD_AIR} -o "${TBD_METALLIB}"
    DEPENDS ${TBD_AIR} VERBATIM)
set_source_files_properties("${TBD_METALLIB}" PROPERTIES GENERATED TRUE MACOSX_PACKAGE_LOCATION Resources)
# These are fallback copies. Xcode must not infer a Metal source compile step from their extension.
set_source_files_properties(${TBD_MSL} PROPERTIES MACOSX_PACKAGE_LOCATION Resources/msl XCODE_EXPLICIT_FILE_TYPE text)

add_executable(cityengine MACOSX_BUNDLE ${TBD_SOURCES} ${TBD_GAME_SOURCES} ${TBD_THIRD_PARTY_SOURCES} "${TBD_METALLIB}" ${TBD_MSL})
target_include_directories(cityengine PRIVATE src third_party/metal-cpp ${TBD_JOLT}
    ${TBD_RECAST}/Recast/Include ${TBD_RECAST}/Detour/Include ${TBD_RECAST}/DetourCrowd/Include
    ${TBD_LUAU}/VM/include ${TBD_LUAU}/Compiler/include ${TBD_LUAU}/Ast/include ${TBD_LUAU}/Common/include
    ${TBD_LUAU}/Bytecode/include ${TBD_LUAU}/Config/include ${TBD_LUAU}/VM/src ${TBD_LUAU}/Compiler/src)
target_compile_definitions(cityengine PRIVATE TBD_HAVE_METAL=1)
# metal-cpp manages its own references: no ARC in the Metal backend; the UIKit shell uses ARC.
set_source_files_properties(src/render/renderer_metal.mm PROPERTIES COMPILE_OPTIONS "-fno-objc-arc")
set_source_files_properties(src/platform/ios/main_ios.mm src/platform/platform_ios.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
target_link_libraries(cityengine PRIVATE
    "-framework Foundation" "-framework CoreGraphics" "-framework UIKit" "-framework QuartzCore" "-framework Metal"
    "-framework ImageIO" "-framework AudioToolbox" "-framework AVFoundation" "-framework CoreAudio")

set_target_properties(cityengine PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_SOURCE_DIR}/src/platform/ios/Info.plist.in
    MACOSX_BUNDLE_GUI_IDENTIFIER ${TBD_BUNDLE_ID}
    MACOSX_BUNDLE_BUNDLE_NAME "CityEngine"
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
    MACOSX_BUNDLE_BUNDLE_VERSION "1"
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${TBD_BUNDLE_ID}
    XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "${CMAKE_OSX_DEPLOYMENT_TARGET}"
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
    XCODE_ATTRIBUTE_CLANG_CXX_LANGUAGE_STANDARD "c++20"
    XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${TBD_TEAM_ID}"
)
if(TBD_TEAM_ID STREQUAL "")
    set_target_properties(cityengine PROPERTIES XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED NO XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "")
endif()
