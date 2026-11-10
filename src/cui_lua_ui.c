/* Live widgets written in Lua (cui_script_widget): a curated ui.* table over
 * the public components, built only when CUI_LUA is ON. The bindings are the
 * public begin/end pairs; the closure sugar (ui.row(opts, fn)) is a small Lua
 * bootstrap over them. Every container begin records its end on a per-call
 * stack, so a Lua error or a forgotten _end unwinds the node tree before the
 * widget call returns, and one bad frame cannot corrupt the next.
 *
 * File-static state is deliberate: the bindings run only while a widget call
 * is on the C stack, single-threaded and balanced (the top-level call always
 * unwinds and clears), so no state survives a widget call. */
#include "cui_internal.h"
#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#define LW_MAX_OPEN 64
#define LW_TF_CAP   256
#define LW_TA_CAP   4096

static cui_ctx *lw_ui;                              /* set only during a widget call */
static int lw_guard;                                /* >0 inside a widget: blocks nested eval */
static void (*lw_end_stack[LW_MAX_OPEN])(cui_ctx *);
static int lw_n;

bool cui__lw_in_widget(void) { return lw_guard > 0; }

static cui_ctx *lw_take(lua_State *L) {
    if (!lw_ui) return luaL_error(L, "ui.* runs only inside cui_script_widget, not from the console"), NULL;
    return lw_ui;
}

/* ---- options: a Lua table becomes a C options struct through a descriptor ---- */

enum { F_INT, F_FLOAT, F_BOOL, F_STR, F_SIZE, F_DIR, F_ALIGN, F_TALIGN, F_FONT, F_TONE, F_BVAR, F_CSIZE };
typedef struct { const char *key; size_t off; uint8_t t; } lw_fld;

static const char *const lw_dirs[] = { "column", "row", "stack", "grid", NULL };
static const char *const lw_aligns[] = { "start", "center", "end", "stretch", NULL };
static const char *const lw_taligns[] = { "start", "center", "right", "left", "end", NULL };
static const char *const lw_fonts[] = { "body", "medium", "display", "mono", NULL };
static const char *const lw_tones[] = { "neutral", "info", "success", "warning", "danger",
                                        "violet", "teal", "rose", "accent", NULL };
static const char *const lw_bvars[] = { "default", "secondary", "primary", "outline", "ghost", "danger", "link", NULL };
static const char *const lw_csizes[] = { "md", "sm", "lg", NULL };

static int lw_map(lua_State *L, int idx, const char *const *names) {
    const char *s = lua_tostring(L, idx);
    if (!s) return luaL_error(L, "string expected");
    for (int k = 0; names[k]; k++) if (!strcmp(names[k], s)) return k;
    return luaL_error(L, "unknown name '%s'", s);
}

static cui_size lw_size_of(lua_State *L, int idx) {
    if (lua_type(L, idx) == LUA_TNUMBER) return (cui_size){ CUI_SIZE_FIXED, (float)lua_tonumber(L, idx) };
    const char *s = lua_tostring(L, idx);
    if (s && !strcmp(s, "fill")) return (cui_size){ CUI_SIZE_GROW, 1 };
    if (s && !strcmp(s, "fit")) return (cui_size){ CUI_SIZE_FIT, 0 };
    luaL_error(L, "a number, 'fill' or 'fit' expected");
    return (cui_size){ CUI_SIZE_FIT, 0 };
}

static void lw_fill(lua_State *L, int tbl, void *st, const lw_fld *f) {
    luaL_checktype(L, tbl, LUA_TTABLE);
    for (; f->key; f++) {
        char *dst = (char *)st + f->off;
        lua_getfield(L, tbl, f->key);
        int t = lua_type(L, -1);
        if (t == LUA_TNIL || t == LUA_TNONE) { lua_pop(L, 1); continue; }
        switch (f->t) {
            case F_INT:    *(int *)dst = (int)luaL_checkinteger(L, -1); break;
            case F_FLOAT:  *(float *)dst = (float)luaL_checknumber(L, -1); break;
            case F_BOOL:   *(bool *)dst = lua_toboolean(L, -1) != 0; break;
            case F_STR:    *(const char **)dst = luaL_checkstring(L, -1); break;
            case F_SIZE:   *(cui_size *)dst = lw_size_of(L, -1); break;
            case F_DIR:    *(int *)dst = lw_map(L, -1, lw_dirs); break;
            case F_ALIGN:  *(int *)dst = lw_map(L, -1, lw_aligns); break;
            case F_TALIGN: *(int *)dst = lw_map(L, -1, lw_taligns); break;
            case F_FONT:   *(int *)dst = lw_map(L, -1, lw_fonts); break;
            case F_TONE:   *(int *)dst = lw_map(L, -1, lw_tones); break;
            case F_BVAR:   *(int *)dst = lw_map(L, -1, lw_bvars); break;
            case F_CSIZE:  *(int *)dst = lw_map(L, -1, lw_csizes); break;
        }
        lua_pop(L, 1);
    }
}

