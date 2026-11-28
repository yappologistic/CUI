/* Wayland backend (Linux, BSD). An xdg-shell toplevel drawn at the display's own pixels
 * (wp_fractional_scale_v1 with wp_viewporter, whole output scales without them), pointer and
 * keyboard through wl_seat and xkbcommon, clipboard text through wl_data_device, EGL (loaded
 * at runtime) on a wl_egl_window for OpenGL, VK_KHR_wayland_surface for Vulkan and wl_shm for
 * the software renderer.
 *
 * Decorations: the compositor's, where it offers xdg-decoration. Where it does not (GNOME), a
 * plain title bar on a subsurface above the content: drag to move, double click to maximize,
 * close, maximize and minimize buttons, and invisible borders around the window to resize by.
 * It is drawn here, in a few hundred bytes of shared memory; no toolkit is loaded for it.
 *
 * Not here yet: text input methods (text-input-v3), compose and dead keys, files dropped on
 * the window, a window title drawn in the built-in title bar, and placing the window (Wayland
 * has no window positions). Built with -DCUI_PLATFORM=wayland, instead of the X11 backend. */
#include "cui_platform.h"

#include <wayland-client.h>
#include <wayland-cursor.h>
#include <wayland-egl.h>
#include <xkbcommon/xkbcommon.h>
#include "xdg-shell-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

extern int memfd_create(const char *name, unsigned int flags);   /* glibc 2.27, musl 1.1.20 */

/* EGL, loaded at runtime like GLX in the X11 backend */
typedef void *cui__egl_display, *cui__egl_config, *cui__egl_context, *cui__egl_surface;
#define EGL_ALPHA_SIZE_ 0x3021
#define EGL_BLUE_SIZE_ 0x3022
#define EGL_GREEN_SIZE_ 0x3023
#define EGL_RED_SIZE_ 0x3024
#define EGL_SURFACE_TYPE_ 0x3033
#define EGL_NONE_ 0x3038
#define EGL_RENDERABLE_TYPE_ 0x3040
#define EGL_WINDOW_BIT_ 0x0004
#define EGL_OPENGL_BIT_ 0x0008
#define EGL_OPENGL_ES3_BIT_ 0x0040
#define EGL_CONTEXT_MAJOR_VERSION_ 0x3098
#define EGL_CONTEXT_MINOR_VERSION_ 0x30FB
#define EGL_CONTEXT_OPENGL_PROFILE_MASK_ 0x30FD
#define EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_ 0x0001
#define EGL_OPENGL_ES_API_ 0x30A0
#define EGL_OPENGL_API_ 0x30A2
#define EGL_PLATFORM_WAYLAND_ 0x31D8

typedef struct cui__egl {
    void *lib;
    cui__egl_display (*GetDisplay)(void *native);
    cui__egl_display (*GetPlatformDisplay)(unsigned platform, void *native, const intptr_t *attrs);
    unsigned (*Initialize)(cui__egl_display, int *major, int *minor);
    unsigned (*BindAPI)(unsigned api);
    unsigned (*ChooseConfig)(cui__egl_display, const int *attrs, cui__egl_config *out, int cap, int *count);
    cui__egl_context (*CreateContext)(cui__egl_display, cui__egl_config, cui__egl_context share, const int *attrs);
    cui__egl_surface (*CreateWindowSurface)(cui__egl_display, cui__egl_config, void *window, const int *attrs);
    unsigned (*MakeCurrent)(cui__egl_display, cui__egl_surface draw, cui__egl_surface read, cui__egl_context);
    unsigned (*SwapBuffers)(cui__egl_display, cui__egl_surface);
    unsigned (*SwapInterval)(cui__egl_display, int interval);
    unsigned (*DestroySurface)(cui__egl_display, cui__egl_surface);
    unsigned (*DestroyContext)(cui__egl_display, cui__egl_context);
    unsigned (*Terminate)(cui__egl_display);
    void *(*GetProcAddress)(const char *name);
    cui__egl_display display;
    cui__egl_config config;
    cui__egl_context context;
    cui__egl_surface surface;
} cui__egl;

#define CUI__WL_BAR 30          /* built-in title bar: height, and the width of a button, in points */
#define CUI__WL_BORDER 8        /* invisible border to resize by, outside the window */
#define CUI__WL_CORNER 20       /* this near a border's end, the corner is dragged */
#define CUI__WL_OUTPUTS 8

typedef enum { CUI__WL_OVER_NOTHING, CUI__WL_OVER_CONTENT, CUI__WL_OVER_BAR, CUI__WL_OVER_BORDER } cui__wl_over;

typedef struct cui__wl_shm_buffer {
    struct wl_buffer *buffer;
    uint32_t *pixels;
    size_t bytes;
    int w, h;
    bool busy;                  /* the compositor reads it: not to be drawn into */
} cui__wl_shm_buffer;

typedef struct cui__wl_output {
    struct wl_output *output;
    uint32_t name;
    int scale, refresh_mhz;
    bool entered;               /* the window is on it */
} cui__wl_output;

typedef struct cui__wl {
    cui_app *app;
    struct wl_display *dpy;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    uint32_t compositor_version;
    struct wl_subcompositor *subcompositor;
    struct wl_shm *shm;
    struct wl_seat *seat;
    uint32_t seat_version;
    struct wl_pointer *pointer;
    struct wl_keyboard *keyboard;
    struct xdg_wm_base *wm;
    struct wp_viewporter *viewporter;
    struct wp_fractional_scale_manager_v1 *fractional_manager;
    struct zxdg_decoration_manager_v1 *decoration_manager;
    struct wl_data_device_manager *data_manager;
    struct wl_data_device *data_device;
    cui__wl_output outputs[CUI__WL_OUTPUTS];
    int output_count;

    /* the window */
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    struct wp_viewport *viewport;
    struct wp_fractional_scale_v1 *fractional;
    struct zxdg_toplevel_decoration_v1 *decoration;
    struct wl_callback *frame;      /* OpenGL: asked for with the last swap, not come yet */
    cui_x11_window gpu_window;      /* the window as cui_gpu_create takes it (Vulkan) */
    struct wl_egl_window *egl_window;
    cui__egl egl;
    int width, height;              /* content, points */
    int fb_w, fb_h;                 /* content, pixels */
    int preferred120;               /* wp_fractional_scale_v1's preferred scale in 120ths; 0 before it says */
    int scale120;                   /* the scale in use, in 120ths */
    float scale;
    int normal_w, normal_h;         /* content size while neither maximized nor fullscreen */
    int pending_w, pending_h;       /* xdg_toplevel.configure, until xdg_surface.configure */
    bool pending_maximized, pending_fullscreen, pending_tiled, pending_activated;
    bool configured, maximized, fullscreen, tiled, minimized;
    bool server_decoration;         /* the compositor draws the title bar */
    unsigned events;                /* counts the compositor's events that are news for the app (cui__wl_pump) */

    /* built-in decoration */
    struct wl_surface *bar_surface;
    struct wl_subsurface *bar_sub;
    struct wp_viewport *bar_viewport;
    cui__wl_shm_buffer bar_buffer;
    int bar_maximized;                        /* what bar_buffer was drawn for */
    uint32_t bar_colors[3];                   /* and with which colors */
    struct wl_surface *border_surface[4];     /* top, bottom, left, right */
    struct wl_subsurface *border_sub[4];
    struct wp_viewport *border_viewport[4];
    cui__wl_shm_buffer clear_pixel;           /* one transparent pixel, stretched over each border */
    int bar_pressed;                          /* button held down on the bar: 1 close, 2 maximize, 3 minimize */
    double bar_click_time;

    /* pointer */
    cui__wl_over over;
    int over_border;                /* which border */
    float px, py;                   /* on the surface it is over, points */
    uint32_t enter_serial, input_serial;
    float scroll[2], scroll120[2];  /* this frame of pointer events: horizontal, vertical */
    bool scroll_any, scroll_has120[2];
    struct wl_cursor_theme *cursor_theme;
    int cursor_theme_scale;
    struct wl_surface *cursor_surface;
    cui_cursor app_cursor;
    const char *shown_cursor;

    /* keyboard */
    struct xkb_context *xkb;
    struct xkb_keymap *keymap;
    struct xkb_state *xkb_state;
    int mods;
    int repeat_rate, repeat_delay_ms;
    uint32_t repeat_key;            /* evdev code + 8; 0: none held */
    double repeat_at;

    /* clipboard */
    struct wl_data_source *clip_source;       /* ours, while we own the selection */
    char *clip_owned, *clip_read;
    struct wl_data_offer *offer, *selection;  /* being described; the current selection */
    struct wl_data_offer *drag_offer;
    int offer_mime, selection_mime;           /* best text type offered: index into cui__wl_text_mimes + 1 */

    /* software renderer */
    cui__wl_shm_buffer sw_buffers[3];
    bool sw_dropped;                /* a picture was not shown: the compositor held all three buffers */
    bool shown;                     /* cui__plat_show was called: pictures may go to the window */
    bool unpaced;                   /* cui_app_desc.no_vsync: frames do not wait for the compositor's frame callbacks */
    bool gpu_started, gpu_skipped;  /* Vulkan: a frame was drawn; one was held back while the window was hidden (cui__plat_gpu_frame) */
    bool gpu_seen;                  /* Vulkan: a frame callback came, so a picture is on the surface */
    bool sw_retry;                  /* software: no buffer could be made and none is out to come back */
    cui_key held_key[256];          /* the key each key code went down as, so a layout switch releases it */
} cui__wl;

#define CUI__WL_CLIP_SECONDS 5.0             /* the longest a paste waits for its text */
#define CUI__WL_CLIP_BYTES (64u << 20)      /* the most text a paste takes */

static const char *const cui__wl_text_mimes[] = { "text/plain;charset=utf-8", "UTF8_STRING", "text/plain" };
static char cui__wl_error[160];
static cui__egl *cui__wl_gl;    /* cui__plat_gl_proc has no app to find it by */

static cui__wl *cui__wl_of(cui_app *app) { return app->plat; }

double cui__plat_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

const char *cui__plat_name(void) { return "wayland"; }

int cui__plat_system_dark(void) {
    const char *gtk = getenv("GTK_THEME");
    if (gtk && strstr(gtk, "dark")) return 1;
    return -1;
}

/* ---- shared memory buffers ---- */


/* A buffer of the software renderer's came back. If a picture was dropped while the compositor
 * held them all, the window lacks what that picture changed: the whole picture goes out again. */
static void cui__wl_sw_buffer_release(void *data, struct wl_buffer *buffer) {
    cui__wl *w = data;
    for (int i = 0; i < 3; i++) if (w->sw_buffers[i].buffer == buffer) w->sw_buffers[i].busy = false;
    if (w->sw_dropped) {
        w->sw_dropped = false;
        w->app->force_present = true;
        cui_request_redraw(w->app->ui);
        w->events++;
    }
}
static const struct wl_buffer_listener cui__wl_sw_buffer_listener = { cui__wl_sw_buffer_release };

static void cui__wl_shm_free(cui__wl_shm_buffer *b) {
    if (b->buffer) wl_buffer_destroy(b->buffer);
    if (b->pixels) munmap(b->pixels, b->bytes);
    memset(b, 0, sizeof *b);
}

