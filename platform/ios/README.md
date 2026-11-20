# CUI on iOS

libcui and examples/hello cross-build for iOS with the `ios` platform
backend (`src/platform/cui_app_ios.m`). Everything here needs a Mac with
Xcode; the CMake part can run anywhere with the iOS SDK installed.

## What the backend does

- `main` in `cui_app_ios.m` calls `UIApplicationMain`; the delegate builds
  the window, a `CAEAGLLayer`-backed view and a hidden text field, then
  starts the app's own `main` — renamed to `cui_ios_user_main` by including
  `cui/cui_ios_main.h` — on a second thread.
- Touches and soft-keyboard text are queued on the main thread and drained
  in `cui__plat_wait` on the app thread; frames run entirely on the app
  thread.
- GL is EAGLContext (GLES 3) with the framebuffer attached to the layer's
  renderbuffer; the software renderer puts a CGImage on the layer.
- Rotation: `layoutSubviews` on the view posts a relayout event; the next
  frame reads the new size, and in the GL path the layer's renderbuffer is
  reallocated (same buffer objects) before it is presented again.

## Building

```sh
cmake -S . -B build-ios -DCMAKE_TOOLCHAIN_FILE=platform/ios/ios.toolchain.cmake \
    -DCMAKE_BUILD_TYPE=MinSizeRel -DCMAKE_OSX_ARCHITECTURES=arm64
cmake --build build-ios
```

This produces `libcui.a` plus the example objects. To get a runnable app,
open Xcode, create an App project, drop in the produced library and
`examples/hello.c` (with `#include "cui/cui_ios_main.h"` above it), and set
the deployment target to iOS 13 or later. A generated `.xcodeproj` shell is
not checked in; adding one (or `xcodegen` rules) is a follow-up for a Mac.

For a bare command-line run on the simulator (Xcode 16, iOS 18.5 SDK,
verified with examples/hello):

```sh
cp build-ios/examples/hello /tmp/hello && file /tmp/hello   # Mach-O, platform IOSSIMULATOR
xcrun simctl boot "iPhone 16 Pro" && xcrun simctl install booted <bundle.app>
xcrun simctl launch booted org.cui.hello && xcrun simctl io booted screenshot shot.png
```

## Simulator notes

- On the simulator `CAEAGLLayer` falls back to a plain `CALayer` (no EAGL),
  and even the class-fallback drawable composites black; `cui__plat_gl_create`
  therefore rejects the simulator by environment check and the app runs the
  software renderer. On a device the GL renderer is used.
- Without a `UILaunchScreen` in the bundle (see `Info.plist.in`), iOS starts
  the scene at a provisional 320x480 and the first run draws nothing until
  the app is left and reopened; the generated example bundles carry the
  launch screen and the delegate re-syncs the window in
  `applicationDidBecomeActive`.
- Touches are handled by `CUIIOSView`, not the app delegate — a
  `UIApplicationDelegate` never sees them through the responder chain.

## Layout

- `ios.toolchain.cmake` — CMake toolchain defaults for iOS.
