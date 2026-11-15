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

enum { F_INT, F_FLOAT, F_BOOL, F_STR, F_SIZE, F_DIR, F_ALIGN, F_TALIGN, F_FONT, F_TONE, F_BVAR,
       F_CSIZE, F_ENUM, F_COLOR, F_U32 };
typedef struct { const char *key; size_t off; uint8_t t; const char *const *names; } lw_fld;

static const char *const lw_dirs[] = { "column", "row", "stack", "grid", NULL };
static const char *const lw_aligns[] = { "start", "center", "end", "stretch", NULL };
static const char *const lw_taligns[] = { "start", "center", "right", "left", "end", NULL };
static const char *const lw_fonts[] = { "body", "medium", "display", "mono", NULL };
static const char *const lw_tones[] = { "neutral", "info", "success", "warning", "danger",
                                        "violet", "teal", "rose", "accent", NULL };
static const char *const lw_bvars[] = { "default", "secondary", "primary", "outline", "ghost", "danger", "link", NULL };
static const char *const lw_csizes[] = { "md", "sm", "lg", NULL };
static const char *const lw_spinners[] = { "arc", "comet", "dot_matrix", "fill_drain", "radar", "ripple",
                                           "upload", "download", "transfer", "orbit", "bounce", "scan",
                                           "pulse", "spiral", "plasma", NULL };
static const char *const lw_waits[] = { "idle", "busy", "success", "error", NULL };
static const char *const lw_tagstates[] = { "pending", "in_progress", "submitted", "in_review",
                                            "success", "failed", "expired", "custom", NULL };
static const char *const lw_vstates[] = { "idle", "listening", "thinking", "speaking", "muted", NULL };
static const char *const lw_vowners[] = { "you", "reply", NULL };
static const char *const lw_gtracks[] = { "hue", "temperature", "opacity", NULL };
static const char *const lw_faligns[] = { "left", "center", "right", "justify", NULL };
static const char *const lw_flists[] = { "none", "bullets", "numbers", "checks", NULL };
static const char *const lw_tbars[] = { "plain", "grouped", "floating", NULL };

static int lw_map(lua_State *L, int idx, const char *const *names) {
    const char *s = lua_tostring(L, idx);
    if (!s) return luaL_error(L, "string expected");
    for (int k = 0; names[k]; k++) if (!strcmp(names[k], s)) return k;
    return luaL_error(L, "unknown name '%s'", s);
}

static cui_color lw_color_of(lua_State *L, int idx);   /* defined with the charts */

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
            case F_ENUM:   *(int *)dst = lw_map(L, -1, f->names); break;
            case F_COLOR:  *(cui_color *)dst = lw_color_of(L, -1); break;
            case F_U32:    *(uint32_t *)dst = (uint32_t)luaL_checkinteger(L, -1); break;
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

/* ---- shared: Lua arrays become frame-temp C arrays, and user-flipped bools persist ---- */

#define LW_ITEMS   32                                           /* strings of a picker or a toolbar */
#define LW_PARTS   12                                           /* meter parts */
#define LW_USERS   16                                           /* people in a live layer */
#define LW_LEVELS  4096                                         /* voice levels of one call */
#define LW_PAL     16                                           /* palette swatches */
#define LW_STOPS   16                                           /* gradient stops */
#define LW_TOGGLES 8                                            /* canvas toggles */
#define LW_ACTIONS 8                                            /* selection bar actions */

static const char *const *lw_str_array(lua_State *L, int idx, int cap, int *n_out) {
    luaL_checktype(L, idx, LUA_TTABLE);
    int n = (int)luaL_len(L, idx);
    luaL_argcheck(L, n <= cap, idx, "too many items");
    const char **a = cui__frame_alloc(lw_ui, sizeof(char *) * (size_t)(n > 0 ? n : 1));
    for (int k = 0; k < n; k++) {
        lua_geti(L, idx, k + 1);
        a[k] = luaL_checkstring(L, -1);                         /* alive: the table on the stack holds it */
        lua_pop(L, 1);
    }
    *n_out = n;
    return (const char *const *)a;
}

static float *lw_levels(lua_State *L, int idx, int cap, int *n_out) {
    luaL_checktype(L, idx, LUA_TTABLE);
    int n = (int)luaL_len(L, idx);
    luaL_argcheck(L, n <= cap, idx, "too many levels");
    float *v = cui__frame_alloc(lw_ui, sizeof(float) * (size_t)(n > 0 ? n : 1));
    for (int k = 0; k < n; k++) { lua_geti(L, idx, k + 1); v[k] = (float)luaL_checknumber(L, -1); lua_pop(L, 1); }
    *n_out = n;
    return v;
}

static uint8_t *lw_owners(lua_State *L, int idx, int n) {        /* "you" or "reply" for each level */
    uint8_t *o = cui__frame_alloc(lw_ui, (size_t)(n > 0 ? n : 1));
    for (int k = 0; k < n; k++) {
        lua_geti(L, idx, k + 1);
        o[k] = (uint8_t)(lw_map(L, -1, lw_vowners) == CUI_VOICE_REPLY ? 1 : 0);
        lua_pop(L, 1);
    }
    return o;
}

/* One persistent bool the user can flip (a veil, a mute, a tool button): what Lua passed
 * last frame is remembered, so a value the script changed wins over the stale one while
 * the user has not touched it this frame. */
typedef struct { bool v, have, edited, last; } lw_flag;

static lw_flag *lw_flag_state(cui_ctx *ui, const char *key) {
    return cui_state(ui, cui_make_id(ui, key), sizeof(lw_flag));
}

static void lw_flag_sync(lua_State *L, int idx, lw_flag *st) {
    if (!lua_isboolean(L, idx)) { if (!st->have) st->have = true; return; }
    bool want = lua_toboolean(L, idx) != 0;
    if (!st->have) { st->v = want; st->have = true; }
    else if (!st->edited && want != st->last) st->v = want;
    st->last = want;
}

/* ---- loading and waiting ---- */

static const lw_fld fld_spinner[] = {
    { "style", offsetof(cui_spinner_opts, style), F_ENUM, lw_spinners },
    { "color", offsetof(cui_spinner_opts, color), F_COLOR }, { "track", offsetof(cui_spinner_opts, track), F_COLOR },
    { NULL }
};
static const lw_fld fld_skelgroup[] = {
    { "dither", offsetof(cui_skeleton_group_opts, dither), F_BOOL },
    { "width", offsetof(cui_skeleton_group_opts, width), F_SIZE },
    { "gap", offsetof(cui_skeleton_group_opts, gap), F_FLOAT }, { NULL }
};
static const lw_fld fld_sspinner[] = {
    { "size", offsetof(cui_status_spinner_opts, size), F_FLOAT },
    { "color", offsetof(cui_status_spinner_opts, color), F_COLOR },
    { "keep", offsetof(cui_status_spinner_opts, keep), F_BOOL }, { NULL }
};
static const lw_fld fld_pmeter[] = {
    { "total", offsetof(cui_pattern_meter_opts, total), F_FLOAT },
    { "label", offsetof(cui_pattern_meter_opts, label), F_STR }, { "unit", offsetof(cui_pattern_meter_opts, unit), F_STR },
    { "format", offsetof(cui_pattern_meter_opts, format), F_STR },
    { "free_label", offsetof(cui_pattern_meter_opts, free_label), F_STR },
    { "grayscale", offsetof(cui_pattern_meter_opts, grayscale), F_BOOL },
    { "no_legend", offsetof(cui_pattern_meter_opts, no_legend), F_BOOL },
    { "height", offsetof(cui_pattern_meter_opts, height), F_FLOAT },
    { "width", offsetof(cui_pattern_meter_opts, width), F_SIZE }, { NULL }
};

