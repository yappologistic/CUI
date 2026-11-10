/* Editing a single-line field with right-to-left and mixed text: arrows, Delete, mouse, alignment and scroll,
 * replayed against what Microsoft Edge does for the same keys (tests/field_edge.h). Needs no font (the order
 * of the text and the sides hold without one); an optional font argument adds Vazirmatn for real glyph widths. */
#include "../src/cui_internal.h"
#include <cui/cui_render.h>

#define CHECK(x, ...) do { if (!(x)) { fprintf(stderr, __VA_ARGS__); fprintf(stderr, " (line %d)\n", __LINE__); exit(1); } } while (0)
#include "field_edge.h"

static char buf[512];
static cui_rect ime;   /* where the field told the platform its caret is */
static int checks;
static void on_ime(void *user, cui_rect r) { (void)user; ime = r; }

static cui_ctx *make(cui_sw_renderer *sw, cui_direction dir, const char *font) {
    cui_ctx *ui = cui_create(&(cui_ctx_desc){ .renderer = cui_sw_iface(sw), .direction = dir });
    cui_set_viewport(ui, 400, 120, 1);
    cui_set_reduced_motion(ui, true);
    cui_platform_iface pf = { .set_text_input_rect = on_ime };
    cui_set_platform(ui, &pf);
    if (font) {
        cui_font f = cui_font_load_file(ui, font);
        CHECK(f >= 0, "font load failed");
        cui_font_add_fallback(ui, CUI_FONT_BODY, f);
    }
    return ui;
}
static void frame(cui_ctx *ui) {
    cui_begin_frame(ui, 1.0f / 60);
    cui_column(ui, .width = CUI_FILL, .pad = 20) { cui_input(ui, "f", buf, sizeof buf, .autofocus = true, .width = CUI_PX(300)); }
    cui_end_frame(ui);
}
static cui_rect area(cui_ctx *ui) { return cui__prev_rect(ui, cui_make_id(ui, "f") ^ 0xED17u); }
static void press(cui_ctx *ui, cui_key k, int mods) {
    cui_input_key(ui, k, true, mods); frame(ui);
    cui_input_key(ui, k, false, mods); frame(ui);
}
static int cp_bytes(unsigned char c) { return c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4; }
static int byte_at(const char *s, int cp) { int b = 0; while (cp-- > 0) b += cp_bytes((unsigned char)s[b]); return b; }
static int cp_of(const char *s, int byte) { int c = 0, b = 0; while (b < byte) { b += cp_bytes((unsigned char)s[b]); c++; } return c; }
/* the text in the field, the cursor and the anchor at the given code points */
static cui_edit_state *put(cui_ctx *ui, const char *text, int cursor, int anchor) {
    snprintf(buf, sizeof buf, "%s", text);
    frame(ui); frame(ui);
    cui_edit_state *e = ui->edit;
    CHECK(e, "the field has no focus");
    e->cursor = byte_at(text, cursor); e->anchor = byte_at(text, anchor); e->scroll_x = 0;
    return e;
}
static cui_key key_of(char c) { return c == 'H' ? CUI_KEY_HOME : c == 'E' ? CUI_KEY_END : c == 'L' ? CUI_KEY_LEFT : CUI_KEY_RIGHT; }

static void walks(cui_sw_renderer *sw, const char *font) {
    for (size_t t = 0; t < sizeof fe_walk_cases / sizeof *fe_walk_cases; t++) {
        const fe_walks *c = &fe_walk_cases[t];
        for (int shift = 0; shift < 2; shift++) {
            int count = shift ? c->nshift : c->nwalk;
            for (int w = 0; w < count; w++) {
                const fe_walk *k = shift ? &c->shift[w] : &c->walk[w];
                cui_ctx *ui = make(sw, CUI_DIRECTION_LTR, font);
                cui_edit_state *e = put(ui, c->text, 0, 0);
                if (shift) {   /* Edge's shift walks start with the caret at the end Home or End goes to */
                    press(ui, key_of(k->seq[0]), 0);
                    e = ui->edit;
                }
                for (int i = 0; i < k->n; i++) {
                    press(ui, key_of(k->seq[i]), shift ? CUI_MOD_SHIFT : 0);
                    e = ui->edit;
                    int got = cp_of(c->text, e->cursor);
                    CHECK(got == k->off[i], "%s %s%s: key %d of %s leaves the cursor at %d, Edge at %d", c->name, shift ? "shift " : "", "walk",
                          i + 1, k->seq, got, k->off[i]);
                    if (shift) {
                        int a = cp_of(c->text, e->anchor);
                        CHECK(a == k->anchor[i], "%s shift walk: key %d of %s leaves the anchor at %d, Edge at %d", c->name, i + 1, k->seq, a, k->anchor[i]);
                    }
                    checks++;
                }
                cui_destroy(ui);
            }
        }
    }
    printf("arrows, Home, End, Shift: %d steps match Edge\n", checks);
}

static void deletes(cui_sw_renderer *sw, const char *font) {
    int before = checks;
    for (size_t t = 0; t < sizeof fe_del_cases / sizeof *fe_del_cases; t++) {
        const fe_dels *c = &fe_del_cases[t];
        for (int i = 0; i < c->n; i++) {
            const fe_del *d = &c->d[i];
            cui_ctx *ui = make(sw, CUI_DIRECTION_LTR, font);
            put(ui, c->text, d->at, d->at);
            press(ui, d->key ? CUI_KEY_DELETE : CUI_KEY_BACKSPACE, 0);
            int cur = cp_of(buf, ui->edit->cursor);
            CHECK(!strcmp(buf, d->result) && cur == d->cursor, "%s: %s at %d gives \"%s\" with the cursor at %d, Edge \"%s\" at %d",
                  c->name, d->key ? "Delete" : "Backspace", d->at, buf, cur, d->result, d->cursor);
            checks++;
            cui_destroy(ui);
        }
    }
    printf("Backspace and Delete: %d cases match Edge\n", checks - before);
}

