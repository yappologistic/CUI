/* The Win32 frame of a window whose app draws the title bar (cui_app_desc.custom_title_bar):
 * no system caption, the sizing border on every side, the content's size and the smallest size,
 * a maximized window that fills the work area, and a window without the option that keeps the
 * system's title bar.
 *   cui_test_title_bar_win32
 * The window's messages are sent to it directly, so the pointer's real position does not
 * matter. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cui/cui_app.h>

#include <math.h>
#include <stdio.h>
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

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    resizable_window();
    fixed_window();
    system_title_bar();
    printf("%d failures\n", failures);
    return failures ? 1 : 0;
}
