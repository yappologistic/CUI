#!/bin/sh
# Build the Android hello APK and install it on a connected device, then open it.
# Needs adb (in PATH) and one device attached: ./sideload-apk.sh
set -e
cd "$(dirname "$0")"

APK=platform/android/app/build/outputs/apk/debug/app-debug.apk

(cd platform/android && gradle :app:assembleDebug --no-daemon)

adb install -r "$APK"
adb shell am start -n org.cui.hello/android.app.NativeActivity
