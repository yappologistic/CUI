/* Lua console: a colored output pane over a highlighted Lua editor. The pane
 * is the terminal's ring buffer (cui_terminal.c) shown with no input line, the
 * input is the code editor (cui_editor.c) on a small grid, and Enter runs the
 * text when the compiler says the chunk is complete. The Lua state itself
 * lives in cui_lua.c, reached only through the cui__script_* functions and
 * only where CUI_WITH_LUA is set, so this file builds without Lua. */
#include "cui_internal.h"
#include <stdarg.h>


#define CUI__RP_IN    4096    /* bytes of pending input */
#define CUI__RP_HIST  32      /* commands kept */
#define CUI__RP_HLINE 256     /* bytes per command */

typedef struct {
    char buf[CUI__RP_IN];
    char hist[CUI__RP_HIST][CUI__RP_HLINE];
    int hn, hpos;
    bool browsing;
} cui__rp_state;

#ifdef CUI_WITH_LUA

static bool cui__rp_blank(const char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    return !*s;
}

/* the chunk as it was typed: dim prompt, then the text with ">> " on the
 * continuation lines */
static void cui__rp_echo(cui_script *s, const char *prompt, const char *code) {
    cui__script_write(s, "\x1b[2m", -1);
    cui__script_write(s, prompt, -1);
    cui__script_write(s, "\x1b[0m ", -1);
    int len = (int)strlen(code), start = 0;
    for (int i = 0; i < len; i++) {
        if (code[i] != '\n') continue;
        cui__script_write(s, code + start, i - start);
        cui__script_write(s, "\n", 1);
        start = i + 1;
        if (start < len) cui__script_write(s, "\x1b[2m>> \x1b[0m", -1);
    }
    if (start < len) {
        cui__script_write(s, code + start, len - start);
        cui__script_write(s, "\n", 1);
    } else if (!len) {
        cui__script_write(s, "\n", 1);
    }
}

static void cui__rp_hist(cui__rp_state *st, const char *entry) {
    size_t n = strlen(entry);
    while (n > 0 && (entry[n - 1] == '\n' || entry[n - 1] == ' ' || entry[n - 1] == '\t' || entry[n - 1] == '\r')) n--;
    if (!n) return;
    if (st->hn > 0 && st->hist[st->hn - 1][n] == 0 && !strncmp(st->hist[st->hn - 1], entry, n)) return;
    if (st->hn == CUI__RP_HIST) {
        memmove(st->hist[0], st->hist[1], sizeof st->hist[0] * (CUI__RP_HIST - 1));
        st->hn--;
    }
    memcpy(st->hist[st->hn], entry, n);
    st->hist[st->hn][n] = 0;
    st->hn++;
}

#endif /* CUI_WITH_LUA */