static int lw_l_spinner(lua_State *L) {
    cui_ctx *ui = lw_take(L);
    LW_OPTS(cui_spinner_opts, o, 2, fld_spinner);
    cui_spinner_ex(ui, (float)luaL_checknumber(L, 1), o_p);
    return 0;
}
static int lw_l_skeleton(lua_State *L) {
    lw_take(L);
    cui_skeleton(lw_ui, lw_size_of(L, 1), (float)luaL_checknumber(L, 2));
    return 0;
}
static int lw_l_skelgroup_begin(lua_State *L) {
    cui_ctx *ui = lw_take(L);
    LW_OPTS(cui_skeleton_group_opts, o, 1, fld_skelgroup);
    LW_ROOM;
    cui_skeleton_group_begin_ex(ui, o_p);
    lw_open(cui_skeleton_group_end);
    return 0;
}
static int lw_l_skelgroup_end(lua_State *L) { lw_take(L); return lw_close(L, "skeleton_group"); }
static int lw_l_status_spinner(lua_State *L) {
    cui_ctx *ui = lw_take(L);
    int s = lw_map(L, 2, lw_waits);
    LW_OPTS(cui_status_spinner_opts, o, 3, fld_sspinner);
    lua_pushboolean(L, cui_status_spinner_ex(ui, luaL_checkstring(L, 1), (cui_status)s, o_p));
    return 1;
}
static int lw_l_pattern_meter(lua_State *L) {
    cui_ctx *ui = lw_take(L);
    const char *id = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    int n = (int)luaL_len(L, 2);
    luaL_argcheck(L, n <= LW_PARTS, 2, "too many parts");
    cui_meter_part *p = cui__frame_alloc(ui, sizeof *p * (size_t)(n > 0 ? n : 1));
    for (int k = 0; k < n; k++) {
        memset(&p[k], 0, sizeof p[k]);
        lua_geti(L, 2, k + 1);
        luaL_checktype(L, -1, LUA_TTABLE);
        lua_getfield(L, -1, "name");
        p[k].name = lua_isnil(L, -1) ? NULL : luaL_checkstring(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, -1, "value");
        p[k].value = (float)luaL_checknumber(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, -1, "color");
        if (lua_istable(L, -1)) p[k].color = lw_color_of(L, lua_gettop(L));
        lua_pop(L, 1);
        lua_getfield(L, -1, "density");
        if (!lua_isnil(L, -1)) p[k].density = (float)lua_tonumber(L, -1);
        lua_pop(L, 2);                                                        /* density (maybe) and the part */
    }
    LW_OPTS(cui_pattern_meter_opts, o, 3, fld_pmeter);
    lua_pushinteger(L, cui_pattern_meter_ex(ui, id, p, n, o_p));
    return 1;
}

/* ---- voice ---- */

static const lw_fld fld_vwave[] = {
    { "clip", offsetof(cui_voice_wave_opts, clip), F_BOOL },
    { "owner", offsetof(cui_voice_wave_opts, owner), F_ENUM, lw_vowners },
    { "color", offsetof(cui_voice_wave_opts, color), F_COLOR },
    { "pitch", offsetof(cui_voice_wave_opts, pitch), F_FLOAT },
    { "step", offsetof(cui_voice_wave_opts, step), F_FLOAT },
    { "width", offsetof(cui_voice_wave_opts, width), F_SIZE }, { NULL }
};
static const lw_fld fld_vnote[] = {
    { "mine", offsetof(cui_voice_note_opts, mine), F_BOOL },
    { "width", offsetof(cui_voice_note_opts, width), F_SIZE }, { NULL }
};
static const lw_fld fld_vsession[] = {
    { "label", offsetof(cui_voice_session_opts, label), F_STR },
    { "transcript", offsetof(cui_voice_session_opts, transcript), F_STR },
    { "upcoming", offsetof(cui_voice_session_opts, upcoming), F_STR },
    { "speaker", offsetof(cui_voice_session_opts, speaker), F_ENUM, lw_vowners },
    { "you", offsetof(cui_voice_session_opts, you), F_STR },
    { "assistant", offsetof(cui_voice_session_opts, assistant), F_STR },
    { "no_end", offsetof(cui_voice_session_opts, no_end), F_BOOL },
    { "width", offsetof(cui_voice_session_opts, width), F_SIZE }, { NULL }
};

typedef struct { float p, last; bool have, edited; } lw_prog;

static int lw_l_voice_wave(lua_State *L) {
    cui_ctx *ui = lw_take(L);
    const char *id = luaL_checkstring(L, 1);
    int n;
    float *lv = lw_levels(L, 2, LW_LEVELS, &n);
    LW_OPTS(cui_voice_wave_opts, o, 3, fld_vwave);
    lw_prog *pr = NULL;
    if (!lua_isnoneornil(L, 3)) {
        lua_getfield(L, 3, "progress");
        if (lua_isnumber(L, -1)) {
            char key[128];
            snprintf(key, sizeof key, "vw:%s", id);
            pr = cui_state(ui, cui_make_id(ui, key), sizeof *pr);
            float want = (float)lua_tonumber(L, -1);
            if (!pr->have) { pr->p = want; pr->have = true; }
            else if (!pr->edited && want != pr->last) pr->p = want;
            pr->last = want;
            o.progress = &pr->p;
            o_p = &o;
        }
        lua_pop(L, 1);
        lua_getfield(L, 3, "owners");
        if (lua_istable(L, -1)) { o.owners = lw_owners(L, lua_gettop(L), n); o_p = &o; }
        lua_pop(L, 1);
    }
    bool seeked = cui_voice_wave_ex(ui, id, lv, n, o_p);
    lua_pushboolean(L, seeked);
    if (pr) { pr->edited = seeked; lua_pushnumber(L, pr->p); }
    else lua_pushnil(L);
    return 2;
}

static int lw_l_voice_note(lua_State *L) {
    cui_ctx *ui = lw_take(L);
    const char *id = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    cui_voice_note note;
    memset(&note, 0, sizeof note);
    lua_getfield(L, 2, "levels");
    if (lua_istable(L, -1)) {
        int n;
        note.levels = lw_levels(L, lua_gettop(L), LW_LEVELS, &n);
        note.count = n;
    }
    lua_pop(L, 1);
    lua_getfield(L, 2, "position");
    if (lua_isnumber(L, -1)) note.position = (float)lua_tonumber(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 2, "duration");
    if (lua_isnumber(L, -1)) note.duration = (float)lua_tonumber(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 2, "speed");
    if (lua_isnumber(L, -1)) note.speed = (float)lua_tonumber(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 2, "playing");
    note.playing = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    LW_OPTS(cui_voice_note_opts, o, 3, fld_vnote);
    int r = cui_voice_note_ex(ui, id, &note, o_p);
    lua_pushinteger(L, r);
    lua_newtable(L);
    lua_pushnumber(L, note.position); lua_setfield(L, -2, "position");
    lua_pushnumber(L, note.duration); lua_setfield(L, -2, "duration");
    lua_pushnumber(L, note.speed); lua_setfield(L, -2, "speed");
    lua_pushboolean(L, note.playing); lua_setfield(L, -2, "playing");
    return 2;
}

