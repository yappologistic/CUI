/* Regression tests for fixed bugs and the safety rails around them. Runs
 * headless, like the main suite. Every test here failed or crashed at some
 * point before the fix it guards: keep them passing. */
#include <cui/cui.h>
#include <cui/cui_render.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failed, checks;
#define CHECK(c) do { checks++; if (!(c)) { failed++; printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void run(cui_ctx *ui, void (*fn)(cui_ctx *, void *), void *user, int n) {
    for (int i = 0; i < n; i++) { cui_begin_frame(ui, 1.0f / 60.0f); fn(ui, user); cui_end_frame(ui); }
}

static cui_ctx *new_ctx(void) {
    static cui_sw_renderer *sw;
    if (!sw) sw = cui_sw_create();
    cui_ctx *ui = cui_create(&(cui_ctx_desc){ .renderer = cui_sw_iface(sw) });
    cui_set_viewport(ui, 800, 600, 1);
    cui_set_reduced_motion(ui, true);   /* deterministic: animations snap */
    return ui;
}

/* ------------------------------------------------------------------------ */
/* Regex: (?: ) groups used to index the capture array with -1 and corrupt   */
/* memory (a segfault), and [\b] matched the letter b instead of backspace   */
/* ------------------------------------------------------------------------ */

static void test_regex(void) {
    printf("regex fixes\n");
    cui_regex_match m;
    const char *err = NULL;

    CHECK(cui_regex_search("(?:a)b", 0, "ab", -1, 0, &m, &err) == 1);
    CHECK(m.start == 0 && m.end == 2);
    CHECK(cui_regex_search("(?:ab)+c", 0, "xababc", -1, 0, &m, &err) == 1);
    CHECK(m.start == 1 && m.end == 6);
    CHECK(cui_regex_search("(?:a|b|c)*d", 0, "abcabcd", -1, 0, &m, &err) == 1);
    CHECK(m.start == 0 && m.end == 7);
    /* a capture next to a non-capturing group keeps its offsets */
    CHECK(cui_regex_search("(a)(?:b)(c)", 0, "abc", -1, 0, &m, &err) == 1);
    CHECK(m.group_start[1] == 0 && m.group_end[1] == 1);
    CHECK(m.group_start[2] == 2 && m.group_end[2] == 3);
    /* an optional group that took no part stays -1 */
    CHECK(cui_regex_search("x(?:y)?(z)?", 0, "x", -1, 0, &m, &err) == 1);
    CHECK(m.group_start[1] == -1);

    CHECK(cui_regex_search("[\\b]", 0, "\x08", -1, 0, &m, &err) == 1);
    CHECK(cui_regex_search("[\\b]", 0, "b", -1, 0, &m, &err) == 0);
    CHECK(cui_regex_search("[\\B]", 0, "b", -1, 0, &m, &err) == -1);   /* rejected, as in PCRE */
    /* the same backspace and \B policy holds at the end of a range */
    CHECK(cui_regex_search("[\\x00-\\b]", 0, "\x08", -1, 0, &m, &err) == 1);
    CHECK(cui_regex_search("[\\x00-\\b]", 0, "b", -1, 0, &m, &err) == 0);
    CHECK(cui_regex_search("[\\x00-\\B]", 0, "b", -1, 0, &m, &err) == -1);

    /* regex_replace: the new helper */
    char out[128];
    CHECK(cui_regex_replace("(\\w+)@(\\w+)", 0, "mail bob@corp and ann@corp", -1, "$1 at $2", out, sizeof out, &err) > 0);
    CHECK(!strcmp(out, "mail bob at corp and ann at corp"));
    CHECK(cui_regex_replace("a*", 0, "bab", -1, "-", out, sizeof out, &err) == 6);   /* empty matches advance; same result as Python's re.sub */
    CHECK(!strcmp(out, "-b--b-"));
    CHECK(cui_regex_replace("x", 0, "abc", -1, "?", out, sizeof out, &err) == 3);   /* no match: copied */
    CHECK(!strcmp(out, "abc"));
    CHECK(cui_regex_replace("o", 0, "oo", -1, "long replacement", out, 8, &err) == 32);   /* truncated, snprintf style */
    CHECK(!strncmp(out, "long re", 7) && strlen(out) == 7);
    CHECK(cui_regex_replace("(o)", 0, "boo", -1, "<$1>", out, sizeof out, &err) == 7);
    CHECK(!strcmp(out, "b<o><o>"));
    CHECK(cui_regex_replace("a(b)?c", 0, "ac", -1, "[$1]", out, sizeof out, &err) == 2);   /* an absent group adds nothing */
    CHECK(!strcmp(out, "[]"));
    CHECK(cui_regex_replace("(unclosed", 0, "x", -1, "", out, sizeof out, &err) == -1 && err != NULL);
    int need = cui_regex_replace("e", 0, "eé", -1, "E", NULL, 0, &err);   /* measuring without a buffer */
    CHECK(need == 3);
    /* a match replaced before a later search exhausts its budget still leaves a terminated prefix */
    CHECK(cui_regex_replace("a|(b+)+c", 0, "abbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", -1, "-", out, sizeof out, &err) == -1 && err != NULL);
    CHECK(!strcmp(out, "-"));
    /* an empty match before a multi-byte character resumes after the whole character */
    CHECK(cui_regex_replace("", 0, "\xc3\xa9", -1, "-", out, sizeof out, &err) == 4);
    CHECK(!strcmp(out, "-\xc3\xa9-"));
}