static bool cui__wl_shm_make(cui__wl *w, cui__wl_shm_buffer *b, int width, int height, bool alpha) {
    cui__wl_shm_free(b);
    if (!w->shm || width <= 0 || height <= 0) return false;
    size_t bytes = (size_t)width * (size_t)height * 4;
    int fd = memfd_create("cui", 1 /* MFD_CLOEXEC */);
    if (fd < 0) {
        char path[512];
        const char *dir = getenv("XDG_RUNTIME_DIR");
        snprintf(path, sizeof path, "%s/cui-XXXXXX", dir && *dir ? dir : "/tmp");
        fd = mkstemp(path);
        if (fd >= 0) unlink(path);
    }
    if (fd < 0) return false;
    if (ftruncate(fd, (off_t)bytes) != 0) { close(fd); return false; }
    void *map = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) { close(fd); return false; }
    struct wl_shm_pool *pool = wl_shm_create_pool(w->shm, fd, (int32_t)bytes);
    b->buffer = wl_shm_pool_create_buffer(pool, 0, width, height, width * 4, alpha ? WL_SHM_FORMAT_ARGB8888 : WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    b->pixels = map; b->bytes = bytes; b->w = width; b->h = height; b->busy = false;
    return true;
}

static void cui__wl_damage_all(cui__wl *w, struct wl_surface *s) {
    if (w->compositor_version >= 4) wl_surface_damage_buffer(s, 0, 0, INT32_MAX, INT32_MAX);
    else wl_surface_damage(s, 0, 0, INT32_MAX, INT32_MAX);
}

/* ---- scale and size ---- */

static int cui__wl_output_scale(cui__wl *w) {
    int entered = 0, any = 1;
    for (int i = 0; i < w->output_count; i++) {
        if (w->outputs[i].scale > any) any = w->outputs[i].scale;
        if (w->outputs[i].entered && w->outputs[i].scale > entered) entered = w->outputs[i].scale;
    }
    return entered ? entered : any;   /* before the window is on an output: the sharpest there is */
}

static bool cui__wl_bar_wanted(cui__wl *w) { return !w->server_decoration && !w->fullscreen && w->subcompositor && w->shm; }
static int cui__wl_bar_height(cui__wl *w) { return cui__wl_bar_wanted(w) ? CUI__WL_BAR : 0; }

static void cui__wl_line(cui__wl_shm_buffer *b, int x0, int y0, int x1, int y1, int thick, uint32_t color) {
    int steps = abs(x1 - x0) > abs(y1 - y0) ? abs(x1 - x0) : abs(y1 - y0);
    for (int i = 0; i <= steps; i++) {
        int x = steps ? x0 + (x1 - x0) * i / steps : x0, y = steps ? y0 + (y1 - y0) * i / steps : y0;
        for (int dy = 0; dy < thick; dy++)
            for (int dx = 0; dx < thick; dx++)
                if (x + dx >= 0 && x + dx < b->w && y + dy >= 0 && y + dy < b->h) b->pixels[(size_t)(y + dy) * b->w + (x + dx)] = color;
    }
}

/* The title bar's picture: the theme's surface color, a line under it, and the three buttons. */
static bool cui__wl_bar_draw(cui__wl *w) {
    int bw = (w->width * w->scale120 + 60) / 120, bh = (CUI__WL_BAR * w->scale120 + 60) / 120;
    const cui_color *colors = w->app->ui->theme.colors;
    cui_color bg = colors[CUI_TOKEN_SURFACE], fg = colors[CUI_TOKEN_TEXT_SECONDARY], line = colors[CUI_TOKEN_BORDER_SUBTLE];
    uint32_t bgc = 0xff000000u | ((uint32_t)bg.r << 16) | ((uint32_t)bg.g << 8) | bg.b;
    uint32_t fgc = 0xff000000u | ((uint32_t)fg.r << 16) | ((uint32_t)fg.g << 8) | fg.b;
    uint32_t linec = ((uint32_t)line.a << 24) | ((uint32_t)line.r << 16) | ((uint32_t)line.g << 8) | line.b;
    if (bw == w->bar_buffer.w && bh == w->bar_buffer.h && w->bar_maximized == (int)w->maximized &&
        bgc == w->bar_colors[0] && fgc == w->bar_colors[1] && linec == w->bar_colors[2]) return false;
    /* (the compositor took the old picture when it was committed: its buffer can go first) */
    if (!cui__wl_shm_make(w, &w->bar_buffer, bw, bh, false)) return false;
    cui__wl_shm_buffer fresh = w->bar_buffer;
    w->bar_colors[0] = bgc; w->bar_colors[1] = fgc; w->bar_colors[2] = linec;
    /* the border color is translucent in most themes: over the bar's own color */
    uint32_t lr = (line.r * line.a + bg.r * (255 - line.a)) / 255, lg = (line.g * line.a + bg.g * (255 - line.a)) / 255,
             lb = (line.b * line.a + bg.b * (255 - line.a)) / 255;
    for (size_t i = 0; i < (size_t)bw * bh; i++) fresh.pixels[i] = bgc;
    for (int x = 0; x < bw; x++) fresh.pixels[(size_t)(bh - 1) * bw + x] = 0xff000000u | (lr << 16) | (lg << 8) | lb;
    float s = (float)w->scale120 / 120.0f;
    int thick = (int)(s * 1.2f + 0.5f); if (thick < 1) thick = 1;
    int half = (int)(4.5f * s), cy = bh / 2 - thick / 2;
    for (int button = 1; button <= 3; button++) {
        if (button == 2 && w->app->desc.fixed_size) continue;
        int cx = (int)(((float)w->width - ((float)button - 0.5f) * CUI__WL_BAR) * s) - thick / 2;
        if (button == 1) {
            cui__wl_line(&fresh, cx - half, cy - half, cx + half, cy + half, thick, fgc);
            cui__wl_line(&fresh, cx - half, cy + half, cx + half, cy - half, thick, fgc);
        } else if (button == 2) {
            cui__wl_line(&fresh, cx - half, cy - half, cx + half, cy - half, thick, fgc);
            cui__wl_line(&fresh, cx - half, cy + half, cx + half, cy + half, thick, fgc);
            cui__wl_line(&fresh, cx - half, cy - half, cx - half, cy + half, thick, fgc);
            cui__wl_line(&fresh, cx + half, cy - half, cx + half, cy + half, thick, fgc);
        } else {
            cui__wl_line(&fresh, cx - half, cy + half, cx + half, cy + half, thick, fgc);
        }
    }
    wl_surface_attach(w->bar_surface, fresh.buffer, 0, 0);
    cui__wl_damage_all(w, w->bar_surface);
    w->bar_maximized = w->maximized;
    return true;
}

static struct wl_surface *cui__wl_child(cui__wl *w, struct wl_subsurface **sub, struct wp_viewport **viewport) {
    struct wl_surface *s = wl_compositor_create_surface(w->compositor);
    *sub = wl_subcompositor_get_subsurface(w->subcompositor, s, w->surface);
    wl_subsurface_set_desync(*sub);
    *viewport = w->viewporter ? wp_viewporter_get_viewport(w->viewporter, s) : NULL;
    return s;
}

static void cui__wl_child_free(struct wl_surface **s, struct wl_subsurface **sub, struct wp_viewport **viewport) {
    if (*viewport) wp_viewport_destroy(*viewport);
    if (*sub) wl_subsurface_destroy(*sub);
    if (*s) wl_surface_destroy(*s);
    *s = NULL; *sub = NULL; *viewport = NULL;
}

/* Makes, places or removes the built-in title bar and the resize borders for the window as it
 * is now. Their places take effect with the content's next commit. */
static void cui__wl_decorate(cui__wl *w) {
    bool bar = cui__wl_bar_wanted(w);
    bool borders = bar && w->viewporter && !w->maximized && !w->tiled && !w->app->desc.fixed_size;
    if (!bar && w->bar_surface) {
        cui__wl_child_free(&w->bar_surface, &w->bar_sub, &w->bar_viewport);
        cui__wl_shm_free(&w->bar_buffer);
    }
    if (bar) {
        if (!w->bar_surface) w->bar_surface = cui__wl_child(w, &w->bar_sub, &w->bar_viewport);
        wl_subsurface_set_position(w->bar_sub, 0, -CUI__WL_BAR);
        if (w->bar_viewport) wp_viewport_set_destination(w->bar_viewport, w->width, CUI__WL_BAR);
        else wl_surface_set_buffer_scale(w->bar_surface, w->scale120 / 120);
        cui__wl_bar_draw(w);
        wl_surface_commit(w->bar_surface);
    }
    for (int i = 0; i < 4; i++) {
        if (!borders) {
            if (w->border_surface[i]) cui__wl_child_free(&w->border_surface[i], &w->border_sub[i], &w->border_viewport[i]);
            continue;
        }
        if (!w->clear_pixel.buffer && cui__wl_shm_make(w, &w->clear_pixel, 1, 1, true)) w->clear_pixel.pixels[0] = 0;
        if (!w->clear_pixel.buffer) break;
        bool fresh = !w->border_surface[i];
        if (fresh) w->border_surface[i] = cui__wl_child(w, &w->border_sub[i], &w->border_viewport[i]);
        int across = w->width + 2 * CUI__WL_BORDER, down = w->height + CUI__WL_BAR;
        int x = i == 3 ? w->width : -CUI__WL_BORDER, y = i == 0 ? -CUI__WL_BAR - CUI__WL_BORDER : i == 1 ? w->height : -CUI__WL_BAR;
        wl_subsurface_set_position(w->border_sub[i], x, y);
        wp_viewport_set_destination(w->border_viewport[i], i < 2 ? across : CUI__WL_BORDER, i < 2 ? CUI__WL_BORDER : down);
        if (fresh) wl_surface_attach(w->border_surface[i], w->clear_pixel.buffer, 0, 0);
        wl_surface_commit(w->border_surface[i]);
    }
}

/* The window's size, scale or decoration changed: buffers, viewport, geometry. */
static void cui__wl_apply(cui__wl *w) {
    int scale120 = w->preferred120 ? w->preferred120 : cui__wl_output_scale(w) * 120;
    int fb_w = (w->width * scale120 + 60) / 120, fb_h = (w->height * scale120 + 60) / 120;
    if (w->viewport) {
        wl_surface_set_buffer_scale(w->surface, 1);
        wp_viewport_set_destination(w->viewport, w->width, w->height);
    } else {
        wl_surface_set_buffer_scale(w->surface, scale120 / 120);
    }
    bool changed = fb_w != w->fb_w || fb_h != w->fb_h || scale120 != (int)(w->scale * 120.0f + 0.5f);
    w->scale120 = scale120;
    w->scale = (float)scale120 / 120.0f;
    w->fb_w = fb_w; w->fb_h = fb_h;
    w->gpu_window.width = fb_w; w->gpu_window.height = fb_h;
    if (w->egl_window) wl_egl_window_resize(w->egl_window, fb_w, fb_h, 0, 0);
    struct wl_region *opaque = wl_compositor_create_region(w->compositor);   /* nothing behind the content shows through */
    wl_region_add(opaque, 0, 0, w->width, w->height);
    wl_surface_set_opaque_region(w->surface, opaque);
    wl_region_destroy(opaque);
    int bar = cui__wl_bar_height(w);
    xdg_surface_set_window_geometry(w->xdg_surface, 0, -bar, w->width, w->height + bar);
    cui__wl_decorate(w);
    if (changed) {
        w->app->force_present = true;
        cui_request_redraw(w->app->ui);
    }
}

/* ---- cursors ---- */