/* build an options struct from the table at idx; absent or nil keeps the defaults */
#define LW_OPTS(type, name, idx, fields) \
    type name; memset(&name, 0, sizeof name); \
    const type *name##_p = NULL; \
    if (!lua_isnoneornil(L, (idx))) { lw_fill(L, (idx), &name, fields); name##_p = &name; }

/* ---- containers: each begin records its end, so errors and forgotten ends unwind ---- */

static const lw_fld fld_box[] = {
    { "id", offsetof(cui_box, id), F_STR }, { "dir", offsetof(cui_box, dir), F_DIR },
    { "width", offsetof(cui_box, width), F_SIZE }, { "height", offsetof(cui_box, height), F_SIZE },
    { "pad", offsetof(cui_box, pad), F_FLOAT }, { "pad_x", offsetof(cui_box, pad_x), F_FLOAT },
    { "pad_y", offsetof(cui_box, pad_y), F_FLOAT }, { "gap", offsetof(cui_box, gap), F_FLOAT },
    { "columns", offsetof(cui_box, columns), F_INT }, { "wrap", offsetof(cui_box, wrap), F_BOOL },
    { "align", offsetof(cui_box, align), F_ALIGN },
    { "min_width", offsetof(cui_box, min_width), F_FLOAT }, { "max_width", offsetof(cui_box, max_width), F_FLOAT },
    { "min_height", offsetof(cui_box, min_height), F_FLOAT }, { "max_height", offsetof(cui_box, max_height), F_FLOAT },
    { "radius", offsetof(cui_box, radius), F_FLOAT }, { "border", offsetof(cui_box, border), F_FLOAT },
    { "opacity", offsetof(cui_box, opacity), F_FLOAT },
    { "scroll_x", offsetof(cui_box, scroll_x), F_BOOL }, { "scroll_y", offsetof(cui_box, scroll_y), F_BOOL },
    { "clip", offsetof(cui_box, clip), F_BOOL }, { NULL }
};
static const lw_fld fld_card[] = {
    { "id", offsetof(cui_card_opts, id), F_STR }, { "width", offsetof(cui_card_opts, width), F_SIZE },
    { "height", offsetof(cui_card_opts, height), F_SIZE }, { "max_width", offsetof(cui_card_opts, max_width), F_FLOAT },
    { "pad", offsetof(cui_card_opts, pad), F_FLOAT }, { "pad_x", offsetof(cui_card_opts, pad_x), F_FLOAT },
    { "pad_y", offsetof(cui_card_opts, pad_y), F_FLOAT }, { "gap", offsetof(cui_card_opts, gap), F_FLOAT },
    { "dir", offsetof(cui_card_opts, dir), F_DIR }, { "align", offsetof(cui_card_opts, align), F_ALIGN },
    { "wrap", offsetof(cui_card_opts, wrap), F_BOOL }, { "hoverable", offsetof(cui_card_opts, hoverable), F_BOOL },
    { "muted", offsetof(cui_card_opts, muted), F_BOOL }, { "selected", offsetof(cui_card_opts, selected), F_BOOL },
    { NULL }
};
static const lw_fld fld_scroll[] = {
    { "id", offsetof(cui_scroll_opts, id), F_STR }, { "width", offsetof(cui_scroll_opts, width), F_SIZE },
    { "height", offsetof(cui_scroll_opts, height), F_SIZE }, { "horizontal", offsetof(cui_scroll_opts, horizontal), F_BOOL },
    { "pad", offsetof(cui_scroll_opts, pad), F_FLOAT }, { "gap", offsetof(cui_scroll_opts, gap), F_FLOAT },
    { "fade_edges", offsetof(cui_scroll_opts, fade_edges), F_BOOL }, { NULL }
};

/* room is checked before the real begin so a refused open never leaves a node */
#define LW_ROOM do { if (lw_n == LW_MAX_OPEN) return luaL_error(L, "containers are too deep"); } while (0)
static void lw_open(void (*end)(cui_ctx *)) { lw_end_stack[lw_n++] = end; }

static int lw_close(lua_State *L, const char *what) {
    if (lw_n == 0) return luaL_error(L, "ui.%s_end without ui.%s_begin", what, what);
    lw_end_stack[--lw_n](lw_ui);
    return 0;
}

static int lw_l_box_begin(lua_State *L) {
    cui_ctx *ui = lw_take(L);
    LW_OPTS(cui_box, b, 1, fld_box);
    LW_ROOM;
    cui_box_begin(ui, b_p);
    lw_open(cui_box_end);
    return 0;
}
static int lw_l_box_end(lua_State *L) { lw_take(L); return lw_close(L, "box"); }
static int lw_l_row_begin(lua_State *L) {
    cui_ctx *ui = lw_take(L);
    LW_OPTS(cui_box, b, 1, fld_box);
    LW_ROOM;
    cui_row_begin(ui, b_p);
    lw_open(cui_box_end);                  /* row and column close with the box end */
    return 0;
}
static int lw_l_row_end(lua_State *L) { lw_take(L); return lw_close(L, "row"); }
static int lw_l_column_begin(lua_State *L) {
    cui_ctx *ui = lw_take(L);
    LW_OPTS(cui_box, b, 1, fld_box);
    LW_ROOM;
    cui_column_begin(ui, b_p);
    lw_open(cui_box_end);
    return 0;
}
static int lw_l_column_end(lua_State *L) { lw_take(L); return lw_close(L, "column"); }
static int lw_l_card_begin(lua_State *L) {
    cui_ctx *ui = lw_take(L);
    LW_OPTS(cui_card_opts, c, 1, fld_card);
    LW_ROOM;
    lua_pushboolean(L, cui_card_begin(ui, c_p));
    lw_open(cui_card_end);
    return 1;
}
static int lw_l_card_end(lua_State *L) { lw_take(L); return lw_close(L, "card"); }
static int lw_l_scroll_begin(lua_State *L) {
    cui_ctx *ui = lw_take(L);
    LW_OPTS(cui_scroll_opts, s, 1, fld_scroll);
    LW_ROOM;
    cui_scroll_begin(ui, s_p);
    lw_open(cui_scroll_end);
    return 0;
}
static int lw_l_scroll_end(lua_State *L) { lw_take(L); return lw_close(L, "scroll"); }

/* ---- display ---- */

static const lw_fld fld_text[] = {
    { "font", offsetof(cui_text_opts, font), F_FONT }, { "size", offsetof(cui_text_opts, size), F_FLOAT },
    { "align", offsetof(cui_text_opts, align), F_TALIGN }, { "nowrap", offsetof(cui_text_opts, nowrap), F_BOOL },
    { "tabular", offsetof(cui_text_opts, tabular), F_BOOL }, { "width", offsetof(cui_text_opts, width), F_SIZE },
    { "line_height", offsetof(cui_text_opts, line_height), F_FLOAT }, { NULL }
};
static const lw_fld fld_badge[] = {
    { "tone", offsetof(cui_badge_opts, tone), F_TONE }, { "dot", offsetof(cui_badge_opts, dot), F_BOOL },
    { "solid", offsetof(cui_badge_opts, solid), F_BOOL }, { "icon", offsetof(cui_badge_opts, icon), F_STR },
    { "size", offsetof(cui_badge_opts, size), F_CSIZE }, { NULL }
};
static const lw_fld fld_progress[] = {
    { "indeterminate", offsetof(cui_progress_opts, indeterminate), F_BOOL },
    { "tone", offsetof(cui_progress_opts, tone), F_TONE }, { "height", offsetof(cui_progress_opts, height), F_FLOAT },
    { "label", offsetof(cui_progress_opts, label), F_STR }, { "show_value", offsetof(cui_progress_opts, show_value), F_BOOL },
    { "width", offsetof(cui_progress_opts, width), F_SIZE }, { NULL }
};

static int lw_l_text(lua_State *L) {
    cui_ctx *ui = lw_take(L);
    const char *t = luaL_checkstring(L, 1);
    LW_OPTS(cui_text_opts, o, 2, fld_text);
    cui_text_ex(ui, t, o_p);
    return 0;
}
static int lw_l_heading(lua_State *L) {
    cui_ctx *ui = lw_take(L);
    cui_heading(ui, luaL_checkstring(L, 1), CUI_CLAMP((int)luaL_optinteger(L, 2, 2), 1, 4));
    return 0;
}
static int lw_l_muted(lua_State *L) { cui_muted(lw_take(L), luaL_checkstring(L, 1)); return 0; }
static int lw_l_label(lua_State *L) { cui_label(lw_take(L), luaL_checkstring(L, 1)); return 0; }
static int lw_l_secondary(lua_State *L) { cui_secondary(lw_take(L), luaL_checkstring(L, 1)); return 0; }
static int lw_l_code(lua_State *L) { cui_code(lw_take(L), luaL_checkstring(L, 1)); return 0; }
static int lw_l_kbd(lua_State *L) { cui_kbd(lw_take(L), luaL_checkstring(L, 1)); return 0; }
static int lw_l_icon(lua_State *L) {
    cui_icon(lw_take(L), luaL_checkstring(L, 1), (float)luaL_optnumber(L, 2, 0), (cui_color){ 0 });
    return 0;
}
static int lw_l_separator(lua_State *L) { cui_separator(lw_take(L)); return 0; }
static int lw_l_spacer(lua_State *L) { cui_spacer(lw_take(L)); return 0; }
static int lw_l_gap(lua_State *L) { cui_gap(lw_take(L), (float)luaL_checknumber(L, 1)); return 0; }
static int lw_l_badge(lua_State *L) {
    cui_ctx *ui = lw_take(L);
    LW_OPTS(cui_badge_opts, o, 2, fld_badge);
    cui_badge_ex(ui, luaL_checkstring(L, 1), o_p);
    return 0;
}
static int lw_l_progress(lua_State *L) {
    cui_ctx *ui = lw_take(L);
    LW_OPTS(cui_progress_opts, o, 2, fld_progress);
    cui_progress_ex(ui, (float)luaL_checknumber(L, 1), o_p);
    return 0;
}

/* ---- controls: values pass through Lua, the C component sees a frame temp ---- */

static const lw_fld fld_button[] = {
    { "variant", offsetof(cui_button_opts, variant), F_BVAR }, { "size", offsetof(cui_button_opts, size), F_CSIZE },
    { "icon", offsetof(cui_button_opts, icon), F_STR }, { "icon_right", offsetof(cui_button_opts, icon_right), F_STR },
    { "disabled", offsetof(cui_button_opts, disabled), F_BOOL }, { "loading", offsetof(cui_button_opts, loading), F_BOOL },
    { "full_width", offsetof(cui_button_opts, full_width), F_BOOL }, { "selected", offsetof(cui_button_opts, selected), F_BOOL },
    { "tooltip", offsetof(cui_button_opts, tooltip), F_STR }, { "shortcut", offsetof(cui_button_opts, shortcut), F_STR },
    { "width", offsetof(cui_button_opts, width), F_SIZE }, { NULL }
};
static const lw_fld fld_check[] = {
    { "description", offsetof(cui_check_opts, description), F_STR },
    { "disabled", offsetof(cui_check_opts, disabled), F_BOOL },
    { "indeterminate", offsetof(cui_check_opts, indeterminate), F_BOOL }, { NULL }
};
static const lw_fld fld_slider[] = {
    { "step", offsetof(cui_slider_opts, step), F_FLOAT }, { "show_value", offsetof(cui_slider_opts, show_value), F_BOOL },
    { "format", offsetof(cui_slider_opts, format), F_STR }, { "disabled", offsetof(cui_slider_opts, disabled), F_BOOL },
    { "width", offsetof(cui_slider_opts, width), F_SIZE }, { NULL }
};
static const lw_fld fld_input[] = {
    { "label", offsetof(cui_input_opts, label), F_STR }, { "placeholder", offsetof(cui_input_opts, placeholder), F_STR },
    { "icon", offsetof(cui_input_opts, icon), F_STR }, { "hint", offsetof(cui_input_opts, hint), F_STR },
    { "error", offsetof(cui_input_opts, error), F_STR }, { "password", offsetof(cui_input_opts, password), F_BOOL },
    { "disabled", offsetof(cui_input_opts, disabled), F_BOOL }, { "clearable", offsetof(cui_input_opts, clearable), F_BOOL },
    { "autofocus", offsetof(cui_input_opts, autofocus), F_BOOL }, { "read_only", offsetof(cui_input_opts, read_only), F_BOOL },
    { "size", offsetof(cui_input_opts, size), F_CSIZE }, { "width", offsetof(cui_input_opts, width), F_SIZE },
    { "height", offsetof(cui_input_opts, height), F_FLOAT }, { "prefix", offsetof(cui_input_opts, prefix), F_STR },
    { "suffix", offsetof(cui_input_opts, suffix), F_STR }, { NULL }
};
static const lw_fld fld_toast[] = {
    { "description", offsetof(cui_toast_opts, description), F_STR }, { "tone", offsetof(cui_toast_opts, tone), F_TONE },
    { "duration", offsetof(cui_toast_opts, duration), F_FLOAT }, { "action", offsetof(cui_toast_opts, action), F_STR },
    { "show_timer", offsetof(cui_toast_opts, show_timer), F_BOOL }, { NULL }
};

static int lw_l_button(lua_State *L) {
    cui_ctx *ui = lw_take(L);
    LW_OPTS(cui_button_opts, o, 2, fld_button);
    lua_pushboolean(L, cui_button_ex(ui, luaL_checkstring(L, 1), o_p));
    return 1;
}
static int lw_l_toggle(lua_State *L, bool (*fn)(cui_ctx *, const char *, bool *, const cui_check_opts *)) {
    cui_ctx *ui = lw_take(L);
    const char *label = luaL_checkstring(L, 1);
    bool v = lua_toboolean(L, 2) != 0;
    LW_OPTS(cui_check_opts, o, 3, fld_check);
    fn(ui, label, &v, o_p);
    lua_pushboolean(L, v);
    return 1;
}
static int lw_l_checkbox(lua_State *L) { return lw_l_toggle(L, cui_checkbox_ex); }
static int lw_l_switch(lua_State *L) { return lw_l_toggle(L, cui_switch_ex); }

static int lw_l_slider(lua_State *L) {
    cui_ctx *ui = lw_take(L);
    const char *id = luaL_checkstring(L, 1);
    float v = (float)luaL_optnumber(L, 2, 0), lo = 0, hi = 1;
    LW_OPTS(cui_slider_opts, o, 3, fld_slider);
    if (!lua_isnoneornil(L, 3)) {
        lua_getfield(L, 3, "min"); if (!lua_isnil(L, -1)) lo = (float)lua_tonumber(L, -1); lua_pop(L, 1);
        lua_getfield(L, 3, "max"); if (!lua_isnil(L, -1)) hi = (float)lua_tonumber(L, -1); lua_pop(L, 1);
    }
    cui_slider_ex(ui, id, &v, lo, hi, o_p);
    lua_pushnumber(L, v);
    return 1;
}
static int lw_l_rating(lua_State *L) {
    cui_ctx *ui = lw_take(L);
    int v = (int)luaL_optinteger(L, 2, 0), max = (int)luaL_optinteger(L, 3, 5);
    cui_rating(ui, luaL_checkstring(L, 1), &v, max);
    lua_pushinteger(L, v);
    return 1;
}

/* Single-line fields keep the value in Lua and the working buffer in C.
 * If the user typed last frame the buffer rules; otherwise a changed Lua
 * value (the console can assign it) overwrites the buffer. */
static int lw_l_input(lua_State *L, bool area) {
    cui_ctx *ui = lw_take(L);
    const char *id = luaL_checkstring(L, 1);
    const char *arg = luaL_checkstring(L, 2);
    char key[80];
    snprintf(key, sizeof key, "%s:%s", area ? "ta" : "tf", id);
    cui_id fid = cui_make_id(ui, key);
    int r = 0;
    LW_OPTS(cui_input_opts, o, 3, fld_input);
    if (area) {
        typedef struct { char buf[LW_TA_CAP]; bool have, edited; } ta;
        ta *st = cui_state(ui, fid, sizeof *st);
        if (!st->have) { snprintf(st->buf, sizeof st->buf, "%s", arg); st->have = true; }
        else if (!st->edited && strcmp(st->buf, arg)) snprintf(st->buf, sizeof st->buf, "%s", arg);
        r = cui_textarea_ex(ui, id, st->buf, sizeof st->buf, o_p);
        st->edited = (r & CUI_CHANGED) != 0;
        if (r & (CUI_CHANGED | CUI_SUBMITTED)) lua_pushstring(L, st->buf);
        else lua_pushvalue(L, 2);
    } else {
        typedef struct { char buf[LW_TF_CAP]; bool have, edited; } tf;
        tf *st = cui_state(ui, fid, sizeof *st);
        if (!st->have) { snprintf(st->buf, sizeof st->buf, "%s", arg); st->have = true; }
        else if (!st->edited && strcmp(st->buf, arg)) snprintf(st->buf, sizeof st->buf, "%s", arg);
        r = cui_input_ex(ui, id, st->buf, sizeof st->buf, o_p);
        st->edited = (r & CUI_CHANGED) != 0;
        if (r & (CUI_CHANGED | CUI_SUBMITTED)) lua_pushstring(L, st->buf);
        else lua_pushvalue(L, 2);
    }
    lua_pushboolean(L, (r & CUI_SUBMITTED) != 0);
    return 2;
}
static int lw_l_textfield(lua_State *L) { return lw_l_input(L, false); }
static int lw_l_textarea(lua_State *L) { return lw_l_input(L, true); }

static int lw_l_toast(lua_State *L) {
    cui_ctx *ui = lw_take(L);
    LW_OPTS(cui_toast_opts, o, 2, fld_toast);
    lua_pushinteger(L, cui_toast_ex(ui, luaL_checkstring(L, 1), o_p));
    return 1;
}

/* ---- charts: series as {name, values, color} tables over the public chart ---- */

static const lw_fld fld_chart[] = {
    { "width", offsetof(cui_chart_opts, width), F_SIZE }, { "height", offsetof(cui_chart_opts, height), F_FLOAT },
    { "area", offsetof(cui_chart_opts, area), F_BOOL }, { "no_grid", offsetof(cui_chart_opts, no_grid), F_BOOL },
    { "no_legend", offsetof(cui_chart_opts, no_legend), F_BOOL }, { "smooth", offsetof(cui_chart_opts, smooth), F_BOOL },
    { "format", offsetof(cui_chart_opts, format), F_STR }, { "min", offsetof(cui_chart_opts, min), F_FLOAT },
    { "max", offsetof(cui_chart_opts, max), F_FLOAT }, { NULL }
};

#define LW_CHART_MAX 8                                          /* series per chart */
#define LW_CHART_LABELS 64                                      /* x-axis labels */

static cui_color lw_color_of(lua_State *L, int idx) {
    float c[4] = { 0.5f, 0.5f, 0.5f, 1 };
    for (int ch = 1; ch <= 4; ch++) {
        lua_geti(L, idx, ch);
        if (!lua_isnil(L, -1)) c[ch - 1] = (float)luaL_checknumber(L, -1);
        lua_pop(L, 1);
    }
    return (cui_color){ (uint8_t)(255 * cui__clampf(c[0], 0, 1) + 0.5f), (uint8_t)(255 * cui__clampf(c[1], 0, 1) + 0.5f),
                        (uint8_t)(255 * cui__clampf(c[2], 0, 1) + 0.5f), (uint8_t)(255 * cui__clampf(c[3], 0, 1) + 0.5f) };
}

static int lw_l_line_chart(lua_State *L) {
    cui_ctx *ui = lw_take(L);
    const char *id = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    int n = (int)luaL_len(L, 2);
    luaL_argcheck(L, n <= LW_CHART_MAX, 2, "too many series");
    cui_series s[LW_CHART_MAX];
    for (int k = 0; k < n; k++) {
        memset(&s[k], 0, sizeof s[k]);
        lua_geti(L, 2, k + 1);
        luaL_checktype(L, -1, LUA_TTABLE);
        lua_getfield(L, -1, "name");
        s[k].name = lua_isnil(L, -1) ? NULL : luaL_checkstring(L, -1);      /* lives while the table is on the stack */
        lua_pop(L, 1);
        lua_getfield(L, -1, "values");
        luaL_checktype(L, -1, LUA_TTABLE);
        int m = (int)luaL_len(L, -1);
        float *v = cui__frame_alloc(ui, sizeof(float) * (size_t)(m > 0 ? m : 1));   /* the chart copies it at build */
        for (int j = 1; j <= m; j++) { lua_geti(L, -1, j); v[j - 1] = (float)luaL_checknumber(L, -1); lua_pop(L, 1); }
        s[k].values = v; s[k].count = m;
        lua_pop(L, 1);
        lua_getfield(L, -1, "color");
        if (lua_istable(L, -1)) s[k].color = lw_color_of(L, lua_gettop(L));
        lua_pop(L, 2);                                                      /* color (maybe) and the series table */
    }
    LW_OPTS(cui_chart_opts, o, 3, fld_chart);
    if (!lua_isnoneornil(L, 3)) {                                           /* labels: an array of strings in the opts */
        lua_getfield(L, 3, "labels");
        if (lua_istable(L, -1)) {
            int nl = (int)luaL_len(L, -1);
            luaL_argcheck(L, nl <= LW_CHART_LABELS, 3, "too many labels");
            static const char *labels[LW_CHART_LABELS];
            for (int j = 1; j <= nl; j++) {
                lua_geti(L, -1, j);
                labels[j - 1] = luaL_checkstring(L, -1);
                lua_pop(L, 1);
            }
            o.labels = labels; o.label_count = nl;
            o_p = &o;
        }
        lua_pop(L, 1);
    }
    cui_line_chart_ex(ui, id, s, n, o_p);
    return 0;
}

/* ---- install: the raw table plus the Lua bootstrap that adds the closures ---- */

static const luaL_Reg lw_raw[] = {
    { "box_begin", lw_l_box_begin }, { "box_end", lw_l_box_end },
    { "row_begin", lw_l_row_begin }, { "row_end", lw_l_row_end },
    { "column_begin", lw_l_column_begin }, { "column_end", lw_l_column_end },
    { "card_begin", lw_l_card_begin }, { "card_end", lw_l_card_end },
    { "scroll_begin", lw_l_scroll_begin }, { "scroll_end", lw_l_scroll_end },
    { "text", lw_l_text }, { "heading", lw_l_heading }, { "muted", lw_l_muted },
    { "label", lw_l_label }, { "secondary", lw_l_secondary }, { "code", lw_l_code },
    { "kbd", lw_l_kbd }, { "icon", lw_l_icon }, { "separator", lw_l_separator },
    { "spacer", lw_l_spacer }, { "gap", lw_l_gap }, { "badge", lw_l_badge },
    { "progress", lw_l_progress }, { "button", lw_l_button }, { "checkbox", lw_l_checkbox },
    { "switch", lw_l_switch }, { "slider", lw_l_slider }, { "rating", lw_l_rating },
    { "textfield", lw_l_textfield }, { "textarea", lw_l_textarea }, { "toast", lw_l_toast },
    { "line_chart", lw_l_line_chart },
    { NULL, NULL }
};

static const char *const lw_boot =
    "__cui_self = {}\n"
    "ui = {}\n"
    "local raw = __cui\n"
    "for _, n in ipairs{ 'text', 'heading', 'muted', 'label', 'secondary', 'code', 'kbd', 'icon',"
    " 'separator', 'spacer', 'gap', 'badge', 'progress', 'button', 'checkbox', 'switch',"
    " 'slider', 'rating', 'textfield', 'textarea', 'toast', 'line_chart' } do ui[n] = raw[n] end\n"
    "for _, n in ipairs{ 'row_begin', 'row_end', 'column_begin', 'column_end', 'box_begin', 'box_end',"
    " 'card_begin', 'card_end', 'scroll_begin', 'scroll_end' } do ui[n] = raw[n] end\n"
    "local function scoped(b, e)\n"
    "  return function(opts, fn)\n"
    "    if type(opts) == 'function' then fn, opts = opts, nil end\n"
    "    local r = raw[b](opts)\n"
    "    local ok, err = pcall(fn)\n"
    "    raw[e]()\n"
    "    if not ok then error(err, 0) end\n"
    "    return r\n"
    "  end\n"
    "end\n"
    "ui.row = scoped('row_begin', 'row_end')\n"
    "ui.column = scoped('column_begin', 'column_end')\n"
    "ui.box = scoped('box_begin', 'box_end')\n"
    "ui.card = scoped('card_begin', 'card_end')\n"
    "ui.scroll = scoped('scroll_begin', 'scroll_end')\n"
    "widget = function(f)\n"
    "  if f == nil then __cui_widget = nil return end\n"
    "  if type(f) ~= 'function' then error('widget(f) wants a function or nil', 2) end\n"
    "  __cui_widget = f\n"
    "end\n";

void cui__lua_ui_install(cui_script *s) {
    lua_State *L = (lua_State *)cui__script_lua_impl(s);
    if (!L) return;
    luaL_newlib(L, lw_raw);
    lua_setglobal(L, "__cui");
    if (luaL_dostring(L, lw_boot)) {
        lua_pop(L, 1);   /* the console still works; the widgets will report no ui table */
    }
}

/* ---- the widget call ---- */

typedef struct { char err[256]; } cui__lw_state;

void cui_script_widget(cui_ctx *ui, const char *id_str, cui_script *s, const char *fn) {
    int first = ui->node_count;
    if (!s || !fn || !fn[0]) {
        int t = cui__text_node(ui, "Build with CUI_LUA=ON to run Lua.", -1, CUI_FONT_BODY,
                               cui__fs(ui, ui->theme.text_sm), CUI_C(ui, TEXT_MUTED), true);
        ui->nodes[t].pad[0] = ui->nodes[t].pad[1] = ui->nodes[t].pad[2] = ui->nodes[t].pad[3] = 12;
        cui__keep_ltr(ui, first);
        return;
    }
    lua_State *L = (lua_State *)cui__script_lua_impl(s);
    cui__lw_state *st = cui_state(ui, cui_make_id(ui, id_str), sizeof *st);
    st->err[0] = 0;

    /* self: one persistent table per widget, in the script's __cui_self */
    int base = lua_gettop(L);
    lua_getglobal(L, "__cui_self");
    lua_pushfstring(L, "%s\1%s", id_str, fn);
    if (!lua_istable(L, base + 1)) { lua_settop(L, base); snprintf(st->err, sizeof st->err, "ui table was removed"); goto done; }
    lua_pushvalue(L, base + 2);
    lua_gettable(L, base + 1);
    if (lua_isnil(L, base + 3)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, base + 2);   /* key */
        lua_pushvalue(L, base + 3);   /* value */
        lua_settable(L, base + 1);    /* __cui_self[key] = self */
    }
    int self_idx = base + 3;

    lua_getglobal(L, "__cui_widget");   /* widget(f) from the console replaces the slot */
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        lua_getglobal(L, fn);
    }
    if (!lua_isfunction(L, -1)) {
        snprintf(st->err, sizeof st->err, "no Lua function '%s'", fn);
        lua_settop(L, 0);
        goto done;
    }
    lua_pushstring(L, fn);                   /* the script can call _G[__cui_slot] to wrap the app's own */
    lua_setglobal(L, "__cui_slot");
    lua_getglobal(L, "ui");
    lua_pushvalue(L, self_idx);              /* the call is fn(ui, self) */

    lw_ui = ui;
    lw_guard++;
    int open_before = lw_n;
    cui_push_id(ui, id_str);
    int err = lua_pcall(L, 2, 0, 0);
    cui_pop_id(ui);
    if (err) {
        const char *m = lua_tostring(L, -1);
        snprintf(st->err, sizeof st->err, "%s", m ? m : "(error)");
        lua_pop(L, 1);
    }
    /* close every container the call left open, newest first */
    int left = lw_n - open_before;
    while (lw_n > open_before) lw_end_stack[--lw_n](ui);
    lw_guard--;
    lw_ui = NULL;
    if (!err && left > 0) snprintf(st->err, sizeof st->err, "%d container(s) left open (begin without end)", left);
    lua_settop(L, 0);                        /* self, key and __cui_self */

done:
    if (lua_gettop(L) > 0) lua_settop(L, 0);
    if (st->err[0]) {
        int t = cui__text_node(ui, st->err, -1, CUI_FONT_BODY, cui__fs(ui, ui->theme.text_sm), CUI_C(ui, DANGER), true);
        ui->nodes[t].pad[0] = ui->nodes[t].pad[2] = 6;
    }
    cui__keep_ltr(ui, first);
}