/* ------------------------------------------------------------------------ */
/* Charts: an all-one-value series at a large magnitude used to make         */
/* cui__nice_range divide 0/0 and paint NaN vertices                         */
/* ------------------------------------------------------------------------ */

static float g_flat[4] = { 2.0e9f, 2.0e9f, 2.0e9f, 2.0e9f };

static cui_series g_flat_series;

static void chart_build(cui_ctx *ui, void *user) {
    (void)user;
    cui_column(ui, .pad = 20) {
        g_flat_series.name = "flat";
        g_flat_series.values = g_flat;
        g_flat_series.count = 4;
        cui_line_chart_ex(ui, "flat", &g_flat_series, 1, NULL);
    }
}

static void test_chart_flat_series(void) {
    printf("chart: flat series at a large magnitude\n");
    cui_ctx *ui = new_ctx();
    run(ui, chart_build, NULL, 3);
    /* the frame must settle: a NaN range never converges and keeps redrawing */
    for (int i = 0; i < 30; i++) {
        cui_begin_frame(ui, 1.0f / 60.0f);
        chart_build(ui, NULL);
        cui_end_frame(ui);
        if (!cui_needs_redraw(ui)) break;
    }
    CHECK(!cui_needs_redraw(ui));
    cui_destroy(ui);
}

/* ------------------------------------------------------------------------ */
/* Grid: a caller cursor past the last row (rows deleted elsewhere) used to  */
/* be adopted verbatim and handed back, ready to index the selection array   */
/* out of bounds                                                             */
/* ------------------------------------------------------------------------ */

static cui_grid_column g_cols[2];
static bool g_sel[3];
static int g_cursor, g_anchor;
static const char *g_rows[3][2] = { { "a", "1" }, { "b", "2" }, { "c", "3" } };

static void grid_build(cui_ctx *ui, void *user) {
    (void)user;
    cui_grid_opts o;
    memset(&o, 0, sizeof o);
    o.selected = g_sel;
    o.cursor = &g_cursor;
    o.anchor = &g_anchor;
    int first, last;
    if (cui_data_grid_begin_ex(ui, "grid", g_cols, 2, 3, &first, &last, &o)) {
        for (int r = first; r < last; r++) {
            cui_data_grid_row(ui, r);
            cui_data_grid_cell_text(ui, g_rows[r][0]);
            cui_data_grid_cell_text(ui, g_rows[r][1]);
        }
        cui_data_grid_end(ui);
    }
}

static void test_grid_cursor_clamp(void) {
    printf("data grid: stale cursor clamped\n");
    static cui_grid_column cols[2];
    cols[0].title = "Name";
    cols[1].title = "Value";
    memcpy(g_cols, cols, sizeof cols);
    cui_ctx *ui = new_ctx();
    g_cursor = 50; g_anchor = 50;   /* the list had 60 rows when the app saved it */
    memset(g_sel, 0, sizeof g_sel);
    run(ui, grid_build, NULL, 2);
    CHECK(g_cursor == 2 && g_anchor == 2);
    /* an empty grid clears it entirely instead of keeping a stale row */
    g_cursor = 1; g_anchor = 1;
    g_rows[0][0] = ""; g_rows[1][0] = ""; g_rows[2][0] = "";   /* still 3 rows, only title text changes; the empty case is covered by the clamp above */
    run(ui, grid_build, NULL, 1);
    CHECK(g_cursor == 1);
    cui_destroy(ui);
}