static void cui__wl_show_cursor(cui__wl *w, const char *names) {
    /* names: alternatives separated by '|', the first the theme has is shown */
    if (!w->pointer || !w->shm || names == w->shown_cursor) return;
    int scale = (w->scale120 + 119) / 120;
    if (scale < 1) scale = 1;
    if (!w->cursor_theme || w->cursor_theme_scale != scale) {
        if (w->cursor_theme) wl_cursor_theme_destroy(w->cursor_theme);
        const char *size = getenv("XCURSOR_SIZE");
        int px = size ? atoi(size) : 0;
        w->cursor_theme = wl_cursor_theme_load(getenv("XCURSOR_THEME"), (px > 0 ? px : 24) * scale, w->shm);
        w->cursor_theme_scale = scale;
    }
    if (!w->cursor_theme) return;
    struct wl_cursor *cursor = NULL;
    for (const char *p = names; p && *p && !cursor;) {
        char name[48];
        size_t n = strcspn(p, "|");
        snprintf(name, sizeof name, "%.*s", (int)(n < sizeof name - 1 ? n : sizeof name - 1), p);
        cursor = wl_cursor_theme_get_cursor(w->cursor_theme, name);
        p += n; if (*p == '|') p++;
    }
    if (!cursor || cursor->image_count < 1) return;
    struct wl_cursor_image *image = cursor->images[0];
    if (!w->cursor_surface) w->cursor_surface = wl_compositor_create_surface(w->compositor);
    wl_surface_set_buffer_scale(w->cursor_surface, scale);
    wl_surface_attach(w->cursor_surface, wl_cursor_image_get_buffer(image), 0, 0);
    cui__wl_damage_all(w, w->cursor_surface);
    wl_surface_commit(w->cursor_surface);
    wl_pointer_set_cursor(w->pointer, w->enter_serial, w->cursor_surface, (int)image->hotspot_x / scale, (int)image->hotspot_y / scale);
    w->shown_cursor = names;
}

static void cui__wl_show_app_cursor(cui__wl *w) {
    static const char *const names[CUI_CURSOR_COUNT] = {
        "left_ptr|default", "hand2|pointer|hand1", "xterm|text", "sb_h_double_arrow|ew-resize|col-resize",
        "sb_v_double_arrow|ns-resize|row-resize", "fleur|move|all-scroll", "not-allowed|crossed_circle|X_cursor" };
    cui__wl_show_cursor(w, names[w->app_cursor]);
}

static void cui__wl_set_cursor(void *user, cui_cursor c) {
    cui__wl *w = cui__wl_of(user);
    if ((int)c < 0 || c >= CUI_CURSOR_COUNT) return;
    w->app_cursor = c;
    if (w->over == CUI__WL_OVER_CONTENT) cui__wl_show_app_cursor(w);
}

/* ---- built-in decoration: pointer ---- */

/* The xdg_toplevel resize edge under the pointer on a border. */
static uint32_t cui__wl_border_edge(cui__wl *w) {
    int across = w->width + 2 * CUI__WL_BORDER, down = w->height + CUI__WL_BAR, edge_zone = CUI__WL_BORDER + CUI__WL_CORNER;
    switch (w->over_border) {
    case 0: return w->px < edge_zone ? XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT : w->px > across - edge_zone ? XDG_TOPLEVEL_RESIZE_EDGE_TOP_RIGHT : XDG_TOPLEVEL_RESIZE_EDGE_TOP;
    case 1: return w->px < edge_zone ? XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT : w->px > across - edge_zone ? XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT : XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM;
    case 2: return w->py < CUI__WL_CORNER ? XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT : w->py > down - CUI__WL_CORNER ? XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT : XDG_TOPLEVEL_RESIZE_EDGE_LEFT;
    default: return w->py < CUI__WL_CORNER ? XDG_TOPLEVEL_RESIZE_EDGE_TOP_RIGHT : w->py > down - CUI__WL_CORNER ? XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT : XDG_TOPLEVEL_RESIZE_EDGE_RIGHT;
    }
}

static void cui__wl_decoration_cursor(cui__wl *w) {
    if (w->over == CUI__WL_OVER_BAR) { cui__wl_show_cursor(w, "left_ptr|default"); return; }
    switch (cui__wl_border_edge(w)) {
    case XDG_TOPLEVEL_RESIZE_EDGE_TOP: cui__wl_show_cursor(w, "top_side|n-resize|sb_v_double_arrow"); break;
    case XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM: cui__wl_show_cursor(w, "bottom_side|s-resize|sb_v_double_arrow"); break;
    case XDG_TOPLEVEL_RESIZE_EDGE_LEFT: cui__wl_show_cursor(w, "left_side|w-resize|sb_h_double_arrow"); break;
    case XDG_TOPLEVEL_RESIZE_EDGE_RIGHT: cui__wl_show_cursor(w, "right_side|e-resize|sb_h_double_arrow"); break;
    case XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT: cui__wl_show_cursor(w, "top_left_corner|nw-resize"); break;
    case XDG_TOPLEVEL_RESIZE_EDGE_TOP_RIGHT: cui__wl_show_cursor(w, "top_right_corner|ne-resize"); break;
    case XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT: cui__wl_show_cursor(w, "bottom_left_corner|sw-resize"); break;
    default: cui__wl_show_cursor(w, "bottom_right_corner|se-resize"); break;
    }
}

/* The button of the title bar under the pointer: 1 close, 2 maximize, 3 minimize, 0 none. */
static int cui__wl_bar_button(cui__wl *w) {
    int n = (int)(((float)w->width - w->px) / CUI__WL_BAR) + 1;
    if (w->px > (float)w->width || n < 1 || n > 3 || (n == 2 && w->app->desc.fixed_size)) return 0;
    return n;
}

static void cui__wl_toggle_maximized(cui__wl *w) {
    if (w->app->desc.fixed_size) return;
    if (w->maximized) xdg_toplevel_unset_maximized(w->toplevel);
    else xdg_toplevel_set_maximized(w->toplevel);
}

static void cui__wl_decoration_button(cui__wl *w, uint32_t serial, uint32_t button, bool down) {
    if (w->over == CUI__WL_OVER_BORDER) {
        if (down && button == 0x110) xdg_toplevel_resize(w->toplevel, w->seat, serial, cui__wl_border_edge(w));
        return;
    }
    if (button == 0x111) {   /* right: the compositor's window menu */
        if (down) xdg_toplevel_show_window_menu(w->toplevel, w->seat, serial, (int)w->px, (int)w->py - CUI__WL_BAR);
        return;
    }
    if (button != 0x110) return;
    int over = cui__wl_bar_button(w);
    if (down) {
        w->bar_pressed = over;
        if (over) return;
        double now = cui__plat_now();
        if (now - w->bar_click_time < 0.4) { w->bar_click_time = 0; cui__wl_toggle_maximized(w); return; }
        w->bar_click_time = now;
        xdg_toplevel_move(w->toplevel, w->seat, serial);
        return;
    }
    if (over && over == w->bar_pressed) {
        if (over == 1) w->app->running = false;
        else if (over == 2) cui__wl_toggle_maximized(w);
        else xdg_toplevel_set_minimized(w->toplevel);
    }
    w->bar_pressed = 0;
}

/* ---- pointer ---- */

static void cui__wl_pointer_enter(void *data, struct wl_pointer *p, uint32_t serial, struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y) {
    cui__wl *w = data;
    w->events++;
    (void)p;
    w->enter_serial = serial;
    w->px = (float)wl_fixed_to_double(x); w->py = (float)wl_fixed_to_double(y);
    w->shown_cursor = NULL;   /* the compositor forgot the cursor when the pointer left */
    w->over = CUI__WL_OVER_NOTHING;
    if (surface == w->surface) w->over = CUI__WL_OVER_CONTENT;
    else if (surface && surface == w->bar_surface) w->over = CUI__WL_OVER_BAR;
    else for (int i = 0; i < 4; i++) if (surface && surface == w->border_surface[i]) { w->over = CUI__WL_OVER_BORDER; w->over_border = i; }
    if (w->over == CUI__WL_OVER_CONTENT) {
        cui_input_mouse_move(w->app->ui, w->px, w->py);
        cui__wl_show_app_cursor(w);
    } else if (w->over != CUI__WL_OVER_NOTHING) {
        cui__wl_decoration_cursor(w);
    }
}

static void cui__wl_pointer_leave(void *data, struct wl_pointer *p, uint32_t serial, struct wl_surface *surface) {
    cui__wl *w = data;
    w->events++;
    (void)p; (void)serial; (void)surface;
    if (w->over == CUI__WL_OVER_CONTENT) {
        /* a pointer that leaves during a press was taken by the compositor (a window move, its
         * menu): the release goes to it, so the buttons are let go here */
        static const cui_mouse_button buttons[] = { CUI_MOUSE_LEFT, CUI_MOUSE_RIGHT, CUI_MOUSE_MIDDLE };
        for (size_t i = 0; i < sizeof buttons / sizeof buttons[0]; i++)
            if (cui_mouse_down(w->app->ui, buttons[i])) cui_input_mouse_button(w->app->ui, buttons[i], false);
        cui_input_mouse_leave(w->app->ui);
    }
    w->over = CUI__WL_OVER_NOTHING;
    w->bar_pressed = 0;
}

static void cui__wl_pointer_motion(void *data, struct wl_pointer *p, uint32_t time, wl_fixed_t x, wl_fixed_t y) {
    cui__wl *w = data;
    w->events++;
    (void)p; (void)time;
    w->px = (float)wl_fixed_to_double(x); w->py = (float)wl_fixed_to_double(y);
    if (w->over == CUI__WL_OVER_CONTENT) cui_input_mouse_move(w->app->ui, w->px, w->py);
    else if (w->over != CUI__WL_OVER_NOTHING) cui__wl_decoration_cursor(w);
}

static void cui__wl_pointer_button(void *data, struct wl_pointer *p, uint32_t serial, uint32_t time, uint32_t button, uint32_t state) {
    cui__wl *w = data;
    w->events++;
    (void)p; (void)time;
    bool down = state == WL_POINTER_BUTTON_STATE_PRESSED;
    w->input_serial = serial;
    if (w->over == CUI__WL_OVER_BAR || w->over == CUI__WL_OVER_BORDER) { cui__wl_decoration_button(w, serial, button, down); return; }
    if (w->over != CUI__WL_OVER_CONTENT) return;
    cui_input_mouse_move(w->app->ui, w->px, w->py);
    if (button == 0x110) cui_input_mouse_button(w->app->ui, CUI_MOUSE_LEFT, down);        /* BTN_LEFT */
    else if (button == 0x111) cui_input_mouse_button(w->app->ui, CUI_MOUSE_RIGHT, down);  /* BTN_RIGHT */
    else if (button == 0x112) cui_input_mouse_button(w->app->ui, CUI_MOUSE_MIDDLE, down); /* BTN_MIDDLE */
}

/* A wheel notch is 10 units on the axis, or 120 where the compositor counts in 120ths; a
 * touchpad sends fractions of that. CUI counts lines, up and left positive. */
static void cui__wl_scroll_flush(cui__wl *w) {
    if (!w->scroll_any) return;
    float dx = w->scroll_has120[0] ? w->scroll120[0] / 120.0f : w->scroll[0] / 10.0f;
    float dy = w->scroll_has120[1] ? w->scroll120[1] / 120.0f : w->scroll[1] / 10.0f;
    if (w->over == CUI__WL_OVER_CONTENT && (dx != 0 || dy != 0)) cui_input_scroll(w->app->ui, -dx, -dy);
    w->scroll[0] = w->scroll[1] = w->scroll120[0] = w->scroll120[1] = 0;
    w->scroll_has120[0] = w->scroll_has120[1] = w->scroll_any = false;
}

