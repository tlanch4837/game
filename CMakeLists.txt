# iOS build of the engine with the Metal backend (Windows keeps build.bat: MSVC, Direct3D 12 and Vulkan).
#
# On a Mac with Xcode (UNVERIFIED: written on Windows, not yet configured on a Mac):
#   cmake -S . -B build-ios -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_DEPLOYMENT_TARGET=16.0
#   cmake --build build-ios --config Release -- -sdk iphoneos CODE_SIGNING_ALLOWED=NO     # unsigned, for Sideloadly
# or open build-ios/cityengine.xcodeproj. Set TBD_TEAM_ID (-DTBD_TEAM_ID=XXXXXXXXXX) to sign in Xcode.
#
# The shaders are the committed src/shaders/msl/*.metal (made on Windows by msl.bat); the build compiles them into
# default.metallib and also copies the sources into the bundle (msl/) as a run-time fallback.
cmake_minimum_required(VERSION 3.28)

if(NOT APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    message(FATAL_ERROR "CMakeLists.txt builds the iOS (Metal) app; on Windows use build.bat")
endif()

project(cityengine VERSION 0.1.0 LANGUAGES C CXX OBJC OBJCXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_STANDARD 20)
set(TBD_TEAM_ID "" CACHE STRING "Apple development team for code signing (empty: unsigned)")
set(TBD_BUNDLE_ID "com.ovoa.cityengine" CACHE STRING "Bundle identifier")

set(TBD_SOURCES
    src/render/renderer_metal.mm
    src/platform/apple/log_apple.cpp
    src/platform/ios/main_ios.mm
)
# The rest of the engine (city, ai, physics, script, gui, ...) joins as each part is made platform-free; the Win32 shell
# (src/main.cpp) and the D3D12 / Vulkan backends stay Windows-only.

# ---- Metal shaders: one .air per .metal, linked into default.metallib.
file(GLOB TBD_MSL CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/src/shaders/msl/*.metal)
set(TBD_AIR)
foreach(msl ${TBD_MSL})
    get_filename_component(stem ${msl} NAME_WE)
    set(air ${CMAKE_BINARY_DIR}/air/${stem}.air)
    # No -sdk: inside an Xcode build xcrun follows SDKROOT, so device and simulator builds each get their own.
    add_custom_command(OUTPUT ${air}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/air
        COMMAND xcrun metal -std=metal3.0 -gline-tables-only -frecord-sources -c ${msl} -o ${air}
        DEPENDS ${msl} VERBATIM)
    list(APPEND TBD_AIR ${air})
endforeach()
set(TBD_METALLIB ${CMAKE_BINARY_DIR}/default.metallib)
add_custom_command(OUTPUT ${TBD_METALLIB}
    COMMAND xcrun metallib ${TBD_AIR} -o ${TBD_METALLIB}
    DEPENDS ${TBD_AIR} VERBATIM)
add_custom_target(tbd_shaders DEPENDS ${TBD_METALLIB})
set_source_files_properties(${TBD_METALLIB} PROPERTIES GENERATED TRUE MACOSX_PACKAGE_LOCATION Resources)
set_source_files_properties(${TBD_MSL} PROPERTIES MACOSX_PACKAGE_LOCATION Resources/msl HEADER_FILE_ONLY TRUE)

add_executable(cityengine MACOSX_BUNDLE ${TBD_SOURCES} ${TBD_METALLIB} ${TBD_MSL})
add_dependencies(cityengine tbd_shaders)
target_include_directories(cityengine PRIVATE src third_party/metal-cpp)
target_compile_definitions(cityengine PRIVATE TBD_HAVE_METAL=1)
# metal-cpp manages its own references: no ARC in the Metal backend; the UIKit shell uses ARC.
set_source_files_properties(src/render/renderer_metal.mm PROPERTIES COMPILE_OPTIONS "-fno-objc-arc")
set_source_files_properties(src/platform/ios/main_ios.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
target_link_libraries(cityengine PRIVATE "-framework Foundation" "-framework UIKit" "-framework QuartzCore" "-framework Metal")

set_target_properties(cityengine PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_SOURCE_DIR}/src/platform/ios/Info.plist.in
    MACOSX_BUNDLE_GUI_IDENTIFIER ${TBD_BUNDLE_ID}
    MACOSX_BUNDLE_BUNDLE_NAME "CityEngine"
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${TBD_BUNDLE_ID}
    XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET 16.0
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
    XCODE_ATTRIBUTE_CLANG_CXX_LANGUAGE_STANDARD "c++20"
    XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${TBD_TEAM_ID}"
)
if(TBD_TEAM_ID STREQUAL "")
    set_target_properties(cityengine PROPERTIES XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED NO XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "")
endif()