static void alignment(cui_sw_renderer *sw, const char *font) {
    /* the first Persian letter in an empty left-to-right field moves the caret to the right side */
    cui_ctx *ui = make(sw, CUI_DIRECTION_LTR, font);
    put(ui, "", 0, 0);
    cui_rect a = area(ui);
    CHECK(a.w > 100, "field area missing");
    CHECK(ime.x < a.x + 2, "empty field: caret at %.1f, expected the left edge %.1f", ime.x, a.x);
    cui_input_text(ui, "س"); frame(ui); frame(ui);
    CHECK(ime.x > a.x + a.w - 40 && ime.x <= a.x + a.w + 0.5f, "first Persian letter: caret at %.1f, expected near the right edge %.1f", ime.x, a.x + a.w);
    float one = ime.x;
    cui_input_text(ui, "ل"); frame(ui); frame(ui);
    CHECK(ime.x < one - 1 && ime.x > a.x + a.w - 80, "second Persian letter: caret %.1f must move left of %.1f", ime.x, one);
    cui_destroy(ui);
    /* Latin text starts at the left again */
    ui = make(sw, CUI_DIRECTION_LTR, font);
    put(ui, "", 0, 0);
    cui_input_text(ui, "ab"); frame(ui); frame(ui);
    CHECK(ime.x < a.x + 40, "Latin text: caret at %.1f, expected the left side", ime.x);
    cui_destroy(ui);
    /* an empty field in a right-to-left screen keeps its caret on the right edge */
    ui = make(sw, CUI_DIRECTION_RTL, font);
    put(ui, "", 0, 0);
    a = area(ui);
    CHECK(ime.x > a.x + a.w - 2 && ime.x <= a.x + a.w + 0.5f, "empty field in an RTL screen: caret at %.1f, expected the right edge %.1f", ime.x, a.x + a.w);
    cui_destroy(ui);
    checks += 4;
    printf("alignment: left, right, and the empty field in an RTL screen passed\n");
}

static void mouse(cui_sw_renderer *sw, const char *font) {
    const char *text = fe_geoms[0].text;   /* P3, right-to-left: its start is at the right */
    cui_ctx *ui = make(sw, CUI_DIRECTION_LTR, font);
    put(ui, text, 0, 0);
    cui_rect a = area(ui);
    float ys = a.y + a.h / 2;
    for (int side = 0; side < 2; side++) {
        float x = side ? a.x + a.w - 1 : a.x + 1;
        cui_input_mouse_move(ui, x, ys); frame(ui);
        cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true); frame(ui);
        cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false); frame(ui);
        int at = cp_of(text, ui->edit->cursor);
        int want = side ? 0 : cp_of(text, (int)strlen(text));
        CHECK(at == want, "click at the %s edge puts the cursor at %d, expected %d", side ? "right" : "left", at, want);
        checks++;
    }
    /* dragging from the right end to the left selects the whole word */
    cui_input_mouse_move(ui, a.x + a.w - 1, ys); frame(ui);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true); frame(ui);
    cui_input_mouse_move(ui, a.x + 1, ys); frame(ui);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false); frame(ui);
    CHECK(ui->edit->anchor == 0 && ui->edit->cursor == (int)strlen(text), "drag across the word selects %d..%d", ui->edit->anchor, ui->edit->cursor);
    cui_destroy(ui);
    checks++;
    printf("mouse: click at either edge and a drag across a right-to-left word passed\n");
}

static void overflow(cui_sw_renderer *sw, const char *font) {
    char text[480] = "";
    for (int k = 0; k < 12; k++) strcat(text, "سلام دنیا ");
    cui_ctx *ui = make(sw, CUI_DIRECTION_LTR, font);
    cui_edit_state *e = put(ui, text, 0, 0);
    cui_rect a = area(ui);
    press(ui, CUI_KEY_END, 0);
    e = ui->edit;
    CHECK(e->cursor == (int)strlen(text), "End did not reach the end");
    CHECK(e->scroll_x > 0, "long Persian text did not scroll");
    CHECK(ime.x >= a.x - 0.5f && ime.x <= a.x + a.w + 0.5f, "caret at the end is outside the field: %.1f not in %.1f..%.1f", ime.x, a.x, a.x + a.w);
    CHECK(ime.x < a.x + 8, "caret at the end of Persian text should sit at the left edge, not %.1f", ime.x);
    press(ui, CUI_KEY_HOME, 0);
    e = ui->edit;
    CHECK(e->cursor == 0 && e->scroll_x == 0, "Home: cursor %d, scroll %.1f", e->cursor, e->scroll_x);
    CHECK(ime.x > a.x + a.w - 2 && ime.x <= a.x + a.w + 0.5f, "Home: caret at %.1f, expected the right edge %.1f", ime.x, a.x + a.w);
    cui_destroy(ui);
    checks += 5;
    printf("overflow: caret stays inside the field at the end, Home returns to the right edge\n");
}

int main(int argc, char **argv) {
    const char *font = argc > 1 ? argv[1] : NULL;
    cui_sw_renderer *sw = cui_sw_create();
    walks(sw, font); deletes(sw, font); alignment(sw, font); mouse(sw, font); overflow(sw, font);
    printf("field edit: %d checks passed%s\n", checks, font ? " (with font)" : " (no font)");
    cui_sw_destroy(sw);
    return 0;
}