static int lw_l_voice_session(lua_State *L) {
    cui_ctx *ui = lw_take(L);
    const char *id = luaL_checkstring(L, 1);
    int stt = lw_map(L, 2, lw_vstates);
    int n;
    float *lv = lw_levels(L, 3, LW_LEVELS, &n);
    LW_OPTS(cui_voice_session_opts, o, 4, fld_vsession);
    lw_flag *mute = NULL;
    if (!lua_isnoneornil(L, 4)) {
        lua_getfield(L, 4, "owners");
        if (lua_istable(L, -1)) { o.owners = lw_owners(L, lua_gettop(L), n); o_p = &o; }
        lua_pop(L, 1);
        lua_getfield(L, 4, "muted");
        if (!lua_isnil(L, -1)) {
            char key[128];
            snprintf(key, sizeof key, "vs:%s", id);
            mute = lw_flag_state(ui, key);
            lw_flag_sync(L, lua_gettop(L), mute);
            o.muted = &mute->v;
            o_p = &o;
        }
        lua_pop(L, 1);
    }
    int r = cui_voice_session_ex(ui, id, (cui_voice_state)stt, lv, n, o_p);
    lua_pushinteger(L, r);
    if (mute) { mute->edited = (r & CUI_VOICE_SESSION_MUTE) != 0; lua_pushboolean(L, mute->v); }
    else lua_pushnil(L);
    return 2;
}

static int lw_l_voice_resample(lua_State *L) {
    cui_ctx *ui = lw_take(L);
    int n;
    float *in = lw_levels(L, 1, LW_LEVELS, &n);
    int cols = (int)luaL_checkinteger(L, 2);
    luaL_argcheck(L, cols > 0 && cols <= LW_LEVELS, 2, "columns out of range");
    float *out = cui__frame_alloc(ui, sizeof(float) * (size_t)cols);
    int m = cui_voice_resample(in, n, out, cols);
    lua_createtable(L, m, 0);
    for (int k = 0; k < m; k++) { lua_pushnumber(L, out[k]); lua_seti(L, -2, k + 1); }
    return 1;
}

/* ---- veil ---- */

static const lw_fld fld_veil[] = {
    { "label", offsetof(cui_veil_opts, label), F_STR }, { "hint", offsetof(cui_veil_opts, hint), F_STR },
    { "toggle", offsetof(cui_veil_opts, toggle), F_BOOL }, { "radius", offsetof(cui_veil_opts, radius), F_FLOAT },
    { "color", offsetof(cui_veil_opts, color), F_COLOR }, { "width", offsetof(cui_veil_opts, width), F_SIZE },
    { NULL }
};

static int lw_l_veil_begin(lua_State *L) {
    cui_ctx *ui = lw_take(L);
    const char *id = luaL_checkstring(L, 1);
    LW_OPTS(cui_veil_opts, o, 3, fld_veil);
    LW_ROOM;
    char key[128];
    snprintf(key, sizeof key, "veil:%s", id);
    lw_flag *st = lw_flag_state(ui, key);
    lw_flag_sync(L, 2, st);
    bool changed = cui_veil_begin_ex(ui, id, &st->v, o_p);
    st->edited = changed;
    lw_open(cui_veil_end);
    lua_pushboolean(L, changed);
    lua_pushboolean(L, st->v);
    return 2;
}
static int lw_l_veil_end(lua_State *L) { lw_take(L); return lw_close(L, "veil"); }

/* ---- toolbar ---- */

static const lw_fld fld_toolbar[] = {
    { "no_border", offsetof(cui_toolbar_opts, no_border), F_BOOL },
    { "style", offsetof(cui_toolbar_opts, style), F_ENUM, lw_tbars }, { NULL }
};
static const lw_fld fld_tool[] = {
    { "label", offsetof(cui_tool_opts, label), F_STR }, { "tooltip", offsetof(cui_tool_opts, tooltip), F_STR },
    { "shortcut", offsetof(cui_tool_opts, shortcut), F_STR },
    { "disabled", offsetof(cui_tool_opts, disabled), F_BOOL },
    { "tone", offsetof(cui_tool_opts, tone), F_TONE }, { "flyout", offsetof(cui_tool_opts, flyout), F_BOOL },
    { NULL }
};

static int lw_l_toolbar_begin(lua_State *L) {
    cui_ctx *ui = lw_take(L);
    const char *id = luaL_checkstring(L, 1);
    LW_OPTS(cui_toolbar_opts, o, 2, fld_toolbar);
    LW_ROOM;
    cui_toolbar_begin(ui, id, o_p);
    lw_open(cui_toolbar_end);
    return 0;
}
static int lw_l_toolbar_end(lua_State *L) { lw_take(L); return lw_close(L, "toolbar"); }
static int lw_l_toolbar_button(lua_State *L) {
    cui_ctx *ui = lw_take(L);
    const char *icon = lua_isnoneornil(L, 1) ? NULL : luaL_checkstring(L, 1);
    LW_OPTS(cui_tool_opts, o, 2, fld_tool);
    lw_flag *fl = NULL;
    if (!lua_isnoneornil(L, 2)) {
        lua_getfield(L, 2, "toggle");
        if (!lua_isnil(L, -1)) {
            char key[160];
            snprintf(key, sizeof key, "tool:%s|%s", icon ? icon : "", o.label ? o.label : "");
            fl = lw_flag_state(ui, key);
            lw_flag_sync(L, lua_gettop(L), fl);
            o.toggle = &fl->v;
            o_p = &o;
        }
        lua_pop(L, 1);
    }
    bool r = cui_toolbar_button_ex(ui, icon, o_p);
    lua_pushboolean(L, r);
    if (fl) { fl->edited = r; lua_pushboolean(L, fl->v); return 2; }
    return 1;
}
static int lw_l_toolbar_separator(lua_State *L) { cui_toolbar_separator(lw_take(L)); return 0; }
static int lw_l_toolbar_spacer(lua_State *L) { cui_toolbar_spacer(lw_take(L)); return 0; }
static int lw_l_toolbar_group_begin(lua_State *L) {
    cui_ctx *ui = lw_take(L);
    LW_ROOM;
    cui_toolbar_group_begin(ui);
    lw_open(cui_toolbar_group_end);
    return 0;
}
static int lw_l_toolbar_group_end(lua_State *L) { lw_take(L); return lw_close(L, "toolbar_group"); }
static int lw_l_toolbar_radio(lua_State *L) {
    cui_ctx *ui = lw_take(L);
    const char *id = luaL_checkstring(L, 1);
    int n;
    const char *const *icons = lw_str_array(L, 2, LW_ITEMS, &n);
    int idx = (int)luaL_checkinteger(L, 3);
    const char *const *labels = NULL;
    int nl = 0;
    if (!lua_isnoneornil(L, 4)) {
        lua_getfield(L, 4, "labels");
        if (lua_istable(L, -1)) labels = lw_str_array(L, lua_gettop(L), LW_ITEMS, &nl);
        lua_pop(L, 1);
    }
    cui_toolbar_radio_opts o;
    memset(&o, 0, sizeof o);
    const cui_toolbar_radio_opts *o_p = NULL;
    if (labels) { o.labels = labels; o_p = &o; }
    cui_toolbar_radio_ex(ui, id, icons, n, &idx, o_p);
    lua_pushinteger(L, idx);
    return 1;
}
static int lw_l_toolbar_primary(lua_State *L) {
    cui_ctx *ui = lw_take(L);
    const char *icon = lua_isnoneornil(L, 1) ? NULL : luaL_checkstring(L, 1);
    LW_OPTS(cui_tool_opts, o, 2, fld_tool);
    lua_pushboolean(L, cui_toolbar_primary_ex(ui, icon, o_p));
    return 1;
}
static int lw_l_toolbar_cycle(lua_State *L) {
    cui_ctx *ui = lw_take(L);
    const char *id = luaL_checkstring(L, 1);
    int n;
    const char *const *items = lw_str_array(L, 2, LW_ITEMS, &n);
    int idx = (int)luaL_checkinteger(L, 3);
    LW_OPTS(cui_tool_opts, o, 4, fld_tool);
    cui_toolbar_cycle_ex(ui, id, items, n, &idx, o_p);
    lua_pushinteger(L, idx);
    return 1;
}
static int lw_l_flyout_mark(lua_State *L) { cui_flyout_mark(lw_take(L)); return 0; }

