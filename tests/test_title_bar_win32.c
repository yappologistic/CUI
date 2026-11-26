/* The Win32 frame of a window whose app draws the title bar (cui_app_desc.custom_title_bar):
 * no system caption, the sizing border on every side, the content's size and the smallest size,
 * a maximized window that fills the work area, and a window without the option that keeps the
 * system's title bar. With cui_title_bar drawn: the caption moves the window and a double click
 * maximizes it, the maximize button is the snap layouts' target, and the window buttons work.
 *   cui_test_title_bar_win32
 * The window's messages are sent to it directly, so the pointer's real position does not
 * matter. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cui/cui_app.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void expect(bool ok, const char *what) {
    printf("%s: %s\n", ok ? "ok" : "fail", what);
    if (!ok) failures++;
}

/* Draws one frame of the test window; a frame without cui_title_bar has no caption. */
static bool step(cui_app *app) {
    if (!cui_app_frame(app)) return false;
    cui_ctx *ui = cui_app_ui(app);
    cui_column(ui, .pad = 16) {
        cui_text(ui, "title bar frame test");
    }
    cui_request_wakeup(ui, 0.02f);
    return true;
}

static void settle(cui_app *app, double seconds) {
    double until = cui_time(cui_app_ui(app)) + seconds;
    while (cui_time(cui_app_ui(app)) < until && step(app)) {}
}

static bool reach(cui_app *app, cui_window_state want) {
    double until = cui_time(cui_app_ui(app)) + 2.0;
    while (cui_app_window_state(app) != want && cui_time(cui_app_ui(app)) < until && step(app)) {}
    settle(app, 0.3);
    return cui_app_window_state(app) == want;
}

/* The content in screen pixels. */
static RECT content(HWND hwnd) {
    RECT r;
    GetClientRect(hwnd, &r);
    MapWindowPoints(hwnd, NULL, (POINT *)&r, 2);
    return r;
}

static int hit(HWND hwnd, LONG x, LONG y) {
    return (int)SendMessageW(hwnd, WM_NCHITTEST, 0, MAKELPARAM((WORD)(SHORT)x, (WORD)(SHORT)y));
}

static void expect_size(cui_app *app, float w, float h, const char *what) {
    cui_vec2 s = cui_app_size(app);
    char line[160];
    snprintf(line, sizeof line, "%s: content %.1fx%.1f, expected %.0fx%.0f", what, s.x, s.y, w, h);
    expect(fabsf(s.x - w) <= 1 && fabsf(s.y - h) <= 1, line);
}

/* The sizing border all around, the top one inside the content where the caption was. */
static void expect_edges(HWND hwnd) {
    RECT r;
    GetWindowRect(hwnd, &r);
    LONG x[] = { r.left + 1, (r.left + r.right) / 2, r.right - 2 };
    LONG y[] = { r.top + 1, (r.top + r.bottom) / 2, r.bottom - 2 };
    const int want[3][3] = {
        { HTTOPLEFT, HTTOP, HTTOPRIGHT }, { HTLEFT, HTCLIENT, HTRIGHT }, { HTBOTTOMLEFT, HTBOTTOM, HTBOTTOMRIGHT }
    };
    for (int row = 0; row < 3; row++) {
        for (int col = 0; col < 3; col++) {
            int got = hit(hwnd, x[col], y[row]);
            char line[96];
            snprintf(line, sizeof line, "edge %d,%d: hit %d, expected %d", row, col, got, want[row][col]);
            expect(got == want[row][col], line);
        }
    }
}

static cui_app *open_window(bool custom, bool fixed) {
    cui_window_placement at = { .x = 100, .y = 120, .width = 560, .height = 320, .has_position = true };
    return cui_app_create(&(cui_app_desc){ .title = "CUI title bar frame test", .placement = &at,
                                           .custom_title_bar = custom, .fixed_size = fixed });
}

