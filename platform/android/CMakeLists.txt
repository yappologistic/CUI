# Cross-build of libcui and examples/hello for Android, used by the Gradle
# shell in this directory and by hand with the NDK toolchain:
#
#   cmake -S platform/android -B build-android \
#       -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
#       -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24
#   cmake --build build-android
#
# The result is libhello.so, loaded by NativeActivity through the
# android.app.lib_name meta-data in the manifest.
cmake_minimum_required(VERSION 3.14)
project(cui-android LANGUAGES C)

set(CUI_ANDROID_APP hello CACHE STRING "example to package (sources under examples/)")
set(CUI_PLATFORM android CACHE STRING "" FORCE)
set(CUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CUI_WITH_APP ON CACHE BOOL "" FORCE)

# android_native_app_glue ships with the NDK; the toolchain file knows where
set(CUI_ANDROID_GLUE "" CACHE PATH "directory with android_native_app_glue.c")
if(NOT CUI_ANDROID_GLUE)
    if(ANDROID_NDK)
        set(CUI_ANDROID_GLUE ${ANDROID_NDK}/sources/android/native_app_glue)
    elseif(DEFINED ENV{ANDROID_NDK_HOME})
        set(CUI_ANDROID_GLUE $ENV{ANDROID_NDK_HOME}/sources/android/native_app_glue)
    endif()
endif()
if(NOT EXISTS ${CUI_ANDROID_GLUE}/android_native_app_glue.c)
    message(FATAL_ERROR "android_native_app_glue not found; set CUI_ANDROID_GLUE to the NDK's sources/android/native_app_glue")
endif()

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../.. cui)

add_library(cui_glue STATIC ${CUI_ANDROID_GLUE}/android_native_app_glue.c)
target_include_directories(cui_glue PUBLIC ${CUI_ANDROID_GLUE})
target_compile_definitions(cui_glue PRIVATE NDEBUG=)
target_link_libraries(cui_glue PUBLIC android log)
# the backend includes android_native_app_glue.h by file name
target_include_directories(cui PUBLIC ${CUI_ANDROID_GLUE})

add_library(${CUI_ANDROID_APP} SHARED ${CMAKE_CURRENT_LIST_DIR}/../../examples/${CUI_ANDROID_APP}.c)
# whole-archive: nothing references ANativeActivity_onCreate at link time (the
# JVM looks it up at runtime), so a plain link drops the glue object
# 16 KB (and common) page alignment: required for Play distribution since NDK r27
# and expected with r26's 16 KB device support
target_link_options(${CUI_ANDROID_APP} PRIVATE
    "-Wl,-z,max-page-size=16384"
    "-Wl,-z,common-page-size=16384")
target_link_libraries(${CUI_ANDROID_APP} PRIVATE cui
    -Wl,--whole-archive cui_glue -Wl,--no-whole-archive)