/* ---- tags, status and creative controls ---- */

static const lw_fld fld_stag[] = {
    { "size", offsetof(cui_status_tag_opts, size), F_CSIZE },
    { "removable", offsetof(cui_status_tag_opts, removable), F_BOOL },
    { "icon", offsetof(cui_status_tag_opts, icon), F_STR }, { "tone", offsetof(cui_status_tag_opts, tone), F_TONE },
    { "id", offsetof(cui_status_tag_opts, id), F_STR }, { NULL }
};
static const lw_fld fld_ratio[] = {
    { "tone", offsetof(cui_ratio_slider_opts, tone), F_TONE },
    { "step", offsetof(cui_ratio_slider_opts, step), F_FLOAT },
    { "height", offsetof(cui_ratio_slider_opts, height), F_FLOAT },
    { "width", offsetof(cui_ratio_slider_opts, width), F_SIZE },
    { "disabled", offsetof(cui_ratio_slider_opts, disabled), F_BOOL }, { NULL }
};
static const lw_fld fld_grad[] = {
    { "track", offsetof(cui_gradient_slider_opts, track), F_ENUM, lw_gtracks },
    { "color", offsetof(cui_gradient_slider_opts, color), F_COLOR },
    { "step", offsetof(cui_gradient_slider_opts, step), F_FLOAT },
    { "format", offsetof(cui_gradient_slider_opts, format), F_STR },
    { "value", offsetof(cui_gradient_slider_opts, value), F_STR },
    { "swatch", offsetof(cui_gradient_slider_opts, swatch), F_BOOL },
    { "tool", offsetof(cui_gradient_slider_opts, tool), F_STR },
    { "tool_tooltip", offsetof(cui_gradient_slider_opts, tool_tooltip), F_STR },
    { "tool_on", offsetof(cui_gradient_slider_opts, tool_on), F_BOOL },
    { "plain", offsetof(cui_gradient_slider_opts, plain), F_BOOL },
    { "disabled", offsetof(cui_gradient_slider_opts, disabled), F_BOOL }, { NULL }
};
static const lw_fld fld_stepsl[] = {
    { "format", offsetof(cui_step_slider_opts, format), F_STR },
    { "value", offsetof(cui_step_slider_opts, value), F_STR },
    { "tool", offsetof(cui_step_slider_opts, tool), F_STR },
    { "tool_tooltip", offsetof(cui_step_slider_opts, tool_tooltip), F_STR },
    { "tool_on", offsetof(cui_step_slider_opts, tool_on), F_BOOL },
    { "plain", offsetof(cui_step_slider_opts, plain), F_BOOL },
    { "disabled", offsetof(cui_step_slider_opts, disabled), F_BOOL }, { NULL }
};
static const lw_fld fld_palette[] = {
    { "value", offsetof(cui_palette_strip_opts, value), F_STR },
    { "no_shuffle", offsetof(cui_palette_strip_opts, no_shuffle), F_BOOL },
    { "height", offsetof(cui_palette_strip_opts, height), F_FLOAT },
    { "plain", offsetof(cui_palette_strip_opts, plain), F_BOOL }, { NULL }
};
static const lw_fld fld_aspect[] = {
    { "custom", offsetof(cui_aspect_picker_opts, custom), F_BOOL },
    { "custom_label", offsetof(cui_aspect_picker_opts, custom_label), F_STR },
    { "base", offsetof(cui_aspect_picker_opts, base), F_INT },
    { "value", offsetof(cui_aspect_picker_opts, value), F_STR },
    { "plain", offsetof(cui_aspect_picker_opts, plain), F_BOOL }, { NULL }
};
static const lw_fld fld_optact[] = {
    { "tooltip", offsetof(cui_option_action_opts, tooltip), F_STR },
    { "action_on", offsetof(cui_option_action_opts, action_on), F_BOOL },
    { "action_disabled", offsetof(cui_option_action_opts, action_disabled), F_BOOL },
    { "value", offsetof(cui_option_action_opts, value), F_STR },
    { "plain", offsetof(cui_option_action_opts, plain), F_BOOL }, { NULL }
};
static const lw_fld fld_seedf[] = {
    { "value", offsetof(cui_seed_field_opts, value), F_STR },
    { "random_text", offsetof(cui_seed_field_opts, random_text), F_STR },
    { "fixed_text", offsetof(cui_seed_field_opts, fixed_text), F_STR },
    { "plain", offsetof(cui_seed_field_opts, plain), F_BOOL }, { NULL }
};