static void resizable_window(void) {
    cui_app *app = open_window(true, false);
    if (!app) { expect(false, "open a window with custom_title_bar"); return; }
    HWND hwnd = cui_app_native_window(app);
    settle(app, 0.3);
    expect_size(app, 560, 320, "custom title bar");
    float scale = cui_dpi_scale(cui_app_ui(app));

    RECT outer, in = content(hwnd);
    GetWindowRect(hwnd, &outer);
    expect(in.top == outer.top, "no system caption: the content starts at the window's top");
    expect(in.left > outer.left && in.right < outer.right && in.bottom < outer.bottom,
           "the sizing border stays on the sides and the bottom");
    expect_edges(hwnd);
    LONG below_edge = in.top + (LONG)lroundf(20 * scale);
    expect(hit(hwnd, (in.left + in.right) / 2, below_edge) == HTCLIENT,
           "without cui_title_bar the top of the content is content");

    MINMAXINFO mm;
    memset(&mm, 0, sizeof mm);
    SendMessageW(hwnd, WM_GETMINMAXINFO, 0, (LPARAM)&mm);
    LONG frame_w = (outer.right - outer.left) - (in.right - in.left), frame_h = (outer.bottom - outer.top) - (in.bottom - in.top);
    expect(mm.ptMinTrackSize.x == (LONG)(360 * scale) + frame_w && mm.ptMinTrackSize.y == (LONG)(280 * scale) + frame_h,
           "the smallest size is min_width by min_height of content");

    cui_app_set_size(app, 500, 300);
    settle(app, 0.3);
    expect_size(app, 500, 300, "set_size");
    cui_app_set_size(app, 560, 320);
    settle(app, 0.3);
    in = content(hwnd);

    cui_app_maximize(app);
    expect(reach(app, CUI_WINDOW_MAXIMIZED), "maximize");
    MONITORINFO mi = { .cbSize = sizeof mi };
    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    RECT full = content(hwnd);
    expect(EqualRect(&full, &mi.rcWork), "maximized, the content fills the work area");
    expect(hit(hwnd, (full.left + full.right) / 2, full.top + 1) == HTCLIENT, "maximized, the top edge does not size");

    cui_app_restore(app);
    expect(reach(app, CUI_WINDOW_NORMAL), "restore from maximized");
    RECT back = content(hwnd);
    expect(EqualRect(&back, &in), "restored, the content is where it was");

    cui_app_minimize(app);
    expect(reach(app, CUI_WINDOW_MINIMIZED), "minimize");
    cui_app_restore(app);
    expect(reach(app, CUI_WINDOW_NORMAL), "restore from minimized");
    back = content(hwnd);
    expect(EqualRect(&back, &in), "restored from minimized, the content is where it was");
    cui_app_destroy(app);
}

static void fixed_window(void) {
    cui_app *app = open_window(true, true);
    if (!app) { expect(false, "open a fixed-size window with custom_title_bar"); return; }
    HWND hwnd = cui_app_native_window(app);
    settle(app, 0.3);
    expect_size(app, 560, 320, "fixed size with custom title bar");
    RECT outer, in = content(hwnd);
    GetWindowRect(hwnd, &outer);
    expect(in.top == outer.top, "fixed size: no system caption");
    int top = hit(hwnd, (in.left + in.right) / 2, in.top + 1);
    expect(top == HTCLIENT, "fixed size: the top edge does not size");
    cui_app_destroy(app);
}

static void system_title_bar(void) {
    cui_app *app = open_window(false, false);
    if (!app) { expect(false, "open a window without custom_title_bar"); return; }
    HWND hwnd = cui_app_native_window(app);
    settle(app, 0.3);
    expect_size(app, 560, 320, "system title bar");
    RECT outer, in = content(hwnd);
    GetWindowRect(hwnd, &outer);
    expect(in.top - outer.top >= GetSystemMetrics(SM_CYCAPTION), "without the option the system's caption stays");
    expect(hit(hwnd, (in.left + in.right) / 2, (outer.top + in.top) / 2) == HTCAPTION,
           "without the option the caption moves the window");
    cui_app_destroy(app);
}

/* A window that draws cui_title_bar: a segment over a sidebar and the main one with a control.
 * Its frames are stepped by hand after each message is sent, so the real pointer elsewhere on the
 * desktop cannot slip a leave in between a press and its release. */
typedef struct {
    cui_app *app;
    HWND hwnd;
    cui_rect tool;
    int clicks;
} bar_window;