static cui_repl_result cui__repl_ex_body(cui_ctx *ui, const char *id_str, cui_script *s, const cui_repl_opts *o) {
    cui_repl_opts d;
    memset(&d, 0, sizeof d);
    if (!o) o = &d;
    cui_repl_result res;
    memset(&res, 0, sizeof res);
    cui_id id = cui_make_id(ui, id_str);
    cui__rp_state *st = cui_state(ui, id, sizeof *st);

    float size = cui__fs(ui, o->size > 0 ? o->size : 13);
    float lh = roundf(size * 1.6f);            /* the editor's grid */
    const char *prompt = o->prompt ? o->prompt : ">";

#ifdef CUI_WITH_LUA
    cui_terminal_buffer *term = s ? cui__script_term(s) : NULL;
    bool have = term != NULL;
    bool want_more = have && !cui__rp_blank(st->buf) && cui__script_wants_more(s, st->buf);
#else
    (void)s;
    bool have = false, want_more = false;
#endif

    int nl = 1;
    for (const char *p = st->buf; (p = strchr(p, '\n')) != NULL; p++) nl++;
    float in_h = (float)CUI_MIN(nl, 10) * lh + 12;

    int outer = cui__node_new(ui, id);
    cui_node *on = &ui->nodes[outer];
    on->w = o->width.kind ? o->width : CUI_FILL;
    on->h = o->height < 0 ? CUI_FILL : CUI_PX(o->height > 0 ? o->height : 360);
    on->dir = CUI_COLUMN;
    on->gap = 6;
    cui__open(ui, outer);
    cui_push_id(ui, id_str);

#ifdef CUI_WITH_LUA
    if (have) {
        cui_terminal_opts to;
        memset(&to, 0, sizeof to);
        to.height = -1;                        /* fill the space above the input */
        to.no_input = true;
        to.size = o->size;
        cui_terminal_ex(ui, "out", term, &to);
    } else
#endif
    {
        int t = cui__text_node(ui, have ? "" : "Build with CUI_LUA=ON to run Lua.", -1, CUI_FONT_BODY,
                               cui__fs(ui, ui->theme.text_sm), CUI_C(ui, TEXT_MUTED), true);
        ui->nodes[t].pad[0] = ui->nodes[t].pad[2] = 12;
        ui->nodes[t].pad[1] = ui->nodes[t].pad[3] = 12;
    }

    int row = cui__node_new(ui, 0);
    cui_node *rn = &ui->nodes[row];
    rn->dir = CUI_ROW; rn->align = CUI_ALIGN_START; rn->w = CUI_FILL; rn->gap = 0;
    cui__open(ui, row);
    float fh = cui__font_line_height(ui, CUI_FONT_MONO, size);
    int pnode = cui__text_node(ui, want_more ? ">>" : prompt, -1, CUI_FONT_MONO, size,
                               ui->accent == CUI_ACCENT_NEUTRAL ? CUI_C(ui, TEXT_SECONDARY) : CUI_C(ui, ACCENT), false);
    ui->nodes[pnode].flags |= CUI_NF_TABULAR;
    ui->nodes[pnode].pad[0] = 7 + (lh - fh) * 0.5f;   /* lines up with the editor's first row */
    ui->nodes[pnode].pad[3] = 10; ui->nodes[pnode].pad[1] = 4;
    cui_code_editor_opts eo;
    memset(&eo, 0, sizeof eo);
    eo.lang = "lua";
    eo.width = CUI_FILL;
    eo.height = CUI_MAX(80, in_h);             /* the editor's own minimum */
    eo.size = o->size;
    eo.no_line_numbers = true;
    eo.placeholder = o->placeholder ? o->placeholder : (have ? "-- Lua" : "Lua engine not built");
    eo.autofocus = o->autofocus;
    cui_code_editor_result r = cui_code_editor_ex(ui, "in", st->buf, sizeof st->buf, &eo);
    res.refused = r.refused;
    cui_pop_id(ui);
    cui__close(ui);                            /* row */
    cui__close(ui);                            /* outer */
    ui->last_id = id;

#ifdef CUI_WITH_LUA
    /* ---- keys, once the editor has had its turn ---- */
    if (have) {
        bool focused = r.focused;
        int len = (int)strlen(st->buf);
        bool enter_run = focused && r.changed && r.cursor == len && len > 0 && st->buf[len - 1] == '\n' &&
                         !(ui->mods & CUI_MOD_SHIFT) && !want_more;
        bool ctrl_enter = focused && (ui->mods & CUI_MOD_CTRL) && ui->kpressed[CUI_KEY_ENTER];
        if (focused && (enter_run || ctrl_enter)) {
            if (!cui__rp_blank(st->buf)) {
                cui__rp_echo(s, prompt, st->buf);
                cui__rp_hist(st, st->buf);
                cui__script_exec(s, st->buf, -1);
                res.executed = true;
            }
            st->buf[0] = 0;                    /* the editor's newline goes with it */
            st->browsing = false;
        }
        if (focused && (ui->mods & CUI_MOD_CTRL) && ui->kpressed[CUI_KEY_L]) cui_terminal_clear(term);
        if (focused && len == 0 && st->hn > 0 && !o->no_history &&
            (ui->kpressed[CUI_KEY_UP] || ui->kpressed[CUI_KEY_DOWN])) {
            int dir = ui->kpressed[CUI_KEY_UP] ? -1 : 1;
            if (!st->browsing) { st->hpos = st->hn; st->browsing = true; }
            st->hpos = CUI_CLAMP(st->hpos + dir, 0, st->hn);
            snprintf(st->buf, sizeof st->buf, "%s", st->hpos < st->hn ? st->hist[st->hpos] : "");
            int lines = 1, last = 0;
            for (int k = 0; st->buf[k]; k++) if (st->buf[k] == '\n') { lines++; last = k + 1; }
            cui_push_id(ui, id_str);            /* the goto reaches the editor only in its own scope */
            cui_code_editor_goto(ui, "in", lines, (int)(strlen(st->buf) - last) + 1);
            cui_pop_id(ui);
            ui->kpressed[CUI_KEY_UP] = ui->kpressed[CUI_KEY_DOWN] = 0;
        }
    }
#endif
    return res;
}

