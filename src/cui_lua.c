/* Lua engine for the console (cui_repl.c): owns the Lua state and its output pane.
 * Compiled only when CUI_LUA is ON; the component reaches it through the cui__script_*
 * functions and never the other way around, so a build without this file links no Lua.
 * The state keeps the standard libraries except the ones that can leave the program:
 * io and require are removed whole, os loses execute/exit/remove/rename/tmpname. */
#include "cui_internal.h"
#include <stdarg.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#define CUI__LUA_TEXT  (64 * 1024)   /* bytes held by the pane ring */
#define CUI__LUA_LINES 2048
#define CUI__LUA_FNS   64

typedef struct { cui_script_fn fn; void *ud; char name[64]; } cui__lua_fn;

struct cui_script {
    lua_State *L;
    cui_terminal_buffer term;
    char *text;
    cui_terminal_line *lines;
    cui__lua_fn fns[CUI__LUA_FNS];
    int nfns;
};

/* ---- the pane ---- */

void cui__script_write(cui_script *s, const char *data, int len) {
    if (s) cui_terminal_write(&s->term, data, len);
}

void cui__script_vprintf(cui_script *s, const char *fmt, va_list ap) {
    if (!s) return;
    char buf[8192];
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    if (n > 0) cui_terminal_write(&s->term, buf, CUI_MIN(n, (int)sizeof buf - 1));
}

static void cui__script_error(cui_script *s, const char *msg) {
    cui_terminal_printf(&s->term, "\x1b[31m%s\x1b[0m\n", msg ? msg : "(error)");
}

/* the values on the stack from..to, tab separated, then a newline (print and results) */
static void cui__script_show(cui_script *s, int from, int to) {
    lua_State *L = s->L;
    for (int k = from; k <= to; k++) {
        if (k > from) cui_terminal_write(&s->term, "\t", 1);
        luaL_tolstring(L, k, NULL);
        size_t l;
        const char *p = lua_tolstring(L, -1, &l);
        if (p) cui_terminal_write(&s->term, p, (int)CUI_MIN(l, (size_t)0x7FFFFFFF));
        lua_pop(L, 1);
    }
    cui_terminal_write(&s->term, "\n", 1);
}

static int cui__lua_print(lua_State *L) {
    cui_script *s = lua_touserdata(L, lua_upvalueindex(1));
    cui__script_show(s, 1, lua_gettop(L));
    return 0;
}

/* ---- app functions ---- */

static int cui__lua_appfn(lua_State *L) {
    cui_script *s = lua_touserdata(L, lua_upvalueindex(1));
    int idx = (int)lua_tointeger(L, lua_upvalueindex(2));
    cui__lua_fn *f = &s->fns[idx];
    int n = lua_gettop(L), m = CUI_MIN(n, 32);
    const char *args[32];
    for (int k = 0; k < m; k++) {
        luaL_tolstring(L, k + 1, NULL);
        args[k] = lua_tostring(L, -1);
    }
    char out[1024];
    out[0] = 0;
    int got = f->fn(f->ud, args, m, out, sizeof out);
    lua_pop(L, m);
    if (got && out[0]) {
        lua_pushstring(L, out);
        return 1;
    }
    return 0;
}

void cui__script_set_fn(cui_script *s, const char *name, cui_script_fn fn, void *ud) {
    if (!s || !name || !name[0] || !fn) return;
    int idx = -1;
    for (int k = 0; k < s->nfns; k++)
        if (!strcmp(s->fns[k].name, name)) { idx = k; break; }
    if (idx < 0) {
        if (s->nfns == CUI__LUA_FNS) return;
        idx = s->nfns++;
        snprintf(s->fns[idx].name, sizeof s->fns[idx].name, "%s", name);
    }
    s->fns[idx].fn = fn;
    s->fns[idx].ud = ud;
    lua_pushlightuserdata(s->L, s);
    lua_pushinteger(s->L, idx);
    lua_pushcclosure(s->L, cui__lua_appfn, 2);
    lua_setglobal(s->L, name);
}

/* ---- running ---- */

static bool cui__lua_load(cui_script *s, const char *code, size_t n) {
    return luaL_loadbuffer(s->L, code, n, "=console") == 0;
}

bool cui__script_wants_more(cui_script *s, const char *code) {
    if (!s || !code) return false;
    const char *p = code;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    if (!*p) return false;
    if (cui__lua_load(s, code, strlen(code))) {
        lua_pop(s->L, 1);
        return false;
    }
    const char *m = lua_tostring(s->L, -1);
    bool more = m && strstr(m, "<eof>") != NULL;   /* a chunk the parser is still waiting for */
    lua_pop(s->L, 1);
    return more;
}

/* the chunk on the stack: run it, report errors to the pane, print results.
 * print_value: 1 always, -1 only what the console echoes (not a lone nil). */