static void bar_frame(bar_window *w) {
    cui_ctx *ui = cui_app_ui(w->app);
    cui_vec2 size = cui_app_size(w->app);
    RECT r;
    GetClientRect(w->hwnd, &r);
    if (size.x <= 0 || size.y <= 0) return;
    cui_set_viewport(ui, size.x, size.y, (float)r.right / size.x);
    cui_begin_frame(ui, 1.0f / 60);
    cui_row(ui, .width = CUI_FILL, .height = CUI_FILL) {
        cui_column(ui, .width = CUI_PX(140), .height = CUI_FILL) {
            cui_title_bar(ui, "side", .no_window_buttons = true) {}
        }
        cui_column(ui, .width = CUI_FILL, .height = CUI_FILL) {
            cui_title_bar(ui, "main", .title = "Title bar test") {
                cui_spacer(ui);
                if (cui_button(ui, "Tool", .size = CUI_SIZE_SM)) w->clicks++;
                w->tool = cui_last_rect(ui);
            }
        }
    }
    cui_end_frame(ui);
}

static void bar_settle(bar_window *w) {
    for (int i = 0; i < 3; i++) bar_frame(w);
}

/* Window points to client pixels, and client pixels to the screen. */
static POINT bar_point(bar_window *w, float x, float y) {
    float scale = cui_dpi_scale(cui_app_ui(w->app));
    return (POINT){ (LONG)lroundf(x * scale), (LONG)lroundf(y * scale) };
}
static POINT on_screen(HWND hwnd, POINT p) {
    ClientToScreen(hwnd, &p);
    return p;
}
static int bar_hit(bar_window *w, POINT p) {
    POINT s = on_screen(w->hwnd, p);
    return hit(w->hwnd, s.x, s.y);
}

static void send_mouse(bar_window *w, UINT msg, WPARAM wp, POINT p) {
    SendMessageW(w->hwnd, msg, wp, MAKELPARAM((WORD)(SHORT)p.x, (WORD)(SHORT)p.y));
    bar_frame(w);
}

/* Presses and releases at a client point: the maximize button gets the non-client messages
 * Windows sends there, everything else the client ones. */
static void bar_click(bar_window *w, POINT p) {
    if (bar_hit(w, p) == HTMAXBUTTON) {
        POINT s = on_screen(w->hwnd, p);
        send_mouse(w, WM_NCMOUSEMOVE, HTMAXBUTTON, s);
        send_mouse(w, WM_NCLBUTTONDOWN, HTMAXBUTTON, s);
    } else {
        send_mouse(w, WM_MOUSEMOVE, 0, p);
        send_mouse(w, WM_LBUTTONDOWN, MK_LBUTTON, p);
    }
    send_mouse(w, WM_LBUTTONUP, 0, p);
    bar_frame(w);
}

/* The middle of the maximize button, found through the snap layouts' target as Windows finds
 * it; *minimize gets the middle of the button before it. x is -1 without a maximize button. */
static POINT find_maximize(bar_window *w, POINT *minimize) {
    RECT r;
    GetClientRect(w->hwnd, &r);
    POINT p = bar_point(w, 0, 20);
    LONG right = -1;
    for (p.x = r.right - 1; p.x >= 0; p.x--) {
        bool on = bar_hit(w, p) == HTMAXBUTTON;
        if (on && right < 0) right = p.x;
        if (!on && right >= 0) {
            LONG width = right - p.x;
            if (minimize) *minimize = (POINT){ p.x - width / 2, p.y };
            p.x = p.x + 1 + width / 2;
            return p;
        }
    }
    p.x = -1;
    return p;
}

static void double_click_caption(bar_window *w, POINT p) {
    int area = bar_hit(w, p);
    expect(area == HTCAPTION, "double click: on the caption");
    POINT s = on_screen(w->hwnd, p);
    if (area == HTCAPTION) SendMessageW(w->hwnd, WM_NCLBUTTONDBLCLK, HTCAPTION, MAKELPARAM((WORD)(SHORT)s.x, (WORD)(SHORT)s.y));
    bar_settle(w);
}

/* The window buttons in a resizable window: minimize, maximize and restore, then a double click
 * on either segment. */
