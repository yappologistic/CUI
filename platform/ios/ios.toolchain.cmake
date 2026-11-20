# Toolchain file for cross-building libcui for iOS from a Mac:
#
#   cmake -S . -B build-ios -DCMAKE_TOOLCHAIN_FILE=platform/ios/ios.toolchain.cmake \
#       -DCUI_PLATFORM=ios -DCMAKE_BUILD_TYPE=MinSizeRel -DCMAKE_SYSTEM_NAME=iOS
#   cmake --build build-ios
#
# Requires Xcode (for the iOS SDK). CMake has built-in iOS support through
# CMAKE_SYSTEM_NAME=iOS; this file only sets the defaults CUI wants and can
# be overridden from the command line.
set(CMAKE_SYSTEM_NAME iOS)
# Default to the simulator SDK (pick a real device by passing
# -DCMAKE_OSX_SYSROOT=iphoneos); not FORCE so a caller-supplied SDK wins —
# device binaries are marked platform=iOS and the simulator rejects them.
if(NOT CMAKE_OSX_SYSROOT)
    set(CMAKE_OSX_SYSROOT iphonesimulator CACHE STRING "")
endif()
set(CUI_PLATFORM "ios" CACHE STRING "CUI window backend" FORCE)
set(CUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)

# The examples become iOS apps through the Xcode shell project; on their own
# they build as static libraries linked into the app bundle by Xcode.
set(CMAKE_MACOSX_BUNDLE ON)