static void cui__wl_pointer_axis(void *data, struct wl_pointer *p, uint32_t time, uint32_t axis, wl_fixed_t value) {
    cui__wl *w = data;
    w->events++;
    (void)p; (void)time;
    if (axis > 1) return;
    w->scroll[axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL ? 0 : 1] += (float)wl_fixed_to_double(value);
    w->scroll_any = true;
    if (w->seat_version < 5) cui__wl_scroll_flush(w);   /* no frame event to wait for */
}

static void cui__wl_pointer_frame(void *data, struct wl_pointer *p) { (void)p; ((cui__wl *)data)->events++; cui__wl_scroll_flush(data); }
static void cui__wl_pointer_axis_source(void *data, struct wl_pointer *p, uint32_t source) { (void)data; (void)p; (void)source; }
static void cui__wl_pointer_axis_stop(void *data, struct wl_pointer *p, uint32_t time, uint32_t axis) { (void)data; (void)p; (void)time; (void)axis; }
static void cui__wl_pointer_axis_discrete(void *data, struct wl_pointer *p, uint32_t axis, int32_t discrete) { (void)data; (void)p; (void)axis; (void)discrete; }
#ifdef WL_POINTER_AXIS_VALUE120_SINCE_VERSION
static void cui__wl_pointer_axis_value120(void *data, struct wl_pointer *p, uint32_t axis, int32_t value) {
    cui__wl *w = data;
    w->events++;
    (void)p;
    if (axis > 1) return;
    int i = axis == WL_POINTER_AXIS_HORIZONTAL_SCROLL ? 0 : 1;
    w->scroll120[i] += (float)value;
    w->scroll_has120[i] = w->scroll_any = true;
}
#endif

static const struct wl_pointer_listener cui__wl_pointer_listener = {
    cui__wl_pointer_enter, cui__wl_pointer_leave, cui__wl_pointer_motion, cui__wl_pointer_button, cui__wl_pointer_axis,
    cui__wl_pointer_frame, cui__wl_pointer_axis_source, cui__wl_pointer_axis_stop, cui__wl_pointer_axis_discrete,
#ifdef WL_POINTER_AXIS_VALUE120_SINCE_VERSION
    cui__wl_pointer_axis_value120,
#endif
};

/* ---- keyboard ---- */

static cui_key cui__wl_key(xkb_keysym_t ks) {
    if (ks >= XKB_KEY_a && ks <= XKB_KEY_z) return (cui_key)(CUI_KEY_A + (int)(ks - XKB_KEY_a));
    if (ks >= XKB_KEY_A && ks <= XKB_KEY_Z) return (cui_key)(CUI_KEY_A + (int)(ks - XKB_KEY_A));
    if (ks >= XKB_KEY_0 && ks <= XKB_KEY_9) return (cui_key)(CUI_KEY_0 + (int)(ks - XKB_KEY_0));
    if (ks >= XKB_KEY_F1 && ks <= XKB_KEY_F12) return (cui_key)(CUI_KEY_F1 + (int)(ks - XKB_KEY_F1));
    switch (ks) {
    case XKB_KEY_Tab: case XKB_KEY_ISO_Left_Tab: return CUI_KEY_TAB;
    case XKB_KEY_Return: case XKB_KEY_KP_Enter: return CUI_KEY_ENTER;
    case XKB_KEY_Escape: return CUI_KEY_ESCAPE;
    case XKB_KEY_BackSpace: return CUI_KEY_BACKSPACE;
    case XKB_KEY_Delete: case XKB_KEY_KP_Delete: return CUI_KEY_DELETE;
    case XKB_KEY_space: return CUI_KEY_SPACE;
    case XKB_KEY_Left: case XKB_KEY_KP_Left: return CUI_KEY_LEFT;
    case XKB_KEY_Right: case XKB_KEY_KP_Right: return CUI_KEY_RIGHT;
    case XKB_KEY_Up: case XKB_KEY_KP_Up: return CUI_KEY_UP;
    case XKB_KEY_Down: case XKB_KEY_KP_Down: return CUI_KEY_DOWN;
    case XKB_KEY_Home: case XKB_KEY_KP_Home: return CUI_KEY_HOME;
    case XKB_KEY_End: case XKB_KEY_KP_End: return CUI_KEY_END;
    case XKB_KEY_Page_Up: return CUI_KEY_PAGE_UP;
    case XKB_KEY_Page_Down: return CUI_KEY_PAGE_DOWN;
    case XKB_KEY_minus: return CUI_KEY_MINUS;
    case XKB_KEY_equal: return CUI_KEY_EQUAL;
    case XKB_KEY_comma: return CUI_KEY_COMMA;
    case XKB_KEY_period: return CUI_KEY_PERIOD;
    case XKB_KEY_slash: return CUI_KEY_SLASH;
    default: return CUI_KEY_NONE;
    }
}

