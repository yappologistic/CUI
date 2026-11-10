/* Caret geometry of a single-line field with right-to-left and mixed text, checked against what Microsoft Edge
 * measured for the same strings (tests/field_edge.h). The model is built from Edge's own boxes, so no font is
 * needed. With a font as argv[1] (Vazirmatn), the model CUI builds itself is compared with Edge as well. */
#include "../src/cui_internal.h"
#include <cui/cui_render.h>

#define CHECK(x, ...) do { if (!(x)) { fprintf(stderr, __VA_ARGS__); fprintf(stderr, " (line %d)\n", __LINE__); exit(1); } } while (0)
#include "field_edge.h"

#define TOL 1.0f   /* Edge rounds its boxes to whole pixels */

static int cp_bytes(unsigned char c) { return c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4; }
static int byte_at(const char *s, int cp) { int b = 0; while (cp-- > 0) b += cp_bytes((unsigned char)s[b]); return b; }

static cui_edit_line from_edge(const fe_geom *g, cui_edit_cell *cell) {
    int at = 0;
    for (int k = 0; k < g->ng; k++) {
        cell[k].a = at;
        for (int c = 0; c < g->gcp[k]; c++) at += cp_bytes((unsigned char)g->text[at]);
        cell[k].b = at;
        cell[k].l = g->l[k]; cell[k].r = g->r[k]; cell[k].level = (uint8_t)g->level[k];
    }
    CHECK(g->text[at] == 0, "%s: graphemes do not cover the text", g->name);
    return (cui_edit_line){ .n = g->ng, .len = at, .base = g->base, .width = g->width, .cell = cell };
}

static void geometry(const fe_geom *g, int *checks) {
    cui_edit_cell cell[16];
    cui_edit_line L = from_edge(g, cell);
    for (int j = 0; j <= g->ng; j++) {
        int pos = j < g->ng ? cell[j].a : L.len;
        float x = cui__el_caret_x(&L, pos);
        CHECK(fabsf(x - g->caret[j]) <= TOL, "%s: caret at boundary %d is x=%.2f, Edge has %.2f", g->name, j, x, g->caret[j]);
        (*checks)++;
    }
    for (int k = 0; k < g->nclick; k++) {
        int pos = cui__el_hit(&L, g->click[k].x);
        int to = pos >= L.len ? g->ng : 0;   /* the boundary number: Edge counts graphemes, not code points */
        for (int j = 0; j < g->ng; j++) if (cell[j].a == pos) to = j;
        CHECK(to == g->click[k].to, "%s: click at x=%.0f puts the cursor at boundary %d, Edge at %d", g->name, g->click[k].x, to, g->click[k].to);
        (*checks)++;
    }
    for (int k = 0; k < g->nspan; k++) {
        cui_edit_span sp[16];
        int n = cui__el_spans(&L, byte_at(g->text, g->span[k].a), byte_at(g->text, g->span[k].b), sp, 16);
        CHECK(n == g->span[k].n, "%s: selection [%d,%d) has %d boxes, Edge %d", g->name, g->span[k].a, g->span[k].b, n, g->span[k].n);
        for (int m = 0; m < n; m++)
            CHECK(fabsf(sp[m].l - g->span[k].l[m]) <= TOL && fabsf(sp[m].r - g->span[k].r[m]) <= TOL,
                  "%s: selection [%d,%d) box %d is %.1f..%.1f, Edge %.1f..%.1f", g->name, g->span[k].a, g->span[k].b, m,
                  sp[m].l, sp[m].r, g->span[k].l[m], g->span[k].r[m]);
        (*checks)++;
    }
    /* stepping goes one grapheme in reading order and stops at both ends */
    for (int j = 0; j <= g->ng; j++) {
        int pos = j < g->ng ? cell[j].a : L.len;
        int fwd = cui__el_step(&L, pos, 1), back = cui__el_step(&L, pos, -1);
        CHECK(fwd == (j + 1 <= g->ng ? (j + 1 < g->ng ? cell[j + 1].a : L.len) : L.len), "%s: step forward from %d", g->name, j);
        CHECK(back == (j > 0 ? cell[j - 1].a : 0), "%s: step back from %d", g->name, j);
        (*checks)++;
    }
}