static void use_window_buttons(bar_window *w, POINT side, POINT caption) {
    POINT minimize = { -1, 0 };
    POINT maximize = find_maximize(w, &minimize);
    expect(maximize.x >= 0, "the maximize button is the snap layouts' target (HTMAXBUTTON)");
    if (maximize.x >= 0) {
        bar_click(w, minimize);
        expect(IsIconic(w->hwnd) != 0, "minimize button");
        cui_app_restore(w->app);
        bar_settle(w);
        bar_click(w, maximize);
        expect(IsZoomed(w->hwnd) != 0, "maximize button");
        bar_settle(w);
        maximize = find_maximize(w, NULL);
        if (maximize.x >= 0) bar_click(w, maximize);
        expect(!IsZoomed(w->hwnd), "restore button");
        bar_settle(w);
    }
    double_click_caption(w, side);
    expect(IsZoomed(w->hwnd) != 0, "a double click on the sidebar's segment maximizes");
    double_click_caption(w, caption);
    expect(!IsZoomed(w->hwnd), "a double click on the main segment restores");
}

/* Close turns red under the pointer, and a click on it closes the window. */
static void close_window(bar_window *w) {
    POINT close = bar_point(w, cui_app_size(w->app).x - 12, 18);
    send_mouse(w, WM_MOUSEMOVE, 0, close);
    RECT r;
    GetClientRect(w->hwnd, &r);
    unsigned char *pixels = malloc((size_t)r.right * r.bottom * 4);
    bool shot = pixels && cui_app_screenshot(w->app, pixels, r.right, r.bottom);
    expect(shot, "screenshot of the window");
    if (shot) {
        const unsigned char *px = pixels + 4 * ((size_t)close.y * r.right + close.x);
        expect(px[0] > px[1] + 40 && px[0] > px[2] + 40, "close is red under the pointer");
    }
    free(pixels);
    bar_click(w, close);
    expect(!cui_app_frame(w->app), "the close button closes the window");
}

static void title_bar_window(bool fixed) {
    bar_window w = {0};
    cui_window_placement at = { .x = 100, .y = 120, .width = 560, .height = 320, .has_position = true };
    w.app = cui_app_create(&(cui_app_desc){ .title = "CUI title bar test", .placement = &at,
                                             .custom_title_bar = true, .fixed_size = fixed, .continuous = true });
    if (!w.app) { expect(false, "open a window that draws cui_title_bar"); return; }
    w.hwnd = cui_app_native_window(w.app);
    cui_set_reduced_motion(cui_app_ui(w.app), true);
    bar_settle(&w);
    printf("%s window with cui_title_bar\n", fixed ? "fixed-size" : "resizable");

    POINT side = bar_point(&w, 70, 20), caption = bar_point(&w, 300, 20);
    POINT tool = bar_point(&w, w.tool.x + w.tool.w / 2, w.tool.y + w.tool.h / 2);
    expect(bar_hit(&w, side) == HTCAPTION && bar_hit(&w, caption) == HTCAPTION, "the segments' empty space is the caption");
    expect(w.tool.w > 0 && bar_hit(&w, tool) == HTCLIENT, "a control in the bar is content");
    expect(bar_hit(&w, bar_point(&w, 70, 80)) == HTCLIENT, "below the bar is content");

    send_mouse(&w, WM_MOUSEMOVE, 0, tool);
    send_mouse(&w, WM_LBUTTONDOWN, MK_LBUTTON, tool);
    send_mouse(&w, WM_MOUSEMOVE, MK_LBUTTON, caption);
    expect(bar_hit(&w, caption) == HTCLIENT, "while a control is held, the caption does not move the window");
    send_mouse(&w, WM_LBUTTONUP, 0, caption);
    bar_settle(&w);
    expect(w.clicks == 0 && bar_hit(&w, caption) == HTCAPTION, "released away from the control: no click, the caption again");
    bar_click(&w, tool);
    expect(w.clicks == 1, "a click on the control");

    if (fixed) {
        expect(find_maximize(&w, NULL).x < 0, "fixed size: no maximize button");
        expect(bar_hit(&w, bar_point(&w, 70, 1)) == HTCAPTION, "fixed size: the top edge is caption, not a sizing border");
        double_click_caption(&w, side);
        expect(!IsZoomed(w.hwnd), "fixed size: a double click does not maximize");
    } else {
        use_window_buttons(&w, side, caption);
    }
    close_window(&w);
    cui_app_destroy(w.app);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    resizable_window();
    fixed_window();
    system_title_bar();
    title_bar_window(false);
    title_bar_window(true);
    printf("%d failures\n", failures);
    return failures ? 1 : 0;
}
