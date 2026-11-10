/* The Lua console driving the UI.
 *
 *     count = count + 1
 *     toast("3 times count is " .. count * 3)
 *     dark(true)
 *
 * The app is an editor: a 40-line field, a Run button under it that runs
 * whatever it holds, and the console below. Passing a .lua file on the command
 * line ("repl some.lua") loads its code into the field and runs it at once, so
 * it can be edited and Run again to see the results.
 */
#include <cui/cui.h>
#include <cui/cui_app.h>

#include <stdio.h>
#include <string.h>

typedef struct { cui_script *lua; cui_ctx *ui; bool dark; } rp_app;

#define SNIPPET_LINES 40                                    /* the run field shows 40 lines */
#define SNIPPET_CAP 131072                                  /* scripts up to 128 KiB fit */
static char snippet[SNIPPET_CAP] = "print('hello from the run button')";

static int toast_fn(void *ud, const char *const *args, int n, char *out, size_t cap) {
    (void)out; (void)cap;
    rp_app *a = ud;
    if (!a->ui || n < 1) return 0;
    cui_toast(a->ui, args[0]);
    return 0;
}

static int dark_fn(void *ud, const char *const *args, int n, char *out, size_t cap) {
    (void)out; (void)cap;
    rp_app *a = ud;
    a->dark = n >= 1 && args[0] && (!strcmp(args[0], "true") || !strcmp(args[0], "1"));
    if (a->ui) cui_set_theme(a->ui, a->dark ? CUI_THEME_DARK : CUI_THEME_LIGHT);
    return 0;
}

static void frame(cui_ctx *ui, void *user) {
    rp_app *a = user;
    a->ui = ui;
    /* same containers as the gallery's developer tools page: a scrolling
     * column, so every card keeps its box even when the window is short */
    cui_scroll(ui, .id = "main", .width = CUI_FILL, .height = CUI_FILL, .fade_edges = true) {
        cui_column(ui, .pad = 20, .gap = 12, .width = CUI_FILL) {
            cui_row(ui, .gap = 12, .align = CUI_ALIGN_CENTER) {
                cui_heading(ui, "Lua console", 3);
                cui_muted(ui, "edit and press Run, or type in the console");
                if (cui_switch(ui, "Dark", &a->dark)) cui_set_theme(ui, a->dark ? CUI_THEME_DARK : CUI_THEME_LIGHT);
            }
            cui_textarea(ui, "snippet", snippet, sizeof snippet, .width = CUI_FILL, .height = SNIPPET_LINES * 18, .placeholder = "lua to run");
            if (cui_button(ui, "Run", .icon = CUI_ICON_PLAY, .variant = CUI_BUTTON_PRIMARY))
                cui_script_eval(a->lua, snippet);
            cui_script_widget(ui, "panel", a->lua, "panel");
            cui_repl(ui, "console", a->lua, .width = CUI_FILL, .height = 300, .autofocus = true);
        }
    }
}

int main(int argc, char **argv) {
    static rp_app a;
    a.lua = cui_script_new();
    if (a.lua) {
        cui_script_set_fn(a.lua, "toast", toast_fn, &a);
        cui_script_set_fn(a.lua, "dark", dark_fn, &a);
        cui_script_set_number(a.lua, "count", 0);
        /* the script slot stays empty until a file or the console registers a
         * widget with widget(f); this empty fallback keeps it from reporting
         * a missing function in the meantime */
        cui_script_eval(a.lua, "function panel(ui, self) end");
        cui_script_printf(a.lua, "\x1b[2mtry: count = count + 1   toast(\"hello\")   dark(true)\x1b[0m\n");
        if (argc > 1) {                                        /* repl some.lua: put its code in the field and run it */
            FILE *f = fopen(argv[1], "rb");
            if (f) {
                size_t n = fread(snippet, 1, sizeof snippet - 1, f);
                snippet[n] = 0;
                fclose(f);
                cui_script_eval(a.lua, snippet);
            } else fprintf(stderr, "repl: cannot read %s\n", argv[1]);
        }
    }
    int r = cui_app_run(&(cui_app_desc){ .title = "Lua console", .width = 720, .height = 560 }, frame, &a);
    cui_script_free(a.lua);
    return r;
}