/* left to right in any interface: code, prompts and results (see cui__keep_ltr) */
cui_repl_result cui_repl_ex(cui_ctx *ui, const char *id_str, cui_script *s, const cui_repl_opts *o) {
    int first = ui->node_count;
    cui_repl_result r = cui__repl_ex_body(ui, id_str, s, o);
    cui__keep_ltr(ui, first);
    return r;
}

/* ---- the script handle ---- */

cui_script *cui_script_new(void) {
#ifdef CUI_WITH_LUA
    return cui__script_new();
#else
    return NULL;
#endif
}

void cui_script_free(cui_script *s) {
#ifdef CUI_WITH_LUA
    cui__script_free(s);
#else
    (void)s;
#endif
}

void cui_script_eval(cui_script *s, const char *code) {
#ifdef CUI_WITH_LUA
    cui__script_exec(s, code, -1);
#else
    (void)s; (void)code;
#endif
}

void cui_script_printf(cui_script *s, const char *fmt, ...) {
#ifdef CUI_WITH_LUA
    va_list ap;
    va_start(ap, fmt);
    cui__script_vprintf(s, fmt, ap);
    va_end(ap);
#else
    (void)s; (void)fmt;
#endif
}

void cui_script_set_fn(cui_script *s, const char *name, cui_script_fn fn, void *ud) {
#ifdef CUI_WITH_LUA
    cui__script_set_fn(s, name, fn, ud);
#else
    (void)s; (void)name; (void)fn; (void)ud;
#endif
}

void cui_script_set_number(cui_script *s, const char *name, double v) {
#ifdef CUI_WITH_LUA
    cui__script_set_number(s, name, v);
#else
    (void)s; (void)name; (void)v;
#endif
}

double cui_script_get_number(cui_script *s, const char *name) {
#ifdef CUI_WITH_LUA
    return cui__script_get_number(s, name);
#else
    (void)s; (void)name;
    return 0;
#endif
}

void cui_script_set_string(cui_script *s, const char *name, const char *v) {
#ifdef CUI_WITH_LUA
    cui__script_set_string(s, name, v);
#else
    (void)s; (void)name; (void)v;
#endif
}

const char *cui_script_get_string(cui_script *s, const char *name) {
#ifdef CUI_WITH_LUA
    return cui__script_get_string(s, name);
#else
    (void)s; (void)name;
    return NULL;
#endif
}

void *cui_script_lua(cui_script *s) {
#ifdef CUI_WITH_LUA
    return cui__script_lua_impl(s);
#else
    (void)s;
    return NULL;
#endif
}

#ifndef CUI_WITH_LUA   /* with the engine, cui_lua_ui.c provides this */
void cui_script_widget(cui_ctx *ui, const char *id, cui_script *s, const char *fn) {
    (void)id; (void)s; (void)fn;
    int first = ui->node_count;
    int t = cui__text_node(ui, "Build with CUI_LUA=ON to run Lua.", -1, CUI_FONT_BODY,
                           cui__fs(ui, ui->theme.text_sm), CUI_C(ui, TEXT_MUTED), true);
    ui->nodes[t].pad[0] = ui->nodes[t].pad[1] = ui->nodes[t].pad[2] = ui->nodes[t].pad[3] = 12;
    cui__keep_ltr(ui, first);
}
#endif