static int lw_l_status_tag(lua_State *L) {
    cui_ctx *ui = lw_take(L);
    int s = lw_map(L, 2, lw_tagstates);
    LW_OPTS(cui_status_tag_opts, o, 3, fld_stag);
    lua_pushboolean(L, cui_status_tag_ex(ui, luaL_checkstring(L, 1), (cui_tag_status)s, o_p));
    return 1;
}
static int lw_l_ratio_slider(lua_State *L) {
    cui_ctx *ui = lw_take(L);
    float r = (float)luaL_optnumber(L, 3, 0.5);
    LW_OPTS(cui_ratio_slider_opts, o, 4, fld_ratio);
    cui_ratio_slider_ex(ui, luaL_checkstring(L, 1), luaL_checkstring(L, 2), &r, o_p);
    lua_pushnumber(L, r);
    return 1;
}
static int lw_l_gradient_slider(lua_State *L) {
    cui_ctx *ui = lw_take(L);
    const char *label = luaL_checkstring(L, 1);
    float v = (float)luaL_optnumber(L, 2, 0), lo = (float)luaL_optnumber(L, 3, 0), hi = (float)luaL_optnumber(L, 4, 360);
    LW_OPTS(cui_gradient_slider_opts, o, 5, fld_grad);
    if (!lua_isnoneornil(L, 5)) {
        lua_getfield(L, 5, "stops");
        if (lua_istable(L, -1)) {
            int n = (int)luaL_len(L, -1);
            luaL_argcheck(L, n <= LW_STOPS, 5, "too many stops");
            cui_gradient_stop *s = cui__frame_alloc(ui, sizeof *s * (size_t)(n > 0 ? n : 1));
            for (int k = 0; k < n; k++) {
                lua_geti(L, -1, k + 1);
                luaL_checktype(L, -1, LUA_TTABLE);
                lua_getfield(L, -1, "pos");
                s[k].pos = (float)luaL_checknumber(L, -1);
                lua_pop(L, 1);
                lua_getfield(L, -1, "color");
                s[k].color = lua_istable(L, -1) ? lw_color_of(L, lua_gettop(L)) : (cui_color){ 0, 0, 0, 255 };
                lua_pop(L, 2);
            }
            o.stops = s;
            o.stop_count = n;
            o_p = &o;
        }
        lua_pop(L, 1);
    }
    int r = cui_gradient_slider_ex(ui, label, &v, lo, hi, o_p);
    lua_pushnumber(L, v);
    lua_pushinteger(L, r);
    return 2;
}
static int lw_l_step_slider(lua_State *L) {
    cui_ctx *ui = lw_take(L);
    const char *label = luaL_checkstring(L, 1);
    int v = (int)luaL_optinteger(L, 2, 0), lo = (int)luaL_checkinteger(L, 3), hi = (int)luaL_checkinteger(L, 4);
    LW_OPTS(cui_step_slider_opts, o, 5, fld_stepsl);
    if (!lua_isnoneornil(L, 5)) {
        lua_getfield(L, 5, "labels");
        if (lua_istable(L, -1)) {
            int nl = 0;
            o.labels = lw_str_array(L, lua_gettop(L), LW_ITEMS, &nl);
            o_p = &o;
        }
        lua_pop(L, 1);
    }
    int r = cui_step_slider_ex(ui, label, &v, lo, hi, o_p);
    lua_pushinteger(L, v);
    lua_pushinteger(L, r);
    return 2;
}
static int lw_l_palette_strip(lua_State *L) {
    cui_ctx *ui = lw_take(L);
    const char *label = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);
    luaL_checktype(L, 3, LUA_TTABLE);
    int count = (int)luaL_len(L, 2);
    luaL_argcheck(L, count > 0 && count <= LW_PAL, 2, "1 to 16 swatches");
    luaL_argcheck(L, (int)luaL_len(L, 3) == count, 3, "one lock per swatch");
    typedef struct { cui_color c[LW_PAL]; bool lk[LW_PAL]; uint32_t seed;
                     cui_color wc[LW_PAL]; bool wlk[LW_PAL]; uint32_t wseed;
                     int count; bool have, edited; } pal;
    char key[160];
    snprintf(key, sizeof key, "pal:%s", label);
    pal *st = cui_state(ui, cui_make_id(ui, key), sizeof *st);
    cui_color *tc = cui__frame_alloc(ui, sizeof *tc * (size_t)count);
    bool *tl = cui__frame_alloc(ui, sizeof *tl * (size_t)count);
    for (int k = 0; k < count; k++) {
        lua_geti(L, 2, k + 1);
        tc[k] = lw_color_of(L, lua_gettop(L));
        lua_pop(L, 1);
        lua_geti(L, 3, k + 1);
        tl[k] = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
    }
    uint32_t wseed = 0;
    if (!lua_isnoneornil(L, 4)) {
        lua_getfield(L, 4, "seed");
        if (lua_isnumber(L, -1)) wseed = (uint32_t)lua_tointeger(L, -1);
        lua_pop(L, 1);
    }
    if (!st->have || st->count != count) {
        memcpy(st->c, tc, sizeof *tc * (size_t)count);
        memcpy(st->lk, tl, sizeof *tl * (size_t)count);
        st->seed = st->wseed = wseed;
        st->count = count;
        st->have = true;
    } else if (!st->edited) {
        if (memcmp(st->c, tc, sizeof *tc * (size_t)count) || memcmp(st->lk, tl, sizeof *tl * (size_t)count) ||
            st->wseed != wseed) {
            memcpy(st->c, tc, sizeof *tc * (size_t)count);
            memcpy(st->lk, tl, sizeof *tl * (size_t)count);
            st->seed = wseed;
        }
    }
    memcpy(st->wc, tc, sizeof *tc * (size_t)count);
    memcpy(st->wlk, tl, sizeof *tl * (size_t)count);
    st->wseed = wseed;
    LW_OPTS(cui_palette_strip_opts, o, 4, fld_palette);
    if (!lua_isnoneornil(L, 4)) { o.seed = &st->seed; o_p = &o; }
    int r = cui_palette_strip_ex(ui, label, st->c, st->lk, count, o_p);
    st->edited = (r & CUI_WELL_CHANGED) != 0;
    if (r & CUI_WELL_CHANGED) {
        lua_createtable(L, count, 0);
        for (int k = 0; k < count; k++) {
            lua_createtable(L, 4, 0);
            lua_pushnumber(L, st->c[k].r / 255.0); lua_seti(L, -2, 1);
            lua_pushnumber(L, st->c[k].g / 255.0); lua_seti(L, -2, 2);
            lua_pushnumber(L, st->c[k].b / 255.0); lua_seti(L, -2, 3);
            lua_pushnumber(L, st->c[k].a / 255.0); lua_seti(L, -2, 4);
            lua_seti(L, -2, k + 1);
        }
        lua_createtable(L, count, 0);
        for (int k = 0; k < count; k++) { lua_pushboolean(L, st->lk[k]); lua_seti(L, -2, k + 1); }
    } else { lua_pushvalue(L, 2); lua_pushvalue(L, 3); }
    lua_pushinteger(L, r);
    return 3;
}
static int lw_l_aspect_picker(lua_State *L) {
    cui_ctx *ui = lw_take(L);
    const char *label = luaL_checkstring(L, 1);
    int idx = (int)luaL_optinteger(L, 2, 0);
    LW_OPTS(cui_aspect_picker_opts, o, 3, fld_aspect);
    if (!lua_isnoneornil(L, 3)) {
        lua_getfield(L, 3, "ratios");
        if (lua_istable(L, -1)) {
            int n = (int)luaL_len(L, -1);
            luaL_argcheck(L, n > 0 && n <= LW_ITEMS, 3, "1 to 32 ratios");
            cui_aspect *rs = cui__frame_alloc(ui, sizeof *rs * (size_t)n);
            for (int k = 0; k < n; k++) {
                lua_geti(L, -1, k + 1);
                luaL_checktype(L, -1, LUA_TTABLE);
                lua_getfield(L, -1, "w");
                rs[k].w = (int)luaL_checkinteger(L, -1);
                lua_pop(L, 1);
                lua_getfield(L, -1, "h");
                rs[k].h = (int)luaL_checkinteger(L, -1);
                lua_pop(L, 1);
                lua_getfield(L, -1, "label");
                rs[k].label = lua_isnil(L, -1) ? NULL : luaL_checkstring(L, -1);
                lua_pop(L, 2);
            }
            o.ratios = rs;
            o.count = n;
            o_p = &o;
        }
        lua_pop(L, 1);
    }
    cui_aspect_picker_ex(ui, label, &idx, o_p);
    lua_pushinteger(L, idx);
    return 1;
}
static int lw_l_option_action(lua_State *L) {
    cui_ctx *ui = lw_take(L);
    const char *label = luaL_checkstring(L, 1);
    int n;
    const char *const *items = lw_str_array(L, 2, LW_ITEMS, &n);
    int idx = (int)luaL_optinteger(L, 3, 0);
    const char *icon = lua_isnoneornil(L, 4) ? NULL : luaL_checkstring(L, 4);
    LW_OPTS(cui_option_action_opts, o, 5, fld_optact);
    int r = cui_option_action_ex(ui, label, items, n, &idx, icon, o_p);
    lua_pushinteger(L, idx);
    lua_pushinteger(L, r);
    return 2;
}
static int lw_l_seed_field(lua_State *L) {
    cui_ctx *ui = lw_take(L);
    const char *label = luaL_checkstring(L, 1);
    typedef struct { uint32_t seed, lseed; bool locked, llocked; bool have, edited; } sd;
    char key[160];
    snprintf(key, sizeof key, "seed:%s", label);
    sd *st = cui_state(ui, cui_make_id(ui, key), sizeof *st);
    uint32_t inseed = (uint32_t)luaL_optinteger(L, 2, 0);
    bool inlock = lua_toboolean(L, 3) != 0;
    if (!st->have) { st->seed = st->lseed = inseed; st->locked = st->llocked = inlock; st->have = true; }
    else if (!st->edited) {
        if (inseed != st->lseed) st->seed = inseed;
        if (inlock != st->llocked) st->locked = inlock;
    }
    st->lseed = inseed;
    st->llocked = inlock;
    LW_OPTS(cui_seed_field_opts, o, 4, fld_seedf);
    int r = cui_seed_field_ex(ui, label, &st->seed, &st->locked, o_p);
    st->edited = (r & (CUI_WELL_CHANGED | CUI_WELL_ACTION)) != 0;
    lua_pushinteger(L, (lua_Integer)st->seed);
    lua_pushboolean(L, st->locked);
    lua_pushinteger(L, r);
    return 3;
}