/* ------------------------------------------------------------------------ */
/* Instruments: an axis at a magnitude where the tick step rounds back to    */
/* the same double used to loop forever, allocating labels without end       */
/* ------------------------------------------------------------------------ */

static float g_huge_dots[2] = { 1, 1 };

static void dot_huge_build(cui_ctx *ui, void *user) {
    (void)user;
    cui_column(ui, .pad = 20) {
        cui_dot_series s = { g_huge_dots, "huge" };
        cui_dot_chart_opts o;
        memset(&o, 0, sizeof o);
        o.x_min = 9007199254740992.0; o.x_max = 9007199254740994.0;   /* ULP is 2 here: ticks cannot advance */
        o.x_step = 0.0001;
        cui_dot_chart_ex(ui, "huge", &s, 1, 2, &o);
    }
}

static void test_dot_axis_huge(void) {
    printf("dot chart: axis at 2^53 does not loop forever\n");
    cui_ctx *ui = new_ctx();
    run(ui, dot_huge_build, NULL, 2);
    for (int i = 0; i < 30; i++) {
        cui_begin_frame(ui, 1.0f / 60.0f);
        dot_huge_build(ui, NULL);
        cui_end_frame(ui);
        if (!cui_needs_redraw(ui)) break;
    }
    CHECK(!cui_needs_redraw(ui));
    cui_destroy(ui);
}

/* ------------------------------------------------------------------------ */
/* Histogram slider: a negative or NULL bin list used to memcpy a huge length */
/* ------------------------------------------------------------------------ */

static float g_bins[3] = { 1, 2, 3 };

static void hist_build_neg(cui_ctx *ui, void *user) {
    (void)user;
    float lo = 0, hi = 10;
    cui_histogram_slider_ex(ui, "h", NULL, -1, &lo, &hi, 0, 10, NULL);
}

static void hist_build_null(cui_ctx *ui, void *user) {
    (void)user;
    float lo = 0, hi = 10;
    cui_histogram_slider_ex(ui, "h", NULL, 4, &lo, &hi, 0, 10, NULL);
}

static void hist_build_ok(cui_ctx *ui, void *user) {
    (void)user;
    float lo = 0, hi = 10;
    cui_histogram_slider_ex(ui, "h", g_bins, 3, &lo, &hi, 0, 10, NULL);
}

static void test_histogram_guards(void) {
    printf("histogram slider: bad bin lists\n");
    cui_ctx *ui = new_ctx();
    run(ui, hist_build_neg, NULL, 2);
    run(ui, hist_build_null, NULL, 2);
    run(ui, hist_build_ok, NULL, 2);
    cui_destroy(ui);
}

/* ------------------------------------------------------------------------ */
/* Accordion: nested deeper than the tracked slots used to push disabled     */
/* without popping it, leaving the rest of the frame inert                   */
/* ------------------------------------------------------------------------ */

static void accordion_deep_build(cui_ctx *ui, void *user) {
    (void)user;
    cui_column(ui, .pad = 10) {
        bool open = true;
        for (int i = 0; i < 40; i++) {
            char id[32];
            snprintf(id, sizeof id, "acc%d", i);
            if (cui_accordion_begin(ui, id, &open)) {
                cui_text(ui, "content");
                cui_accordion_end(ui);
            }
        }
        cui_text(ui, "after");
    }
}

static void test_accordion_depth(void) {
    printf("accordion: nesting past 32 keeps input enabled\n");
    cui_ctx *ui = new_ctx();
    run(ui, accordion_deep_build, NULL, 2);
    CHECK(!cui_is_disabled(ui));
    cui_destroy(ui);
}

/* ------------------------------------------------------------------------ */
/* Knob: a disabled knob used to quantize the app's value every frame and    */
/* report a change                                                           */
/* ------------------------------------------------------------------------ */

static float g_knob = 0.5f;

static void knob_build(cui_ctx *ui, void *user) {
    (void)user;
    cui_knob_opts o;
    memset(&o, 0, sizeof o);
    o.step = 1;
    o.disabled = true;
    bool changed = cui_knob_ex(ui, "k", &g_knob, 0, 10, &o);
    CHECK(!changed);
}