static void cui__wl_keyboard_keymap(void *data, struct wl_keyboard *k, uint32_t format, int32_t fd, uint32_t size) {
    cui__wl *w = data;
    w->events++;
    (void)k;
    if (format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 && w->xkb) {
        char *text = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
        if (text != MAP_FAILED) {
            struct xkb_keymap *keymap = xkb_keymap_new_from_buffer(w->xkb, text, strnlen(text, size), XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
            munmap(text, size);
            if (keymap) {
                if (w->xkb_state) xkb_state_unref(w->xkb_state);
                if (w->keymap) xkb_keymap_unref(w->keymap);
                w->keymap = keymap;
                w->xkb_state = xkb_state_new(keymap);
            }
        }
    }
    close(fd);
}

static void cui__wl_keyboard_enter(void *data, struct wl_keyboard *k, uint32_t serial, struct wl_surface *surface, struct wl_array *keys) {
    cui__wl *w = data;
    w->events++;
    (void)k; (void)surface; (void)keys;
    w->input_serial = serial;
    cui_input_focus(w->app->ui, true);
}

static void cui__wl_keyboard_leave(void *data, struct wl_keyboard *k, uint32_t serial, struct wl_surface *surface) {
    cui__wl *w = data;
    w->events++;
    (void)k; (void)serial; (void)surface;
    w->repeat_key = 0;
    w->mods = 0;
    memset(w->held_key, 0, sizeof w->held_key);
    cui_input_focus(w->app->ui, false);
}

/* The key a key code stands for, by its first level, as the X11 backend reads it: Shift+1 is the
 * 1 key. A layout without Latin letters (Russian, Greek, Hebrew, Arabic) names no key CUI has for
 * a letter: the key is then what another layout of the keymap calls it, and failing that its
 * place on the keyboard, so that Ctrl+C is the same key whichever layout is active. */
static cui_key cui__wl_key_of(cui__wl *w, uint32_t code) {
    const xkb_keysym_t *syms = NULL;
    xkb_layout_index_t active = xkb_state_key_get_layout(w->xkb_state, code);
    int n = xkb_keymap_key_get_syms_by_level(w->keymap, code, active, 0, &syms);
    cui_key key = n > 0 ? cui__wl_key(syms[0]) : CUI_KEY_NONE;
    xkb_layout_index_t layouts = xkb_keymap_num_layouts_for_key(w->keymap, code);
    for (xkb_layout_index_t l = 0; !key && l < layouts; l++) {
        if (l == active) continue;
        n = xkb_keymap_key_get_syms_by_level(w->keymap, code, l, 0, &syms);
        if (n > 0) key = cui__wl_key(syms[0]);
    }
    if (!key && code >= 8) {
        /* evdev key codes of the three letter rows */
        static const struct { uint32_t first; const char *letters; } rows[] = { { 16, "qwertyuiop" }, { 30, "asdfghjkl" }, { 44, "zxcvbnm" } };
        uint32_t evdev = code - 8;
        for (size_t r = 0; r < sizeof rows / sizeof rows[0]; r++)
            if (evdev >= rows[r].first && evdev < rows[r].first + strlen(rows[r].letters))
                key = (cui_key)(CUI_KEY_A + (rows[r].letters[evdev - rows[r].first] - 'a'));
    }
    return key;
}

/* A key going down or up, or held long enough to repeat. */
static void cui__wl_key_event(cui__wl *w, uint32_t code, bool down) {
    if (!w->xkb_state) return;
    cui_key key = cui__wl_key_of(w, code);
    if (code < 256 && w->held_key[code]) {   /* a repeat or the release: the key it went down as */
        key = w->held_key[code];
        if (!down) w->held_key[code] = CUI_KEY_NONE;
    } else if (code < 256 && down) w->held_key[code] = key;
    if (key) cui_input_key(w->app->ui, key, down, w->mods);
    if (down && !(w->mods & CUI_MOD_CTRL)) {
        char text[64];
        if (xkb_state_key_get_utf8(w->xkb_state, code, text, sizeof text) > 0) cui_input_text(w->app->ui, text);
    }
}

static void cui__wl_keyboard_key(void *data, struct wl_keyboard *k, uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
    cui__wl *w = data;
    w->events++;
    (void)k; (void)time;
    uint32_t code = key + 8;
    bool down = state == WL_KEYBOARD_KEY_STATE_PRESSED;
    w->input_serial = serial;
    cui__wl_key_event(w, code, down);
    if (down && w->keymap && w->repeat_rate > 0 && xkb_keymap_key_repeats(w->keymap, code)) {
        w->repeat_key = code;
        w->repeat_at = cui__plat_now() + w->repeat_delay_ms / 1000.0;
    } else if (!down && code == w->repeat_key) {
        w->repeat_key = 0;
    }
}

static void cui__wl_keyboard_modifiers(void *data, struct wl_keyboard *k, uint32_t serial, uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group) {
    cui__wl *w = data;
    w->events++;
    (void)k; (void)serial;
    if (!w->xkb_state) return;
    xkb_state_update_mask(w->xkb_state, depressed, latched, locked, 0, 0, group);
    int mods = 0;
    if (xkb_state_mod_name_is_active(w->xkb_state, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) > 0) mods |= CUI_MOD_SHIFT;
    if (xkb_state_mod_name_is_active(w->xkb_state, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0) mods |= CUI_MOD_CTRL;
    if (xkb_state_mod_name_is_active(w->xkb_state, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE) > 0) mods |= CUI_MOD_ALT;
    if (xkb_state_mod_name_is_active(w->xkb_state, XKB_MOD_NAME_LOGO, XKB_STATE_MODS_EFFECTIVE) > 0) mods |= CUI_MOD_SUPER;
    if (mods == w->mods) return;
    bool more = (mods & ~w->mods) != 0;
    w->mods = mods;
    cui_input_key(w->app->ui, CUI_KEY_NONE, more, mods);   /* a modifier on its own, as the X11 backend reports it */
}

static void cui__wl_keyboard_repeat_info(void *data, struct wl_keyboard *k, int32_t rate, int32_t delay) {
    cui__wl *w = data;
    (void)k;
    w->repeat_rate = rate; w->repeat_delay_ms = delay;
    if (rate <= 0) w->repeat_key = 0;   /* the compositor repeats nothing: a held key is not waited on */
}

static const struct wl_keyboard_listener cui__wl_keyboard_listener = {
    cui__wl_keyboard_keymap, cui__wl_keyboard_enter, cui__wl_keyboard_leave, cui__wl_keyboard_key,
    cui__wl_keyboard_modifiers, cui__wl_keyboard_repeat_info };

/* ---- clipboard ---- */

static void cui__wl_offer_offer(void *data, struct wl_data_offer *offer, const char *mime) {
    cui__wl *w = data;
    if (offer != w->offer) return;
    for (int i = 0; i < 3; i++)
        if (!strcmp(mime, cui__wl_text_mimes[i]) && (!w->offer_mime || i + 1 < w->offer_mime)) w->offer_mime = i + 1;
}
static void cui__wl_offer_source_actions(void *data, struct wl_data_offer *offer, uint32_t actions) { (void)data; (void)offer; (void)actions; }
static void cui__wl_offer_action(void *data, struct wl_data_offer *offer, uint32_t action) { (void)data; (void)offer; (void)action; }
static const struct wl_data_offer_listener cui__wl_offer_listener = { cui__wl_offer_offer, cui__wl_offer_source_actions, cui__wl_offer_action };

static void cui__wl_device_offer(void *data, struct wl_data_device *device, struct wl_data_offer *offer) {
    cui__wl *w = data;
    (void)device;
    w->offer = offer;
    w->offer_mime = 0;
    wl_data_offer_add_listener(offer, &cui__wl_offer_listener, w);
}

static void cui__wl_device_enter(void *data, struct wl_data_device *device, uint32_t serial, struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y, struct wl_data_offer *offer) {
    cui__wl *w = data;
    (void)device; (void)serial; (void)surface; (void)x; (void)y;
    if (w->drag_offer && w->drag_offer != offer) wl_data_offer_destroy(w->drag_offer);
    w->drag_offer = offer;   /* dropped files are not taken yet: the drag is refused by not accepting it */
    if (offer == w->offer) w->offer = NULL;
}

static void cui__wl_device_leave(void *data, struct wl_data_device *device) {
    cui__wl *w = data;
    (void)device;
    if (w->drag_offer) wl_data_offer_destroy(w->drag_offer);
    w->drag_offer = NULL;
}

static void cui__wl_device_motion(void *data, struct wl_data_device *device, uint32_t time, wl_fixed_t x, wl_fixed_t y) { (void)data; (void)device; (void)time; (void)x; (void)y; }
static void cui__wl_device_drop(void *data, struct wl_data_device *device) {
    cui__wl *w = data;
    (void)device;
    if (w->drag_offer) wl_data_offer_destroy(w->drag_offer);   /* no leave follows a drop */
    w->drag_offer = NULL;
}

static void cui__wl_device_selection(void *data, struct wl_data_device *device, struct wl_data_offer *offer) {
    cui__wl *w = data;
    w->events++;
    (void)device;
    if (w->selection && w->selection != offer) wl_data_offer_destroy(w->selection);
    w->selection = offer;
    w->selection_mime = offer && offer == w->offer ? w->offer_mime : 0;
    if (offer == w->offer) w->offer = NULL;
}

static const struct wl_data_device_listener cui__wl_device_listener = {
    cui__wl_device_offer, cui__wl_device_enter, cui__wl_device_leave, cui__wl_device_motion, cui__wl_device_drop, cui__wl_device_selection };

static void cui__wl_source_target(void *data, struct wl_data_source *source, const char *mime) { (void)data; (void)source; (void)mime; }

static void cui__wl_source_send(void *data, struct wl_data_source *source, const char *mime, int32_t fd) {
    cui__wl *w = data;
    (void)source; (void)mime;
    const char *text = w->clip_owned ? w->clip_owned : "";
    size_t left = strlen(text);
    /* The reader may go away or stop reading. A write to a closed pipe raises SIGPIPE, which
     * ends the process unless it is held back here; a reader that takes nothing for a second
     * is given up on, so the app does not stop with it. */
    sigset_t pipe_set, old_set, pending;
    sigemptyset(&pipe_set);
    sigaddset(&pipe_set, SIGPIPE);
    sigpending(&pending);
    bool was_pending = sigismember(&pending, SIGPIPE) == 1;
    pthread_sigmask(SIG_BLOCK, &pipe_set, &old_set);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    double last = cui__plat_now();
    bool broke = false;
    while (left > 0) {
        ssize_t n = write(fd, text, left);
        if (n > 0) { text += n; left -= (size_t)n; last = cui__plat_now(); continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && errno == EAGAIN) {
            int ms = (int)((1.0 - (cui__plat_now() - last)) * 1000.0);
            struct pollfd pfd = { fd, POLLOUT, 0 };
            if (ms <= 0 || (poll(&pfd, 1, ms) < 0 && errno != EINTR)) break;
            continue;
        }
        broke = n < 0 && errno == EPIPE;
        break;
    }
    close(fd);
    if (broke && !was_pending) {   /* take the signal this write raised, so it is not delivered later */
        struct timespec none = { 0, 0 };
        sigtimedwait(&pipe_set, NULL, &none);
    }
    pthread_sigmask(SIG_SETMASK, &old_set, NULL);
}

static void cui__wl_source_cancelled(void *data, struct wl_data_source *source) {
    cui__wl *w = data;
    if (w->clip_source == source) w->clip_source = NULL;   /* another client's selection replaced ours */
    wl_data_source_destroy(source);
}

static void cui__wl_source_dnd_drop(void *data, struct wl_data_source *source) { (void)data; (void)source; }
static void cui__wl_source_dnd_finished(void *data, struct wl_data_source *source) { (void)data; (void)source; }
static void cui__wl_source_action(void *data, struct wl_data_source *source, uint32_t action) { (void)data; (void)source; (void)action; }

static const struct wl_data_source_listener cui__wl_source_listener = {
    cui__wl_source_target, cui__wl_source_send, cui__wl_source_cancelled,
    cui__wl_source_dnd_drop, cui__wl_source_dnd_finished, cui__wl_source_action };

static void cui__wl_set_clip(void *user, const char *utf8) {
    cui__wl *w = cui__wl_of(user);
    free(w->clip_owned);
    size_t n = strlen(utf8);
    w->clip_owned = malloc(n + 1);
    if (w->clip_owned) memcpy(w->clip_owned, utf8, n + 1);
    if (!w->data_manager || !w->data_device) return;
    if (w->clip_source) wl_data_source_destroy(w->clip_source);
    w->clip_source = wl_data_device_manager_create_data_source(w->data_manager);
    wl_data_source_add_listener(w->clip_source, &cui__wl_source_listener, w);
    for (int i = 0; i < 3; i++) wl_data_source_offer(w->clip_source, cui__wl_text_mimes[i]);
    wl_data_device_set_selection(w->data_device, w->clip_source, w->input_serial);   /* allowed after an input event */
    wl_display_flush(w->dpy);
}

static const char *cui__wl_get_clip(void *user) {
    cui__wl *w = cui__wl_of(user);
    if (w->clip_source) return w->clip_owned;
    if (!w->selection || !w->selection_mime) return NULL;
    int fds[2];
    if (pipe(fds) != 0) return NULL;
    wl_data_offer_receive(w->selection, cui__wl_text_mimes[w->selection_mime - 1], fds[1]);
    close(fds[1]);
    wl_display_flush(w->dpy);
    /* the owner writes the text and closes the pipe; half a second for one that has stopped
     * writing, counted from what came last, and no more than CUI__WL_CLIP_SECONDS or
     * CUI__WL_CLIP_BYTES in all */
    size_t len = 0, cap = 0;
    char *text = NULL;
    double start = cui__plat_now(), last = start;
    bool eof = false;   /* text only once the owner closed the pipe: a stalled or cut transfer is no paste */
    for (;;) {
        struct pollfd pfd = { fds[0], POLLIN, 0 };
        double now = cui__plat_now();
        int left = (int)((0.5 - (now - last)) * 1000.0);
        if (left <= 0 || now - start > CUI__WL_CLIP_SECONDS || len > CUI__WL_CLIP_BYTES || poll(&pfd, 1, left) <= 0) break;
        if (len + 4096 + 1 > cap) {
            cap = cap ? cap * 2 : 8192;
            char *grown = realloc(text, cap);
            if (!grown) break;
            text = grown;
        }
        ssize_t n = read(fds[0], text + len, 4096);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { eof = n == 0; break; }
        len += (size_t)n;
        last = cui__plat_now();
    }
    close(fds[0]);
    if (!eof) { free(text); text = NULL; }
    free(w->clip_read);
    w->clip_read = text;
    if (text) text[len] = 0;
    return w->clip_read;
}

/* ---- seat, outputs, registry ---- */

static void cui__wl_seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps) {
    cui__wl *w = data;
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !w->pointer) {
        w->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(w->pointer, &cui__wl_pointer_listener, w);
    } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && w->pointer) {
        cui__wl_pointer_leave(w, w->pointer, 0, NULL);   /* no release comes from a gone pointer */
        if (w->seat_version >= WL_POINTER_RELEASE_SINCE_VERSION) wl_pointer_release(w->pointer);
        else wl_pointer_destroy(w->pointer);
        w->pointer = NULL;
    }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !w->keyboard) {
        w->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(w->keyboard, &cui__wl_keyboard_listener, w);
    } else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && w->keyboard) {
        cui__wl_keyboard_leave(w, w->keyboard, 0, NULL);
        if (w->seat_version >= WL_KEYBOARD_RELEASE_SINCE_VERSION) wl_keyboard_release(w->keyboard);
        else wl_keyboard_destroy(w->keyboard);
        w->keyboard = NULL;
    }
}
static void cui__wl_seat_name(void *data, struct wl_seat *seat, const char *name) { (void)data; (void)seat; (void)name; }
static const struct wl_seat_listener cui__wl_seat_listener = { cui__wl_seat_capabilities, cui__wl_seat_name };

static cui__wl_output *cui__wl_output_of(cui__wl *w, struct wl_output *output) {
    for (int i = 0; i < w->output_count; i++) if (w->outputs[i].output == output) return &w->outputs[i];
    return NULL;
}

static void cui__wl_output_geometry(void *data, struct wl_output *o, int32_t x, int32_t y, int32_t pw, int32_t ph, int32_t subpixel, const char *make, const char *model, int32_t transform) {
    (void)data; (void)o; (void)x; (void)y; (void)pw; (void)ph; (void)subpixel; (void)make; (void)model; (void)transform;
}
static void cui__wl_output_mode(void *data, struct wl_output *o, uint32_t flags, int32_t width, int32_t height, int32_t refresh) {
    cui__wl_output *out = cui__wl_output_of(data, o);
    (void)width; (void)height;
    if (out && (flags & WL_OUTPUT_MODE_CURRENT)) out->refresh_mhz = refresh;
}
static void cui__wl_output_done(void *data, struct wl_output *o) {
    cui__wl *w = data;
    w->events++;
    (void)o;
    if (w->configured && !w->fractional) cui__wl_apply(w);   /* whole scales come from the outputs */
}
static void cui__wl_output_scale_event(void *data, struct wl_output *o, int32_t factor) {
    cui__wl_output *out = cui__wl_output_of(data, o);
    if (out) out->scale = factor;
}
static const struct wl_output_listener cui__wl_output_listener = {
    cui__wl_output_geometry, cui__wl_output_mode, cui__wl_output_done, cui__wl_output_scale_event };