/* ---- format bar ---- */

static const lw_fld fld_fbar[] = {
    { "footer", offsetof(cui_format_bar_opts, footer), F_STR },
    { "footer_keys", offsetof(cui_format_bar_opts, footer_keys), F_STR },
    { "delete_question", offsetof(cui_format_bar_opts, delete_question), F_STR },
    { "no_delete", offsetof(cui_format_bar_opts, no_delete), F_BOOL },
    { "no_more", offsetof(cui_format_bar_opts, no_more), F_BOOL }, { NULL }
};

static int lw_l_format_bar(lua_State *L) {
    cui_ctx *ui = lw_take(L);
    const char *id = luaL_checkstring(L, 1);
    cui_text_format f;
    memset(&f, 0, sizeof f);
    if (lua_istable(L, 2)) {
        lua_getfield(L, 2, "color");
        if (lua_isstring(L, -1)) f.color = (cui_tone)lw_map(L, -1, lw_tones);
        lua_pop(L, 1);
        lua_getfield(L, 2, "bold"); f.bold = lua_toboolean(L, -1) != 0; lua_pop(L, 1);
        lua_getfield(L, 2, "underline"); f.underline = lua_toboolean(L, -1) != 0; lua_pop(L, 1);
        lua_getfield(L, 2, "link"); f.link = lua_toboolean(L, -1) != 0; lua_pop(L, 1);
        lua_getfield(L, 2, "style"); f.style = (int)lua_tointeger(L, -1); lua_pop(L, 1);
        lua_getfield(L, 2, "size"); f.size = (float)lua_tonumber(L, -1); lua_pop(L, 1);
        lua_getfield(L, 2, "align");
        if (lua_isstring(L, -1)) f.align = (cui_format_align)lw_map(L, -1, lw_faligns);
        lua_pop(L, 1);
        lua_getfield(L, 2, "list");
        if (lua_isstring(L, -1)) f.list = (cui_format_list)lw_map(L, -1, lw_flists);
        lua_pop(L, 1);
    }
    LW_OPTS(cui_format_bar_opts, o, 3, fld_fbar);
    if (!lua_isnoneornil(L, 3)) {
        lua_getfield(L, 3, "styles");
        if (lua_istable(L, -1)) {
            int n = (int)luaL_len(L, -1);
            luaL_argcheck(L, n <= LW_ITEMS, 3, "too many styles");
            cui_text_style *ts = cui__frame_alloc(ui, sizeof *ts * (size_t)(n > 0 ? n : 1));
            for (int k = 0; k < n; k++) {
                memset(&ts[k], 0, sizeof ts[k]);
                lua_geti(L, -1, k + 1);
                luaL_checktype(L, -1, LUA_TTABLE);
                lua_getfield(L, -1, "name");
                ts[k].name = luaL_checkstring(L, -1);
                lua_pop(L, 1);
                lua_getfield(L, -1, "size");
                if (!lua_isnil(L, -1)) ts[k].size = (float)lua_tonumber(L, -1);
                lua_pop(L, 1);
                lua_getfield(L, -1, "preview_size");
                if (!lua_isnil(L, -1)) ts[k].preview_size = (float)lua_tonumber(L, -1);
                lua_pop(L, 1);
                lua_getfield(L, -1, "font");
                if (lua_isstring(L, -1)) ts[k].font = (cui_font)lw_map(L, -1, lw_fonts);
                lua_pop(L, 2);
            }
            o.styles = ts;
            o.style_count = n;
            o_p = &o;
        }
        lua_pop(L, 1);
        lua_getfield(L, 3, "colors");
        if (lua_istable(L, -1)) {
            int n = (int)luaL_len(L, -1);
            luaL_argcheck(L, n <= LW_ITEMS, 3, "too many colors");
            cui_tone *cs = cui__frame_alloc(ui, sizeof *cs * (size_t)(n > 0 ? n : 1));
            for (int k = 0; k < n; k++) {
                lua_geti(L, -1, k + 1);
                cs[k] = (cui_tone)lw_map(L, -1, lw_tones);
                lua_pop(L, 1);
            }
            o.colors = cs;
            o.color_count = n;
            o_p = &o;
        }
        lua_pop(L, 1);
    }
    int r = cui_format_bar_ex(ui, id, &f, o_p);
    if (lua_istable(L, 2)) {                                             /* the script's table reads back current */
        lua_pushstring(L, lw_tones[f.color]); lua_setfield(L, 2, "color");
        lua_pushboolean(L, f.bold); lua_setfield(L, 2, "bold");
        lua_pushboolean(L, f.underline); lua_setfield(L, 2, "underline");
        lua_pushboolean(L, f.link); lua_setfield(L, 2, "link");
        lua_pushinteger(L, f.style); lua_setfield(L, 2, "style");
        lua_pushnumber(L, f.size); lua_setfield(L, 2, "size");
        lua_pushstring(L, lw_faligns[f.align]); lua_setfield(L, 2, "align");
        lua_pushstring(L, lw_flists[f.list]); lua_setfield(L, 2, "list");
    }
    lua_pushinteger(L, r);
    if (lua_istable(L, 2)) { lua_pushvalue(L, 2); return 2; }
    return 1;
}

/* ---- people and canvas ---- */

static const lw_fld fld_lcur[] = {
    { "no_glide", offsetof(cui_live_cursors_opts, no_glide), F_BOOL }, { NULL }
};
static const lw_fld fld_ppill[] = {
    { "size", offsetof(cui_presence_pill_opts, size), F_FLOAT },
    { "max_visible", offsetof(cui_presence_pill_opts, max_visible), F_INT }, { NULL }
};
static const lw_fld fld_sring[] = {
    { "name", offsetof(cui_selection_ring_opts, name), F_STR },
    { "radius", offsetof(cui_selection_ring_opts, radius), F_FLOAT }, { NULL }
};
static const lw_fld fld_cctrl[] = {
    { "no_fit", offsetof(cui_canvas_controls_opts, no_fit), F_BOOL },
    { "start_side", offsetof(cui_canvas_controls_opts, start_side), F_BOOL }, { NULL }
};
static const lw_fld fld_rcard[] = {
    { "caption", offsetof(cui_result_card_opts, caption), F_STR },
    { "badge", offsetof(cui_result_card_opts, badge), F_STR },
    { "badge_tone", offsetof(cui_result_card_opts, badge_tone), F_TONE },
    { "image_w", offsetof(cui_result_card_opts, image_w), F_INT },
    { "image_h", offsetof(cui_result_card_opts, image_h), F_INT },
    { "width", offsetof(cui_result_card_opts, width), F_SIZE },
    { "aspect", offsetof(cui_result_card_opts, aspect), F_FLOAT }, { NULL }
};
static const lw_fld fld_sbar[] = {
    { "count_format", offsetof(cui_selection_bar_opts, count_format), F_STR },
    { "no_escape", offsetof(cui_selection_bar_opts, no_escape), F_BOOL }, { NULL }
};