static void font_check(const char *font_path, int *checks) {
    cui_sw_renderer *sw = cui_sw_create();
    cui_ctx *ui = cui_create(&(cui_ctx_desc){ .renderer = cui_sw_iface(sw) });
    cui_font f = cui_font_load_file(ui, font_path);
    CHECK(f >= 0, "font load failed");
    cui_font_add_fallback(ui, CUI_FONT_BODY, f);
    cui_begin_frame(ui, 1.0f / 60);
    float worst = 0;
    for (size_t t = 0; t < sizeof fe_geoms / sizeof *fe_geoms; t++) {
        const fe_geom *g = &fe_geoms[t];
        cui_edit_line L;
        CHECK(cui__edit_line(ui, f, 16, g->text, (int)strlen(g->text), g->base, &L), "%s: no model", g->name);
        CHECK(L.n == g->ng, "%s: %d graphemes, Edge %d", g->name, L.n, g->ng);
        for (int j = 0; j <= g->ng; j++) {
            float x = cui__el_caret_x(&L, j < g->ng ? L.cell[j].a : L.len);
            printf("  %s caret %d: CUI %.2f Edge %.2f delta %+.2f\n", g->name, j, x, g->caret[j], x - g->caret[j]);
            if (g->name[0] == 'P') worst = fmaxf(worst, fabsf(x - g->caret[j]));   /* Persian only: Edge used Inter for Latin */
            (*checks)++;
        }
        for (int j = 0; j < L.n; j++) CHECK(L.cell[j].level == g->level[j], "%s: bidi level of grapheme %d is %d, Edge %d", g->name, j, L.cell[j].level, g->level[j]);
    }
    printf("font model: worst Persian caret delta %.2f px\n", worst);
    CHECK(worst <= 1.5f, "Persian carets differ from Edge by %.2f px", worst);
    /* lam and alef drawn as one ligature: the caret still stops between them, at the middle of the ligature */
    const char *la = "سلام لا";
    cui_edit_line L;
    CHECK(cui__edit_line(ui, f, 16, la, (int)strlen(la), 1, &L) && L.n == 7, "ligature line has no model");
    CHECK(L.cell[5].r - L.cell[5].l > 1 && fabsf((L.cell[5].r - L.cell[5].l) - (L.cell[6].r - L.cell[6].l)) < 0.01f,
          "ligature boxes: lam %.2f..%.2f, alef %.2f..%.2f", L.cell[5].l, L.cell[5].r, L.cell[6].l, L.cell[6].r);
    float xa = cui__el_caret_x(&L, L.cell[5].a), xm = cui__el_caret_x(&L, L.cell[6].a), xz = cui__el_caret_x(&L, L.len);
    CHECK(xa > xm + 1 && xm > xz + 1, "caret inside the ligature does not move: %.2f %.2f %.2f", xa, xm, xz);
    (*checks)++;
    /* a mark on the lam (fatha, shadda) has no width and is no ligature part of its own: the alef after it still gets half the ligature */
    const char *marked[] = { "\xD9\x84\xD9\x8E" "\xD8\xA7", "\xD9\x84\xD9\x91" "\xD8\xA7" };   /* lam fatha alef;  lam shadda alef */
    for (int k = 0; k < 2; k++) {
        CHECK(cui__edit_line(ui, f, 16, marked[k], (int)strlen(marked[k]), 1, &L) && L.n == 2, "marked ligature %d has no model", k);
        CHECK(L.cell[1].r - L.cell[1].l > 1 && fabsf((L.cell[1].r - L.cell[1].l) - (L.cell[0].r - L.cell[0].l)) < 0.01f,
              "marked ligature %d: boxes lam %.2f..%.2f, alef %.2f..%.2f, expected two halves", k, L.cell[0].l, L.cell[0].r, L.cell[1].l, L.cell[1].r);
        xa = cui__el_caret_x(&L, L.cell[0].a); xm = cui__el_caret_x(&L, L.cell[1].a); xz = cui__el_caret_x(&L, L.len);
        CHECK(xa > xm + 1 && xm > xz + 1, "marked ligature %d: the caret inside does not move: %.2f %.2f %.2f", k, xa, xm, xz);
        (*checks)++;
    }
    /* LRM and RLM have no width: the line gives each a cell of width 0 where it sits, so the caret stops on both
     * sides of it at one point (as Edge does) and the letter before it keeps its whole box */
    const char *lrm = "a\xE2\x80\x8E" "\xD8\xA8", *rlm = "\xD8\xA8\xE2\x80\x8F" "a";   /* a LRM beh;  beh RLM a */
    CHECK(cui__edit_line(ui, CUI_FONT_BODY, 16, lrm, (int)strlen(lrm), 0, &L) && L.n == 3, "a LRM seen has no model");
    CHECK(L.cell[0].r - L.cell[0].l > 4 && L.cell[1].r == L.cell[1].l && fabsf(L.cell[1].l - L.cell[0].r) < 0.01f,
          "a LRM seen: boxes a %.2f..%.2f, LRM %.2f..%.2f, expected the whole letter and an empty box after it", L.cell[0].l, L.cell[0].r, L.cell[1].l, L.cell[1].r);
    CHECK(fabsf(cui__el_caret_x(&L, L.cell[1].a) - L.cell[0].r) < 0.01f && fabsf(cui__el_caret_x(&L, L.cell[2].a) - L.cell[0].r) < 0.01f,
          "a LRM seen: the caret before and after the LRM is at %.2f and %.2f, expected the letter's edge %.2f", cui__el_caret_x(&L, L.cell[1].a), cui__el_caret_x(&L, L.cell[2].a), L.cell[0].r);
    CHECK(cui__edit_line(ui, CUI_FONT_BODY, 16, rlm, (int)strlen(rlm), 1, &L) && L.n == 3, "beh RLM a has no model");
    CHECK(L.cell[1].r == L.cell[1].l && fabsf(cui__el_caret_x(&L, L.cell[1].a) - L.cell[0].l) < 0.01f && fabsf(cui__el_caret_x(&L, L.cell[2].a) - L.cell[0].l) < 0.01f,
          "beh RLM a: the caret before and after the RLM is at %.2f and %.2f, expected the letter's edge %.2f", cui__el_caret_x(&L, L.cell[1].a), cui__el_caret_x(&L, L.cell[2].a), L.cell[0].l);
    (*checks) += 2;
    cui_end_frame(ui);
    cui_destroy(ui);
    cui_sw_destroy(sw);
}

int main(int argc, char **argv) {
    int checks = 0;
    for (size_t t = 0; t < sizeof fe_geoms / sizeof *fe_geoms; t++) geometry(&fe_geoms[t], &checks);
    printf("field model: %d checks against Edge passed (%d strings)\n", checks, (int)(sizeof fe_geoms / sizeof *fe_geoms));
    if (argc > 1) { checks = 0; font_check(argv[1], &checks); printf("font model: %d carets compared\n", checks); }
    else printf("font model: skipped, no font given\n");
    return 0;
}