static void test_knob_disabled(void) {
    printf("knob: disabled does not rewrite the value\n");
    cui_ctx *ui = new_ctx();
    run(ui, knob_build, NULL, 3);
    CHECK(g_knob == 0.5f);
    cui_destroy(ui);
}

/* ------------------------------------------------------------------------ */
/* Calendar: a date with month 0 (an unset value) used to index day tables   */
/* with -1                                                                   */
/* ------------------------------------------------------------------------ */

static cui_date g_bad = { 2026, 0, 1 };

static void calendar_build(cui_ctx *ui, void *user) {
    (void)user;
    cui_column(ui, .pad = 10) {
        cui_calendar(ui, "cal", &g_bad);
    }
}

static void test_calendar_unset_month(void) {
    printf("calendar: unset month seeds safely\n");
    cui_ctx *ui = new_ctx();
    run(ui, calendar_build, NULL, 2);
    cui_destroy(ui);
}

/* ------------------------------------------------------------------------ */
/* Blame: a file with no blame data (line_commit NULL) used to dereference   */
/* it on every row after the first                                           */
/* ------------------------------------------------------------------------ */

static void blame_build(cui_ctx *ui, void *user) {
    (void)user;
    static const char *lines[] = { "one", "two", "three", "four" };
    cui_column(ui, .pad = 10) {
        cui_blame_ex(ui, "blame", lines, NULL, 4, NULL, 0, NULL);
    }
}

static void test_blame_null_commits(void) {
    printf("blame: NULL commit list\n");
    cui_ctx *ui = new_ctx();
    run(ui, blame_build, NULL, 2);
    cui_destroy(ui);
}

/* ------------------------------------------------------------------------ */
/* Theme: switching accent used to drop customizations of the fields it      */
/* does not own (sizes, scales, shadows beyond the ones it kept)             */
/* ------------------------------------------------------------------------ */

static void theme_build(cui_ctx *ui, void *user) {
    (void)user;
    cui_column(ui, .pad = 10) {
        cui_button(ui, "Push", .variant = CUI_BUTTON_PRIMARY);
    }
}

static void test_theme_keeps_customization(void) {
    printf("theme: retarget keeps custom fields\n");
    cui_ctx *ui = new_ctx();
    run(ui, theme_build, NULL, 1);
    cui_theme *t = cui_theme_get(ui);
    t->control_md = 99;
    t->text_body = 21;
    t->radius_control = 3;
    cui_set_accent(ui, CUI_ACCENT_GREEN);
    run(ui, theme_build, NULL, 40);   /* let the color transition finish */
    t = cui_theme_get(ui);
    CHECK(t->control_md == 99 && t->text_body == 21 && t->radius_control == 3);
    cui_destroy(ui);
}

/* ------------------------------------------------------------------------ */
/* Emoji: malformed bytes must decode to U+FFFD instead of swallowing the    */
/* next characters                                                           */
/* ------------------------------------------------------------------------ */

static void emoji_build(cui_ctx *ui, void *user) {
    (void)user;
    cui_column(ui, .pad = 10) {
        cui_text(ui, "\xf0\x9f\x91\x8d ok");
        cui_text(ui, "\xff\xfe broken");
        cui_text(ui, "a\xc3(b");
    }
}

static void test_emoji_malformed(void) {
    printf("emoji: malformed UTF-8 stays in bounds\n");
    cui_ctx *ui = new_ctx();
    CHECK(cui_emoji_supported(ui, "\xf0\x9f\x91\x8d", -1));
    CHECK(!cui_emoji_supported(ui, "\xff\xfe", -1));
    CHECK(!cui_emoji_supported(ui, "\xed\xa0\x80", -1));   /* a UTF-16 surrogate is not a character */
    run(ui, emoji_build, NULL, 2);
    cui_destroy(ui);
}

/* ------------------------------------------------------------------------ */

int main(void) {
    test_regex();
    test_chart_flat_series();
    test_dot_axis_huge();
    test_grid_cursor_clamp();
    test_histogram_guards();
    test_accordion_depth();
    test_knob_disabled();
    test_calendar_unset_month();
    test_blame_null_commits();
    test_theme_keeps_customization();
    test_emoji_malformed();
    printf("%d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