static cui_live_user *lw_users(lua_State *L, int idx, int *n_out) {
    luaL_checktype(L, idx, LUA_TTABLE);
    int n = (int)luaL_len(L, idx);
    luaL_argcheck(L, n <= LW_USERS, idx, "too many people");
    cui_live_user *u = cui__frame_alloc(lw_ui, sizeof *u * (size_t)(n > 0 ? n : 1));
    for (int k = 0; k < n; k++) {
        memset(&u[k], 0, sizeof u[k]);
        lua_geti(L, idx, k + 1);
        luaL_checktype(L, -1, LUA_TTABLE);
        lua_getfield(L, -1, "name");
        u[k].name = lua_isnil(L, -1) ? NULL : luaL_checkstring(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, -1, "color");
        if (lua_istable(L, -1)) u[k].color = lw_color_of(L, lua_gettop(L));
        lua_pop(L, 1);
        lua_getfield(L, -1, "tone");
        if (lua_isstring(L, -1)) u[k].tone = (cui_tone)lw_map(L, -1, lw_tones);
        lua_pop(L, 1);
        lua_getfield(L, -1, "x");
        u[k].x = (float)lua_tonumber(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, -1, "y");
        u[k].y = (float)lua_tonumber(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, -1, "says");
        u[k].says = lua_isnil(L, -1) ? NULL : luaL_checkstring(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, -1, "speaking");
        u[k].speaking = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        lua_getfield(L, -1, "away");
        u[k].away = lua_toboolean(L, -1) != 0;
        lua_pop(L, 2);
    }
    *n_out = n;
    return u;
}

static int lw_l_live_cursors(lua_State *L) {
    cui_ctx *ui = lw_take(L);
    int n;
    cui_live_user *u = lw_users(L, 2, &n);
    LW_OPTS(cui_live_cursors_opts, o, 3, fld_lcur);
    cui_live_cursors_ex(ui, luaL_checkstring(L, 1), u, n, o_p);
    return 0;
}
static int lw_l_presence_pill(lua_State *L) {
    cui_ctx *ui = lw_take(L);
    int n;
    cui_live_user *u = lw_users(L, 2, &n);
    LW_OPTS(cui_presence_pill_opts, o, 3, fld_ppill);
    cui_presence_pill_ex(ui, luaL_checkstring(L, 1), u, n, o_p);
    return 0;
}
static int lw_l_selection_ring(lua_State *L) {
    cui_ctx *ui = lw_take(L);
    cui_color c = lw_color_of(L, 1);
    LW_OPTS(cui_selection_ring_opts, o, 2, fld_sring);
    cui_selection_ring_ex(ui, c, o_p);
    return 0;
}
static int lw_l_canvas_controls(lua_State *L) {
    cui_ctx *ui = lw_take(L);
    const char *id = luaL_checkstring(L, 1);
    cui_node_view v;
    memset(&v, 0, sizeof v);
    if (lua_istable(L, 2)) {
        lua_getfield(L, 2, "x"); v.x = (float)lua_tonumber(L, -1); lua_pop(L, 1);
        lua_getfield(L, 2, "y"); v.y = (float)lua_tonumber(L, -1); lua_pop(L, 1);
        lua_getfield(L, 2, "zoom"); v.zoom = (float)lua_tonumber(L, -1); lua_pop(L, 1);
    }
    LW_OPTS(cui_canvas_controls_opts, o, 3, fld_cctrl);
    typedef struct { bool on[LW_TOGGLES]; bool have; int count; } tog;
    tog *tg = NULL;
    if (!lua_isnoneornil(L, 3)) {
        lua_getfield(L, 3, "steps");
        if (lua_istable(L, -1)) {
            int ns = 0;
            o.steps = lw_levels(L, lua_gettop(L), LW_ITEMS, &ns);
            o.step_count = ns;
            o_p = &o;
        }
        lua_pop(L, 1);
        lua_getfield(L, 3, "content");
        if (lua_istable(L, -1)) {
            lua_getfield(L, -1, "x"); o.content.x = (float)lua_tonumber(L, -1); lua_pop(L, 1);   /* fit bounds */
            lua_getfield(L, -1, "y"); o.content.y = (float)lua_tonumber(L, -1); lua_pop(L, 1);
            lua_getfield(L, -1, "w"); o.content.w = (float)lua_tonumber(L, -1); lua_pop(L, 1);
            lua_getfield(L, -1, "h"); o.content.h = (float)lua_tonumber(L, -1); lua_pop(L, 1);
            o_p = &o;
        }
        lua_pop(L, 1);
        lua_getfield(L, 3, "toggles");
        if (lua_istable(L, -1)) {
            int n = (int)luaL_len(L, -1);
            luaL_argcheck(L, n <= LW_TOGGLES, 3, "too many toggles");
            char key[128];
            snprintf(key, sizeof key, "cc:%s", id);
            tg = cui_state(ui, cui_make_id(ui, key), sizeof *tg);
            if (!tg->have || tg->count != n) {
                for (int k = 0; k < n; k++) {
                    lua_geti(L, -1, k + 1);
                    lua_getfield(L, -1, "on");
                    tg->on[k] = lua_toboolean(L, -1) != 0;
                    lua_pop(L, 2);
                }
                tg->have = true;
                tg->count = n;
            }
            cui_canvas_toggle *t = cui__frame_alloc(ui, sizeof *t * (size_t)(n > 0 ? n : 1));
            for (int k = 0; k < n; k++) {
                memset(&t[k], 0, sizeof t[k]);
                lua_geti(L, -1, k + 1);
                luaL_checktype(L, -1, LUA_TTABLE);
                lua_getfield(L, -1, "icon");
                t[k].icon = lua_isnil(L, -1) ? NULL : luaL_checkstring(L, -1);
                lua_pop(L, 1);
                lua_getfield(L, -1, "label");
                t[k].label = lua_isnil(L, -1) ? NULL : luaL_checkstring(L, -1);
                lua_pop(L, 1);
                t[k].on = &tg->on[k];
                lua_pop(L, 1);
            }
            o.toggles = t;
            o.toggle_count = n;
            o_p = &o;
        }
        lua_pop(L, 1);
    }
    int r = cui_canvas_controls_ex(ui, id, &v, o_p);
    lua_pushinteger(L, r);
    lua_newtable(L);
    lua_pushnumber(L, v.x); lua_setfield(L, -2, "x");
    lua_pushnumber(L, v.y); lua_setfield(L, -2, "y");
    lua_pushnumber(L, v.zoom); lua_setfield(L, -2, "zoom");
    lua_createtable(L, o.toggle_count, 0);
    for (int k = 0; k < o.toggle_count; k++) { lua_pushboolean(L, tg->on[k]); lua_seti(L, -2, k + 1); }
    return 3;
}
static int lw_l_result_card(lua_State *L) {
    cui_ctx *ui = lw_take(L);
    cui_texture tex = (cui_texture)luaL_optinteger(L, 2, 0);
    LW_OPTS(cui_result_card_opts, o, 4, fld_rcard);
    lua_pushboolean(L, cui_result_card_ex(ui, luaL_checkstring(L, 1), tex, luaL_checkstring(L, 3), o_p));
    return 1;
}
static int lw_l_selection_bar(lua_State *L) {
    cui_ctx *ui = lw_take(L);
    const char *id = luaL_checkstring(L, 1);
    int count = (int)luaL_optinteger(L, 2, 0);
    luaL_checktype(L, 3, LUA_TTABLE);
    int n = (int)luaL_len(L, 3);
    luaL_argcheck(L, n <= LW_ACTIONS, 3, "too many actions");
    cui_selection_action *a = cui__frame_alloc(ui, sizeof *a * (size_t)(n > 0 ? n : 1));
    for (int k = 0; k < n; k++) {
        memset(&a[k], 0, sizeof a[k]);
        lua_geti(L, 3, k + 1);
        luaL_checktype(L, -1, LUA_TTABLE);
        lua_getfield(L, -1, "icon");
        a[k].icon = lua_isnil(L, -1) ? NULL : luaL_checkstring(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, -1, "label");
        a[k].label = lua_isnil(L, -1) ? NULL : luaL_checkstring(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, -1, "tone");
        if (lua_isstring(L, -1)) a[k].tone = (cui_tone)lw_map(L, -1, lw_tones);
        lua_pop(L, 2);
    }
    LW_OPTS(cui_selection_bar_opts, o, 4, fld_sbar);
    lua_pushinteger(L, cui_selection_bar_ex(ui, id, count, a, n, o_p));
    return 1;
}
static int lw_l_image_load(lua_State *L) {
    cui_ctx *ui = lw_take(L);
    int w = 0, h = 0;
    cui_texture tex = cui_image_load(ui, luaL_checkstring(L, 1), &w, &h);
    if (!tex) { lua_pushnil(L); return 1; }
    lua_pushinteger(L, (lua_Integer)tex);
    lua_pushinteger(L, w);
    lua_pushinteger(L, h);
    return 3;
}
static int lw_l_image_destroy(lua_State *L) {
    cui_image_destroy(lw_take(L), (cui_texture)luaL_checkinteger(L, 1));
    return 0;
}
static int lw_l_set_clipboard(lua_State *L) {
    cui_set_clipboard_text(lw_take(L), luaL_checkstring(L, 1));
    return 0;
}
static int lw_l_get_clipboard(lua_State *L) {
    lua_pushstring(L, cui_get_clipboard_text(lw_take(L)));
    return 1;
}