static void cui__lua_run(cui_script *s, int print_value) {
    lua_State *L = s->L;
    int top = lua_gettop(L) - 1;   /* the chunk sits at top + 1 and pcall consumes it */
    if (lua_pcall(L, 0, LUA_MULTRET, 0) != 0) {
        cui__script_error(s, lua_tostring(L, -1));
        lua_pop(L, 1);
        return;
    }
    int nres = lua_gettop(L) - top;
    if (nres > 0 && (print_value == 1 || !(nres == 1 && lua_isnil(L, top + 1))))
        cui__script_show(s, top + 1, top + nres);
    lua_pop(L, nres);
}

void cui__script_exec(cui_script *s, const char *code, int print_result) {
    if (!s || !code) return;
    if (cui__lw_in_widget()) {            /* building the tree from two places at once is a bug */
        cui_terminal_printf(&s->term, "\x1b[31mrun code from outside a widget, not during one\x1b[0m\n");
        return;
    }
    while (*code == ' ' || *code == '\t') code++;
    if (*code == '=') {
        code++;
        while (*code == ' ' || *code == '\t') code++;
        print_result = 1;   /* =expr prints even nil */
    }
    size_t n = strlen(code);
    /* try it as an expression first, the way the stock REPL does */
    if (print_result != 0) {
        char *tmp = malloc(n + 8);
        if (tmp) {
            memcpy(tmp, "return ", 7);
            memcpy(tmp + 7, code, n + 1);
            if (cui__lua_load(s, tmp, n + 7)) {
                free(tmp);
                cui__lua_run(s, print_result);
                return;
            }
            free(tmp);
            lua_pop(s->L, 1);   /* not an expression: try statements */
        }
    }
    if (cui__lua_load(s, code, n)) {
        cui__lua_run(s, print_result >= 1 ? 1 : -1);
        return;
    }
    cui__script_error(s, lua_tostring(s->L, -1));
    lua_pop(s->L, 1);
}

/* ---- globals shared with the app ---- */

void cui__script_set_number(cui_script *s, const char *name, double v) {
    if (!s || !name) return;
    /* whole numbers go in as integers, so the console shows 3 and not 3.0 */
    if (v == (double)(lua_Integer)v) lua_pushinteger(s->L, (lua_Integer)v);
    else lua_pushnumber(s->L, v);
    lua_setglobal(s->L, name);
}

double cui__script_get_number(cui_script *s, const char *name) {
    if (!s || !name) return 0;
    lua_getglobal(s->L, name);
    double v = lua_isnumber(s->L, -1) ? lua_tonumber(s->L, -1) : 0;
    lua_pop(s->L, 1);
    return v;
}

void cui__script_set_string(cui_script *s, const char *name, const char *v) {
    if (!s || !name) return;
    if (v) lua_pushstring(s->L, v);
    else lua_pushnil(s->L);
    lua_setglobal(s->L, name);
}

const char *cui__script_get_string(cui_script *s, const char *name) {
    if (!s || !name) return NULL;
    lua_getglobal(s->L, name);
    const char *m = lua_tostring(s->L, -1);   /* stays valid while the global holds it */
    lua_pop(s->L, 1);
    return m;
}

/* ---- life ---- */

cui_terminal_buffer *cui__script_term(cui_script *s) { return s ? &s->term : NULL; }
void *cui__script_lua_impl(cui_script *s) { return s ? (void *)s->L : NULL; }

static void cui__lua_sandbox(lua_State *L) {
    lua_pushnil(L); lua_setglobal(L, "io");
    lua_pushnil(L); lua_setglobal(L, "require");
    static const char *os_gone[] = { "execute", "exit", "remove", "rename", "tmpname", NULL };
    lua_getglobal(L, "os");
    for (int k = 0; os_gone[k]; k++) {
        lua_pushnil(L);
        lua_setfield(L, -2, os_gone[k]);
    }
    lua_pop(L, 1);
}

cui_script *cui__script_new(void) {
    cui_script *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->text = malloc(CUI__LUA_TEXT);
    s->lines = malloc(sizeof(cui_terminal_line) * CUI__LUA_LINES);
    s->L = luaL_newstate();
    if (!s->text || !s->lines || !s->L) {
        free(s->text); free(s->lines);
        if (s->L) lua_close(s->L);
        free(s);
        return NULL;
    }
    cui_terminal_init(&s->term, s->text, CUI__LUA_TEXT, s->lines, CUI__LUA_LINES);
    luaL_openlibs(s->L);
    cui__lua_sandbox(s->L);
    cui__lua_ui_install(s);   /* the ui.* table for live widgets (cui_lua_ui.c) */
    lua_pushlightuserdata(s->L, s);
    lua_pushcclosure(s->L, cui__lua_print, 1);
    lua_setglobal(s->L, "print");
    cui_terminal_printf(&s->term, "\x1b[2mLua %s.%s.%s console\x1b[0m\n", LUA_VERSION_MAJOR, LUA_VERSION_MINOR, LUA_VERSION_RELEASE);
    return s;
}

void cui__script_free(cui_script *s) {
    if (!s) return;
    lua_close(s->L);
    free(s->text);
    free(s->lines);
    free(s);
}
