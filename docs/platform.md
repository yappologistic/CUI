# Platforms

A build has one window backend, set with `-DCUI_PLATFORM=` (`auto` is Win32 on Windows, Cocoa on macOS and X11 on Linux), and optionally one GPU renderer. `cui_app_backend` returns the backend's name.

| System | Window backend | Renderers | Window position |
|---|---|---|---|
| Windows | `win32` | OpenGL, software, Direct3D 12 (`CUI_D3D12`) | read and set |
| macOS | `cocoa` | OpenGL, software, Metal (`CUI_METAL`) | read and set |
| Linux, X11 | `x11` | OpenGL (GLX), software, Vulkan (`CUI_VULKAN`) | read and set |
| Linux, Wayland | `wayland` | OpenGL (EGL), software, Vulkan (`CUI_VULKAN`) | compositor controlled |
| Any | `sdl2`, `glfw` | OpenGL, software | as the library gives it |

The GPU renderers and the Wayland window are built with CMake only, not in the single-file distribution. `CUI_RENDERER=gpu` picks the GPU renderer at run time; [Performance](performance.md#direct3d-12-vulkan-and-metal-against-opengl) compares them.

On Windows, `cui_app_file_dialog` with `CUI_FILE_FOLDER` works from either a single-threaded or a multithreaded COM apartment. The call waits for selection or cancellation in both cases.

## Wayland windows

`-DCUI_PLATFORM=wayland` builds a native Wayland window (`src/platform/cui_app_wayland.c`) in place of the X11 one: it needs no X11, but does not run on a bare X server. CMake needs `pkg-config`, `wayland-client`, `wayland-egl`, `wayland-cursor`, `xkbcommon`, `wayland-protocols` 1.31 or newer and `wayland-scanner` (Debian and Ubuntu: `pkg-config libwayland-dev libxkbcommon-dev wayland-protocols`). The window is drawn at the display's own pixels, fractional scales included, and the compositor decides the scale (`CUI_SCALE` does nothing). It uses the compositor's title bar, or draws a plain one where there is none (GNOME). A window can neither read nor set its position. Not there yet: input methods, dropped files, touch and the primary selection; a hidden app drawn with Vulkan shows its window when a screenshot is taken.