/* ---- install: the raw table plus the Lua bootstrap that adds the closures ---- */

static const luaL_Reg lw_raw[] = {
    { "box_begin", lw_l_box_begin }, { "box_end", lw_l_box_end },
    { "row_begin", lw_l_row_begin }, { "row_end", lw_l_row_end },
    { "column_begin", lw_l_column_begin }, { "column_end", lw_l_column_end },
    { "card_begin", lw_l_card_begin }, { "card_end", lw_l_card_end },
    { "scroll_begin", lw_l_scroll_begin }, { "scroll_end", lw_l_scroll_end },
    { "skeleton_group_begin", lw_l_skelgroup_begin }, { "skeleton_group_end", lw_l_skelgroup_end },
    { "veil_begin", lw_l_veil_begin }, { "veil_end", lw_l_veil_end },
    { "toolbar_begin", lw_l_toolbar_begin }, { "toolbar_end", lw_l_toolbar_end },
    { "toolbar_group_begin", lw_l_toolbar_group_begin }, { "toolbar_group_end", lw_l_toolbar_group_end },
    { "text", lw_l_text }, { "heading", lw_l_heading }, { "muted", lw_l_muted },
    { "label", lw_l_label }, { "secondary", lw_l_secondary }, { "code", lw_l_code },
    { "kbd", lw_l_kbd }, { "icon", lw_l_icon }, { "separator", lw_l_separator },
    { "spacer", lw_l_spacer }, { "gap", lw_l_gap }, { "badge", lw_l_badge },
    { "progress", lw_l_progress }, { "spinner", lw_l_spinner }, { "skeleton", lw_l_skeleton },
    { "status_spinner", lw_l_status_spinner }, { "pattern_meter", lw_l_pattern_meter },
    { "voice_wave", lw_l_voice_wave }, { "voice_note", lw_l_voice_note },
    { "voice_session", lw_l_voice_session }, { "voice_resample", lw_l_voice_resample },
    { "toolbar_button", lw_l_toolbar_button }, { "toolbar_separator", lw_l_toolbar_separator },
    { "toolbar_spacer", lw_l_toolbar_spacer }, { "toolbar_radio", lw_l_toolbar_radio },
    { "toolbar_primary", lw_l_toolbar_primary }, { "toolbar_cycle", lw_l_toolbar_cycle },
    { "flyout_mark", lw_l_flyout_mark },
    { "status_tag", lw_l_status_tag }, { "ratio_slider", lw_l_ratio_slider },
    { "gradient_slider", lw_l_gradient_slider }, { "step_slider", lw_l_step_slider },
    { "palette_strip", lw_l_palette_strip }, { "aspect_picker", lw_l_aspect_picker },
    { "option_action", lw_l_option_action }, { "seed_field", lw_l_seed_field },
    { "format_bar", lw_l_format_bar },
    { "live_cursors", lw_l_live_cursors }, { "presence_pill", lw_l_presence_pill },
    { "selection_ring", lw_l_selection_ring }, { "canvas_controls", lw_l_canvas_controls },
    { "result_card", lw_l_result_card }, { "selection_bar", lw_l_selection_bar },
    { "image_load", lw_l_image_load }, { "image_destroy", lw_l_image_destroy },
    { "set_clipboard", lw_l_set_clipboard }, { "get_clipboard", lw_l_get_clipboard },
    { "button", lw_l_button }, { "checkbox", lw_l_checkbox },
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
    " 'slider', 'rating', 'textfield', 'textarea', 'toast', 'line_chart',"
    " 'spinner', 'skeleton', 'status_spinner', 'pattern_meter',"
    " 'voice_wave', 'voice_note', 'voice_session', 'voice_resample',"
    " 'toolbar_button', 'toolbar_separator', 'toolbar_spacer', 'toolbar_radio',"
    " 'toolbar_primary', 'toolbar_cycle', 'flyout_mark',"
    " 'status_tag', 'ratio_slider', 'gradient_slider', 'step_slider', 'palette_strip',"
    " 'aspect_picker', 'option_action', 'seed_field', 'format_bar',"
    " 'live_cursors', 'presence_pill', 'selection_ring', 'canvas_controls',"
    " 'result_card', 'selection_bar', 'image_load', 'image_destroy',"
    " 'set_clipboard', 'get_clipboard' } do ui[n] = raw[n] end\n"
    "for _, n in ipairs{ 'row_begin', 'row_end', 'column_begin', 'column_end', 'box_begin', 'box_end',"
    " 'card_begin', 'card_end', 'scroll_begin', 'scroll_end',"
    " 'skeleton_group_begin', 'skeleton_group_end', 'veil_begin', 'veil_end',"
    " 'toolbar_begin', 'toolbar_end', 'toolbar_group_begin', 'toolbar_group_end' } do ui[n] = raw[n] end\n"
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
    "ui.skeleton_group = scoped('skeleton_group_begin', 'skeleton_group_end')\n"
    "ui.toolbar_group = scoped('toolbar_group_begin', 'toolbar_group_end')\n"
    "ui.toolbar = function(id, opts, fn)\n"
    "  if type(opts) == 'function' then fn, opts = opts, nil end\n"
    "  raw.toolbar_begin(id, opts)\n"
    "  local ok, err = pcall(fn)\n"
    "  raw.toolbar_end()\n"
    "  if not ok then error(err, 0) end\n"
    "end\n"
    "ui.veil = function(id, ...)\n"
    "  local revealed, opts, fn\n"
    "  for i = 1, select('#', ...) do\n"
    "    local a = select(i, ...)\n"
    "    local t = type(a)\n"
    "    if t == 'function' then fn = a\n"
    "    elseif t == 'table' then opts = a\n"
    "    elseif a ~= nil then revealed = a and true or nil end\n"
    "  end\n"
    "  local changed, v = raw.veil_begin(id, revealed, opts)\n"
    "  local ok, err = pcall(fn)\n"
    "  raw.veil_end()\n"
    "  if not ok then error(err, 0) end\n"
    "  return changed, v\n"
    "end\n"
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