static void cui__wl_surface_enter(void *data, struct wl_surface *s, struct wl_output *o) {
    cui__wl *w = data;
    w->events++;
    cui__wl_output *out = cui__wl_output_of(w, o);
    (void)s;
    if (out) out->entered = true;
    if (w->configured && !w->fractional) cui__wl_apply(w);
}
static void cui__wl_surface_leave(void *data, struct wl_surface *s, struct wl_output *o) {
    cui__wl *w = data;
    w->events++;
    cui__wl_output *out = cui__wl_output_of(w, o);
    (void)s;
    if (out) out->entered = false;
    if (w->configured && !w->fractional) cui__wl_apply(w);
}
static const struct wl_surface_listener cui__wl_surface_listener = { cui__wl_surface_enter, cui__wl_surface_leave };

static void cui__wl_wm_ping(void *data, struct xdg_wm_base *wm, uint32_t serial) { (void)data; xdg_wm_base_pong(wm, serial); }
static const struct xdg_wm_base_listener cui__wl_wm_listener = { cui__wl_wm_ping };

static void cui__wl_registry_global(void *data, struct wl_registry *registry, uint32_t name, const char *iface, uint32_t version) {
    cui__wl *w = data;
    if (!strcmp(iface, wl_compositor_interface.name)) {
        w->compositor_version = version < 4 ? version : 4;
        w->compositor = wl_registry_bind(registry, name, &wl_compositor_interface, w->compositor_version);
    } else if (!strcmp(iface, wl_subcompositor_interface.name)) {
        w->subcompositor = wl_registry_bind(registry, name, &wl_subcompositor_interface, 1);
    } else if (!strcmp(iface, wl_shm_interface.name)) {
        w->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (!strcmp(iface, wl_seat_interface.name) && !w->seat) {
#ifdef WL_POINTER_AXIS_VALUE120_SINCE_VERSION
        uint32_t newest = 8;
#else
        uint32_t newest = 5;
#endif
        w->seat_version = version < newest ? version : newest;
        w->seat = wl_registry_bind(registry, name, &wl_seat_interface, w->seat_version);
        wl_seat_add_listener(w->seat, &cui__wl_seat_listener, w);
    } else if (!strcmp(iface, xdg_wm_base_interface.name)) {
        w->wm = wl_registry_bind(registry, name, &xdg_wm_base_interface, version < 2 ? version : 2);
        xdg_wm_base_add_listener(w->wm, &cui__wl_wm_listener, w);
    } else if (!strcmp(iface, wp_viewporter_interface.name)) {
        w->viewporter = wl_registry_bind(registry, name, &wp_viewporter_interface, 1);
    } else if (!strcmp(iface, wp_fractional_scale_manager_v1_interface.name)) {
        w->fractional_manager = wl_registry_bind(registry, name, &wp_fractional_scale_manager_v1_interface, 1);
    } else if (!strcmp(iface, zxdg_decoration_manager_v1_interface.name)) {
        w->decoration_manager = wl_registry_bind(registry, name, &zxdg_decoration_manager_v1_interface, 1);
    } else if (!strcmp(iface, wl_data_device_manager_interface.name)) {
        w->data_manager = wl_registry_bind(registry, name, &wl_data_device_manager_interface, version < 3 ? version : 3);
    } else if (!strcmp(iface, wl_output_interface.name) && w->output_count < CUI__WL_OUTPUTS) {
        cui__wl_output *out = &w->outputs[w->output_count++];
        memset(out, 0, sizeof *out);
        out->name = name;
        out->scale = 1;
        out->output = wl_registry_bind(registry, name, &wl_output_interface, version < 2 ? version : 2);
        wl_output_add_listener(out->output, &cui__wl_output_listener, w);
    }
}

static void cui__wl_registry_remove(void *data, struct wl_registry *registry, uint32_t name) {
    cui__wl *w = data;
    (void)registry;
    for (int i = 0; i < w->output_count; i++) {
        if (w->outputs[i].name != name) continue;
        wl_output_destroy(w->outputs[i].output);
        w->outputs[i] = w->outputs[--w->output_count];
        break;
    }
}
static const struct wl_registry_listener cui__wl_registry_listener = { cui__wl_registry_global, cui__wl_registry_remove };

/* ---- the toplevel ---- */

static void cui__wl_toplevel_configure(void *data, struct xdg_toplevel *t, int32_t width, int32_t height, struct wl_array *states) {
    cui__wl *w = data;
    w->events++;
    (void)t;
    w->pending_w = width; w->pending_h = height;
    w->pending_maximized = w->pending_fullscreen = w->pending_tiled = w->pending_activated = false;
    const uint32_t *state;
    wl_array_for_each(state, states) {
        if (*state == XDG_TOPLEVEL_STATE_MAXIMIZED) w->pending_maximized = true;
        else if (*state == XDG_TOPLEVEL_STATE_ACTIVATED) w->pending_activated = true;
        else if (*state == XDG_TOPLEVEL_STATE_FULLSCREEN) w->pending_fullscreen = true;
        else if (*state >= XDG_TOPLEVEL_STATE_TILED_LEFT && *state <= XDG_TOPLEVEL_STATE_TILED_BOTTOM) w->pending_tiled = true;
    }
}

static void cui__wl_toplevel_close(void *data, struct xdg_toplevel *t) {
    cui__wl *w = data;
    w->events++;
    (void)t;
    w->app->running = false;
}

static const struct xdg_toplevel_listener cui__wl_toplevel_listener = { cui__wl_toplevel_configure, cui__wl_toplevel_close };

static void cui__wl_xdg_surface_configure(void *data, struct xdg_surface *xs, uint32_t serial) {
    cui__wl *w = data;
    w->events++;
    w->maximized = w->pending_maximized; w->fullscreen = w->pending_fullscreen; w->tiled = w->pending_tiled;
    /* a compositor configures a window as it minimizes it too (it is no longer the active one):
     * the window is back when it is the active one again */
    if (w->pending_activated) w->minimized = false;
    int bar = cui__wl_bar_height(w);
    bool normal = !w->maximized && !w->fullscreen && !w->tiled;
    /* each axis the compositor leaves at 0 is ours to choose: the size before it was maximized */
    if (w->pending_w > 0) w->width = w->pending_w;
    else if (normal) w->width = w->normal_w;
    if (w->pending_h > 0) w->height = w->pending_h - bar > 1 ? w->pending_h - bar : 1;
    else if (normal) w->height = w->normal_h;
    if (normal) { w->normal_w = w->width; w->normal_h = w->height; }
    xdg_surface_ack_configure(xs, serial);
    cui__wl_apply(w);
    w->configured = true;
    /* the size, geometry and bar take effect with a commit; no frame may follow if nothing in
     * the picture changed */
    if (w->app->presented_once) wl_surface_commit(w->surface);
}

static const struct xdg_surface_listener cui__wl_xdg_surface_listener = { cui__wl_xdg_surface_configure };

/* The smallest and largest size the compositor may give the window, title bar included. */
static void cui__wl_bounds(cui__wl *w) {
    const cui_app_desc *d = &w->app->desc;
    int bar = cui__wl_bar_height(w);
    if (d->fixed_size) {
        xdg_toplevel_set_min_size(w->toplevel, w->width, w->height + bar);
        xdg_toplevel_set_max_size(w->toplevel, w->width, w->height + bar);
    } else {
        xdg_toplevel_set_min_size(w->toplevel, d->min_width, d->min_height + bar);
    }
}

static void cui__wl_decoration_configure(void *data, struct zxdg_toplevel_decoration_v1 *d, uint32_t mode) {
    cui__wl *w = data;
    w->events++;
    (void)d;
    bool server = mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE;
    bool changed = server != w->server_decoration;
    w->server_decoration = server;
    if (changed && w->configured) cui__wl_bounds(w);   /* the title bar came or went: the bounds count it */
}
static const struct zxdg_toplevel_decoration_v1_listener cui__wl_decoration_listener = { cui__wl_decoration_configure };

static void cui__wl_fractional_scale(void *data, struct wp_fractional_scale_v1 *f, uint32_t scale) {
    cui__wl *w = data;
    w->events++;
    (void)f;
    if ((int)scale == w->preferred120 || scale < 60) return;
    w->preferred120 = (int)scale;
    w->shown_cursor = NULL;
    if (w->configured) {
        cui__wl_apply(w);
        if (w->app->presented_once) wl_surface_commit(w->surface);
    }
}
static const struct wp_fractional_scale_v1_listener cui__wl_fractional_listener = { cui__wl_fractional_scale };

/* ---- events ---- */

/* Reads and handles what the compositor sent. Waits up to timeout seconds (< 0: without end)
 * for news: an event for the app, or with for_frame the frame callback of the last swap. 0
 * only takes what is there. What does not end the wait: the renderer's own events (Mesa keeps
 * them on queues of its own: buffers released, frames presented), which come over the same
 * socket and are read here, and the frame callback when the app waits for input. Each would
 * start a frame, and each frame brings more of them. False when the connection is gone. */
static bool cui__wl_pump(cui__wl *w, double timeout, bool for_frame) {
    double start = cui__plat_now();
    unsigned events = w->events;
    for (;;) {
        bool news = for_frame ? !w->frame : w->events != events;
        while (wl_display_prepare_read(w->dpy) != 0)
            if (wl_display_dispatch_pending(w->dpy) < 0) return false;
        bool unsent = false;   /* the socket is full: wait until it takes more, and send the rest next time round */
        if (wl_display_flush(w->dpy) < 0) {
            if (errno != EAGAIN) { wl_display_cancel_read(w->dpy); return false; }
            unsent = true;
        }
        news = news || (for_frame ? !w->frame : w->events != events);
        double now = cui__plat_now();
        double left = timeout < 0 ? -1 : timeout - (now - start);
        int ms = news || timeout == 0 || (timeout > 0 && left <= 0) ? 0 : timeout < 0 ? -1 : (int)(left * 1000.0 + 0.999);
        if (w->repeat_key && w->repeat_rate > 0 && !news) {   /* a held key repeats without the compositor saying so */
            int until = (int)((w->repeat_at - now) * 1000.0 + 0.999);
            if (until < 0) until = 0;
            if (ms < 0 || until < ms) ms = until;
        }
        struct pollfd pfd = { wl_display_get_fd(w->dpy), (short)(POLLIN | (unsent ? POLLOUT : 0)), 0 };
        int ready = poll(&pfd, 1, ms);
        if (ready > 0 && (pfd.revents & POLLIN)) {
            if (wl_display_read_events(w->dpy) < 0) return false;
        } else {
            wl_display_cancel_read(w->dpy);
            if (ready > 0 && (pfd.revents & (POLLERR | POLLHUP))) return false;
        }
        if (wl_display_dispatch_pending(w->dpy) < 0) return false;
        if (w->repeat_key && w->repeat_rate > 0 && cui__plat_now() >= w->repeat_at) {
            cui__wl_key_event(w, w->repeat_key, true);
            w->repeat_at += 1.0 / w->repeat_rate;
            if (w->repeat_at < cui__plat_now()) w->repeat_at = cui__plat_now() + 1.0 / w->repeat_rate;   /* (after a stall: no burst) */
            w->events++;
        }
        news = for_frame ? !w->frame : w->events != events;
        if (news || timeout == 0 || (timeout > 0 && cui__plat_now() - start >= timeout)) return true;
    }
}

void cui__plat_wait(cui_app *app, double timeout) {
    cui__wl *w = cui__wl_of(app);
    if (w->sw_retry && (timeout < 0 || timeout > 0.1)) timeout = 0.1;
    if (!cui__wl_pump(w, timeout, false)) {
        if (app->running) fprintf(stderr, "cui: the Wayland compositor closed the connection\n");
        app->running = false;
    }
    if (w->sw_retry) { w->sw_retry = false; app->force_present = true; }
}

/* ---- window ---- */

static void cui__wl_destroy(cui__wl *w) {
    if (!w) return;
    for (int i = 0; i < 4; i++) cui__wl_child_free(&w->border_surface[i], &w->border_sub[i], &w->border_viewport[i]);
    cui__wl_child_free(&w->bar_surface, &w->bar_sub, &w->bar_viewport);
    cui__wl_shm_free(&w->bar_buffer);
    cui__wl_shm_free(&w->clear_pixel);
    for (int i = 0; i < 3; i++) cui__wl_shm_free(&w->sw_buffers[i]);
    if (w->frame) wl_callback_destroy(w->frame);
    if (w->cursor_surface) wl_surface_destroy(w->cursor_surface);
    if (w->cursor_theme) wl_cursor_theme_destroy(w->cursor_theme);
    if (w->clip_source) wl_data_source_destroy(w->clip_source);
    if (w->selection) wl_data_offer_destroy(w->selection);
    if (w->drag_offer) wl_data_offer_destroy(w->drag_offer);
    if (w->offer) wl_data_offer_destroy(w->offer);
    if (w->data_device) wl_data_device_destroy(w->data_device);
    if (w->decoration) zxdg_toplevel_decoration_v1_destroy(w->decoration);
    if (w->fractional) wp_fractional_scale_v1_destroy(w->fractional);
    if (w->viewport) wp_viewport_destroy(w->viewport);
    if (w->toplevel) xdg_toplevel_destroy(w->toplevel);
    if (w->xdg_surface) xdg_surface_destroy(w->xdg_surface);
    if (w->surface) wl_surface_destroy(w->surface);
    if (w->pointer) wl_pointer_destroy(w->pointer);
    if (w->keyboard) wl_keyboard_destroy(w->keyboard);
    if (w->xkb_state) xkb_state_unref(w->xkb_state);
    if (w->keymap) xkb_keymap_unref(w->keymap);
    if (w->xkb) xkb_context_unref(w->xkb);
    for (int i = 0; i < w->output_count; i++) wl_output_destroy(w->outputs[i].output);
    if (w->seat) wl_seat_destroy(w->seat);
    if (w->data_manager) wl_data_device_manager_destroy(w->data_manager);
    if (w->decoration_manager) zxdg_decoration_manager_v1_destroy(w->decoration_manager);
    if (w->fractional_manager) wp_fractional_scale_manager_v1_destroy(w->fractional_manager);
    if (w->viewporter) wp_viewporter_destroy(w->viewporter);
    if (w->wm) xdg_wm_base_destroy(w->wm);
    if (w->shm) wl_shm_destroy(w->shm);
    if (w->subcompositor) wl_subcompositor_destroy(w->subcompositor);
    if (w->compositor) wl_compositor_destroy(w->compositor);
    if (w->registry) wl_registry_destroy(w->registry);
    if (w->dpy) { wl_display_flush(w->dpy); wl_display_disconnect(w->dpy); }
    free(w->clip_owned);
    free(w->clip_read);
    free(w);
}

bool cui__plat_init(cui_app *app) {
    cui__wl *w = calloc(1, sizeof *w);
    if (!w) return false;
    w->app = app;
    cui__wl_error[0] = 0;
    w->dpy = wl_display_connect(NULL);
    if (!w->dpy) { snprintf(cui__wl_error, sizeof cui__wl_error, "no compositor to connect to"); fprintf(stderr, "cui: wayland: %s\n", cui__wl_error); free(w); return false; }
    w->xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    w->repeat_rate = 25; w->repeat_delay_ms = 400;
    w->registry = wl_display_get_registry(w->dpy);
    wl_registry_add_listener(w->registry, &cui__wl_registry_listener, w);
    wl_display_roundtrip(w->dpy);   /* the globals */
    wl_display_roundtrip(w->dpy);   /* what they say of themselves: outputs, the seat */
    if (!w->compositor || w->compositor_version < 3 || !w->wm) {   /* (3: wl_surface.set_buffer_scale) */
        snprintf(cui__wl_error, sizeof cui__wl_error, "the compositor has no %s", w->compositor_version >= 3 ? "xdg_wm_base" : "wl_compositor 3"); fprintf(stderr, "cui: wayland: %s\n", cui__wl_error);
        cui__wl_destroy(w);
        return false;
    }
    app->plat = w;
    w->width = w->normal_w = app->desc.width;
    w->height = w->normal_h = app->desc.height;
    w->scale = 1;
    w->surface = wl_compositor_create_surface(w->compositor);
    wl_surface_add_listener(w->surface, &cui__wl_surface_listener, w);
    if (w->viewporter) w->viewport = wp_viewporter_get_viewport(w->viewporter, w->surface);
    if (w->fractional_manager && w->viewport) {
        w->fractional = wp_fractional_scale_manager_v1_get_fractional_scale(w->fractional_manager, w->surface);
        wp_fractional_scale_v1_add_listener(w->fractional, &cui__wl_fractional_listener, w);
    }
    w->xdg_surface = xdg_wm_base_get_xdg_surface(w->wm, w->surface);
    xdg_surface_add_listener(w->xdg_surface, &cui__wl_xdg_surface_listener, w);
    w->toplevel = xdg_surface_get_toplevel(w->xdg_surface);
    xdg_toplevel_add_listener(w->toplevel, &cui__wl_toplevel_listener, w);
    xdg_toplevel_set_title(w->toplevel, app->title);
    /* desktops find the window's .desktop file (name, icon, dock entry) by the app ID */
    xdg_toplevel_set_app_id(w->toplevel, app->desc.app_id ? app->desc.app_id : "cui");
    if (w->decoration_manager) {
        w->decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(w->decoration_manager, w->toplevel);
        zxdg_toplevel_decoration_v1_add_listener(w->decoration, &cui__wl_decoration_listener, w);
        zxdg_toplevel_decoration_v1_set_mode(w->decoration, ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    }
    /* sizes the compositor works with are the whole window's: with the built-in title bar.
     * Whether there will be one is known after the first configure, so the bounds are set
     * then (cui__wl_bounds), and again when the decoration or a fixed window's size changes. */
    const cui_window_placement *pl = app->desc.placement;
    if (pl && pl->state == CUI_WINDOW_MAXIMIZED) xdg_toplevel_set_maximized(w->toplevel);
    if (w->seat && w->data_manager) {
        w->data_device = wl_data_device_manager_get_data_device(w->data_manager, w->seat);
        wl_data_device_add_listener(w->data_device, &cui__wl_device_listener, w);
    }
    wl_surface_commit(w->surface);   /* no buffer yet: asks for the first configure */
    for (int i = 0; i < 50 && !w->configured; i++)
        if (wl_display_roundtrip(w->dpy) < 0) break;
    if (!w->configured) {
        snprintf(cui__wl_error, sizeof cui__wl_error, "the compositor did not configure the window"); fprintf(stderr, "cui: wayland: %s\n", cui__wl_error);
        cui__wl_destroy(w);
        app->plat = NULL;
        return false;
    }
    cui__wl_bounds(w);
    cui_platform_iface pi;
    memset(&pi, 0, sizeof pi);
    pi.user = app;
    pi.get_clipboard = cui__wl_get_clip;
    pi.set_clipboard = cui__wl_set_clip;
    pi.set_cursor = cui__wl_set_cursor;
    cui_set_platform(app->ui, &pi);
    return true;
}

/* The window appears with its first picture: a Wayland surface without a buffer is not shown. */
void cui__plat_show(cui_app *app) {
    cui__wl *w = cui__wl_of(app);
    if (!w->shown) {
        w->shown = true;
        app->force_present = true;
        cui_request_redraw(app->ui);
    }
    wl_display_flush(w->dpy);
}

static void cui__wl_egl_close(cui__egl *e) {
    if (e->display) {
        e->MakeCurrent(e->display, NULL, NULL, NULL);
        if (e->surface) e->DestroySurface(e->display, e->surface);
        if (e->context) e->DestroyContext(e->display, e->context);
        e->Terminate(e->display);
    }
    e->display = NULL; e->surface = NULL; e->context = NULL;
    if (cui__wl_gl == e) cui__wl_gl = NULL;
}

void cui__plat_shutdown(cui_app *app) {
    cui__wl *w = cui__wl_of(app);
    if (!w) return;
    cui__wl_egl_close(&w->egl);
    if (w->egl_window) wl_egl_window_destroy(w->egl_window);
    cui__wl_destroy(w);
    app->plat = NULL;
}

/* ---- OpenGL ---- */

static bool cui__wl_egl_load(cui__egl *e) {
    if (!e->lib) e->lib = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (!e->lib) e->lib = dlopen("libEGL.so", RTLD_NOW | RTLD_GLOBAL);
    if (!e->lib) return false;
#define CUI__EGL(name) (*(void **)&e->name = dlsym(e->lib, "egl" #name))
    CUI__EGL(GetDisplay); CUI__EGL(GetPlatformDisplay); CUI__EGL(Initialize); CUI__EGL(BindAPI); CUI__EGL(ChooseConfig);
    CUI__EGL(CreateContext); CUI__EGL(CreateWindowSurface); CUI__EGL(MakeCurrent); CUI__EGL(SwapBuffers); CUI__EGL(SwapInterval);
    CUI__EGL(DestroySurface); CUI__EGL(DestroyContext); CUI__EGL(Terminate); CUI__EGL(GetProcAddress);
#undef CUI__EGL
    return e->GetDisplay && e->Initialize && e->BindAPI && e->ChooseConfig && e->CreateContext && e->CreateWindowSurface &&
           e->MakeCurrent && e->SwapBuffers && e->SwapInterval && e->DestroySurface && e->DestroyContext && e->Terminate &&
           e->GetProcAddress;
}

/* A context on the window: OpenGL 3.3 core, or OpenGL ES 3.0 where the driver has no desktop GL. */
static bool cui__wl_egl_context(cui__wl *w, bool es) {
    cui__egl *e = &w->egl;
    if (!e->BindAPI(es ? EGL_OPENGL_ES_API_ : EGL_OPENGL_API_)) return false;
    /* no alpha: the window is opaque, and the compositor need not blend it */
    for (int alpha = 0; alpha <= 8 && !e->context; alpha += 8) {
        int attrs[] = { EGL_SURFACE_TYPE_, EGL_WINDOW_BIT_, EGL_RENDERABLE_TYPE_, es ? EGL_OPENGL_ES3_BIT_ : EGL_OPENGL_BIT_,
                        EGL_RED_SIZE_, 8, EGL_GREEN_SIZE_, 8, EGL_BLUE_SIZE_, 8, EGL_ALPHA_SIZE_, alpha, EGL_NONE_ };
        int count = 0;
        if (!e->ChooseConfig(e->display, attrs, &e->config, 1, &count) || count < 1) continue;
        int gl[] = { EGL_CONTEXT_MAJOR_VERSION_, 3, EGL_CONTEXT_MINOR_VERSION_, 3,
                     EGL_CONTEXT_OPENGL_PROFILE_MASK_, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_, EGL_NONE_ };
        int gles[] = { EGL_CONTEXT_MAJOR_VERSION_, 3, EGL_NONE_ };
        e->context = e->CreateContext(e->display, e->config, NULL, es ? gles : gl);
    }
    if (!e->context) return false;
    e->surface = e->CreateWindowSurface(e->display, e->config, w->egl_window, NULL);
    if (!e->surface || !e->MakeCurrent(e->display, e->surface, e->surface, e->context)) {
        if (e->surface) e->DestroySurface(e->display, e->surface);
        e->DestroyContext(e->display, e->context);
        e->surface = NULL; e->context = NULL;
        return false;
    }
    return true;
}

bool cui__plat_gl_create(cui_app *app, bool vsync) {
    cui__wl *w = cui__wl_of(app);
    cui__egl *e = &w->egl;
    w->unpaced = !vsync;
    if (!cui__wl_egl_load(e)) return false;
    e->display = e->GetPlatformDisplay ? e->GetPlatformDisplay(EGL_PLATFORM_WAYLAND_, w->dpy, NULL) : NULL;
    if (!e->display) e->display = e->GetDisplay(w->dpy);
    if (!e->display || !e->Initialize(e->display, NULL, NULL)) { e->display = NULL; return false; }
    w->egl_window = wl_egl_window_create(w->surface, w->fb_w, w->fb_h);
    bool desktop = w->egl_window && cui__wl_egl_context(w, false);
    if (!w->egl_window || (!desktop && !cui__wl_egl_context(w, true))) {
        if (w->egl_window) wl_egl_window_destroy(w->egl_window);
        w->egl_window = NULL;
        e->Terminate(e->display);
        e->display = NULL;
        return false;
    }
    /* The swap never waits: a swap that waits for the compositor stops for good while the
     * window is hidden. cui__plat_gl_swap paces by the surface's frame callbacks instead. */
    e->SwapInterval(e->display, 0);
    cui__wl_gl = e;
    return true;
}

void *cui__plat_gl_proc(const char *name) {
    void *p = cui__wl_gl ? cui__wl_gl->GetProcAddress(name) : NULL;
    if (!p && cui__wl_gl) p = dlsym(cui__wl_gl->lib, name);
    return p;
}

void cui__plat_gl_release(cui_app *app) {
    cui__wl *w = cui__wl_of(app);
    cui__wl_egl_close(&w->egl);
    if (w->egl_window) wl_egl_window_destroy(w->egl_window);
    w->egl_window = NULL;
}

static void cui__wl_frame_done(void *data, struct wl_callback *callback, uint32_t time) {
    cui__wl *w = data;
    (void)time;
    if (w->frame == callback) { w->frame = NULL; w->gpu_seen = true; }
    if (w->gpu_skipped) {   /* the window is seen again: the frame that was not drawn */
        w->gpu_skipped = false;
        cui_request_redraw(w->app->ui);
        w->events++;
    }
    wl_callback_destroy(callback);
}
static const struct wl_callback_listener cui__wl_frame_listener = { cui__wl_frame_done };

/* Before a Vulkan frame: false when it must not be drawn now. A presentation that waits for
 * the display (FIFO) waits without end while the window is hidden, and the app stops answering
 * with it. So each frame asks for a frame callback of its own, and the next is drawn once it
 * came; a hidden window gets none, and is drawn again when it does (cui__wl_frame_done). */
bool cui__plat_gpu_frame(cui_app *app, int width, int height) {
    cui__wl *w = cui__wl_of(app);
    if (!w->shown) return false;
    double start = cui__plat_now();
    bool unpaced = app->desc.no_vsync || !w->gpu_seen;   /* until a callback came, none may come */
    while (w->frame && app->running && !unpaced) {
        double left = 0.25 - (cui__plat_now() - start);
        if (left <= 0) break;
        if (!cui__wl_pump(w, left, true)) { app->running = false; return false; }
    }
    if (w->frame && unpaced) { wl_callback_destroy(w->frame); w->frame = NULL; }
    if (w->frame) { w->gpu_skipped = true; return false; }
    if (width != w->fb_w || height != w->fb_h) {   /* a configure came while waiting: draw at the new size */
        cui_request_redraw(app->ui);
        return false;
    }
    if (w->gpu_started) {   /* (the first frame maps the window: nothing to wait for before it) */
        w->frame = wl_surface_frame(w->surface);
        wl_callback_add_listener(w->frame, &cui__wl_frame_listener, w);
        wl_surface_commit(w->surface);
    }
    w->gpu_started = true;
    return true;
}

void cui__plat_gl_swap(cui_app *app) {
    cui__wl *w = cui__wl_of(app);
    cui__egl *e = &w->egl;
    if (!e->surface || !w->shown) return;   /* (a hidden app draws, for screenshots, and shows nothing) */
    /* one frame per refresh: wait until the compositor has shown the last one. A hidden
     * window gets no callback: after a quarter of a second the frame goes out anyway. */
    double start = cui__plat_now();
    while (w->frame && app->running && !w->unpaced) {
        double left = 0.25 - (cui__plat_now() - start);
        if (left <= 0) break;
        if (!cui__wl_pump(w, left, true)) { app->running = false; return; }
    }
    if (w->frame) wl_callback_destroy(w->frame);
    w->frame = wl_surface_frame(w->surface);
    wl_callback_add_listener(w->frame, &cui__wl_frame_listener, w);
    e->SwapBuffers(e->display, e->surface);
}

double cui__plat_refresh_hz(cui_app *app) {
    cui__wl *w = cui__wl_of(app);
    int entered = 0, any = 0;
    for (int i = 0; i < w->output_count; i++) {
        if (w->outputs[i].refresh_mhz > any) any = w->outputs[i].refresh_mhz;
        if (w->outputs[i].entered && w->outputs[i].refresh_mhz > entered) entered = w->outputs[i].refresh_mhz;
    }
    return (entered ? entered : any) / 1000.0;
}

/* ---- software pictures ---- */

static void cui__wl_present_rect(cui_app *app, const void *bgra, int width, int height, int stride, int x, int y, int pw, int ph) {
    cui__wl *w = cui__wl_of(app);
    cui__wl_shm_buffer *b = NULL;
    for (int i = 0; i < 3 && !b; i++) if (w->sw_buffers[i].buffer && !w->sw_buffers[i].busy && w->sw_buffers[i].w == width && w->sw_buffers[i].h == height) b = &w->sw_buffers[i];
    for (int i = 0; i < 3 && !b; i++) if (!w->sw_buffers[i].busy && cui__wl_shm_make(w, &w->sw_buffers[i], width, height, false)) {
        b = &w->sw_buffers[i];
        wl_buffer_add_listener(b->buffer, &cui__wl_sw_buffer_listener, w);
    }
    if (!w->shown) return;   /* a hidden app: no picture, so no window */
    if (!b) {   /* all three are out and come back, or none could be made and a retry is due */
        bool all_out = w->sw_buffers[0].busy && w->sw_buffers[1].busy && w->sw_buffers[2].busy;
        if (all_out) w->sw_dropped = true; else w->sw_retry = true;   /* one shown is not released */
        return;
    }   /* the compositor holds all three: the whole picture goes out when one comes back */
    for (int row = 0; row < height; row++) memcpy(b->pixels + (size_t)row * width, (const uint8_t *)bgra + (size_t)row * stride, (size_t)width * 4);
    wl_surface_attach(w->surface, b->buffer, 0, 0);
    if (w->compositor_version >= 4) wl_surface_damage_buffer(w->surface, x, y, pw, ph);
    else wl_surface_damage(w->surface, 0, 0, INT32_MAX, INT32_MAX);
    wl_surface_commit(w->surface);
    b->busy = true;
    wl_display_flush(w->dpy);
}

void cui__plat_present(cui_app *app, const void *bgra, int width, int height, int stride) {
    cui__wl_present_rect(app, bgra, width, height, stride, 0, 0, width, height);
}

bool cui__plat_present_part(cui_app *app, const void *bgra, int width, int height, int stride, int x, int y, int pw, int ph) {
    cui__wl_present_rect(app, bgra, width, height, stride, x, y, pw, ph);
    return true;
}

/* ---- the rest of the interface ---- */

void cui__plat_metrics(cui_app *app, float *width, float *height, float *scale, int *fb_w, int *fb_h) {
    cui__wl *w = cui__wl_of(app);
    *scale = w->scale;
    *fb_w = w->fb_w; *fb_h = w->fb_h;
    *width = (float)w->fb_w / w->scale;
    *height = (float)w->fb_h / w->scale;
    /* asked for once a frame: the built-in title bar follows a change of theme */
    if (w->bar_surface && w->configured && cui__wl_bar_draw(w)) wl_surface_commit(w->bar_surface);
}

void cui__plat_set_title(cui_app *app, const char *title) {
    cui__wl *w = cui__wl_of(app);
    xdg_toplevel_set_title(w->toplevel, title);
    wl_display_flush(w->dpy);
}

void cui__plat_set_size(cui_app *app, int width, int height) {
    cui__wl *w = cui__wl_of(app);
    if (w->maximized || w->fullscreen || w->tiled || width < 1 || height < 1) return;   /* the compositor's size holds */
    if (!app->desc.fixed_size) {   /* no smaller than the window may be made by hand */
        if (width < app->desc.min_width) width = app->desc.min_width;
        if (height < app->desc.min_height) height = app->desc.min_height;
    }
    w->width = w->normal_w = width;
    w->height = w->normal_h = height;
    if (app->desc.fixed_size) cui__wl_bounds(w);   /* a fixed window's bounds are its size */
    cui__wl_apply(w);
    if (app->presented_once) wl_surface_commit(w->surface);
}

void *cui__plat_native(cui_app *app) { return cui__wl_of(app)->surface; }

cui_x11_window *cui__plat_x11_window(cui_app *app) {
    cui__wl *w = cui__wl_of(app);
    w->gpu_window.display = w->dpy;
    w->gpu_window.wl_surface = w->surface;
    w->gpu_window.width = w->fb_w;
    w->gpu_window.height = w->fb_h;
    return &w->gpu_window;
}

void cui__plat_maximize(cui_app *app) { xdg_toplevel_set_maximized(cui__wl_of(app)->toplevel); }

void cui__plat_minimize(cui_app *app) {
    cui__wl *w = cui__wl_of(app);
    xdg_toplevel_set_minimized(w->toplevel);
    w->minimized = true;   /* Wayland does not say when it ends: taken back when the window is active again */
}

void cui__plat_restore(cui_app *app) {
    cui__wl *w = cui__wl_of(app);
    if (w->maximized) xdg_toplevel_unset_maximized(w->toplevel);
    w->minimized = false;   /* a client cannot bring its window back from the dock */
}

cui_window_state cui__plat_window_state(cui_app *app) {
    cui__wl *w = cui__wl_of(app);
    return w->minimized ? CUI_WINDOW_MINIMIZED : w->maximized ? CUI_WINDOW_MAXIMIZED : CUI_WINDOW_NORMAL;
}

void cui__plat_set_position(cui_app *app, int x, int y) { (void)app; (void)x; (void)y; }   /* the compositor places windows */

void cui__plat_placement(cui_app *app, cui_window_placement *out) {
    cui__wl *w = cui__wl_of(app);
    out->width = w->normal_w;
    out->height = w->normal_h;
    out->has_position = false;
    out->state = w->maximized ? CUI_WINDOW_MAXIMIZED : CUI_WINDOW_NORMAL;
}

