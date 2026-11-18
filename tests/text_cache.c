/* Compare optimized breaks with the original exact-candidate wrapper. */
#define CUI_TEXT_SHAPING
#include "../src/cui_text.c"
#include <stdio.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static int reference_wrap(cui_ctx *ui, cui_font font, float size, const char *text, int len,
                           float width, cui_text_layout_line *lines, int max_lines) {
    font = cui__resolve_font(ui, font);
    const char *saved = ui->text_paragraph;
    int saved_len = ui->text_paragraph_len;
    ui->text_paragraph = text; ui->text_paragraph_len = len;
    int n = 0, start = 0;
    for (;;) {
        int end = start, best = start, next = start;
        float best_width = 0;
        while (end < len && text[end] != '\n') {
            while (end < len && text[end] != ' ' && text[end] != '\t' && text[end] != '\n') end++;
            float w = cui__shape_run(ui, font, size, text + start, end - start, false, false, 0, 0, (cui_color){0}, 1e30f, NULL);
            if (w > width + 0.01f && best > start) break;
            best = end; best_width = w;
            next = end;
            while (next < len && (text[next] == ' ' || text[next] == '\t')) next++;
            if (next == len || text[next] == '\n') break;
            end = next;
        }
        if (lines && n < max_lines) lines[n] = (cui_text_layout_line){start, best, best_width};
        n++;
        if (next == len) break;
        start = next;
        if (text[start] == '\n') start++;
    }
    ui->text_paragraph = saved; ui->text_paragraph_len = saved_len;
    return n;
}
int main(int argc, char **argv) {
    cui_ctx *ui = cui_create(NULL); CHECK(ui);
    if (argc > 1) { cui_font f = cui_font_load_file(ui, argv[1]); CHECK(f >= 0); cui_font_set_role(ui, CUI_FONT_BODY, f); }
    const char *mixed = "سلام English";
    float expected = CUI_MAX(cui_measure_text(ui, CUI_FONT_BODY, 24, "سلام", -1).width,
                             cui_measure_text(ui, CUI_FONT_BODY, 24, "English", -1).width);
    float actual = cui__text_min_width(ui, CUI_FONT_BODY, 24, mixed, (int)strlen(mixed));
    cui_set_viewport(ui, 500, 200, 1);
    cui_begin_frame(ui, 1.0f/60);
    cui_row(ui, .width = CUI_PX(76)) {
        cui_text(ui, mixed, .size = 24, .align = CUI_TEXT_CENTER);
    }
    cui_end_frame(ui);
    float expected_box = CUI_MAX(76, ceilf(expected * 4) / 4);
    const cui_draw_data *draw = cui_get_draw_data(ui);
    CHECK(draw->vertex_count > 0);
    printf("mixed constrained: minimum=%.6f expected=%.6f box=%.6f expected_box=%.6f first_x=%.6f\n",
           actual, expected, ui->nodes[2].rect.w, expected_box, draw->vertices[0].x);
    CHECK(fabsf(actual - expected) < .001f);
    CHECK(fabsf(ui->nodes[2].rect.w - expected_box) < .001f);
    const char *samples[] = {
        "سلام دنیا امروز می‌خوام سه تا Timer و ۱۲۳ کار رو بررسی کنم.",
        "\nسلام\n\nدنیا\n", "سلامسلامسلامسلام دنیا", "  سلام  دنیا\t Timer 123 فارسی  ",
        "بِسْمِ اللَّهِ لَا نَشانهٔ فارسی (Timer) ۱۲۳!", "سلام abc ffi AV To لا دنیا."
    };
    float widths[] = {0, 1, 20, 60, 110, 240, 360, 640.5f};
    for (int s = 0; s < 6; s++) for (int sz = 14; sz <= 24; sz += 10) for (int w = 0; w < 8; w++) {
        const char *text = samples[s]; int len = (int)strlen(text);
        cui_text_layout_line a[128], b[128];
        int n = reference_wrap(ui, CUI_FONT_BODY, (float)sz, text, len, widths[w], a, 128);
        int m = cui__text_wrap(ui, CUI_FONT_BODY, (float)sz, text, len, widths[w], b, 128);
        CHECK(n == m);
        for (int j = 0; j < n; j++) {
            if (a[j].start != b[j].start || a[j].end != b[j].end || fabsf(a[j].width - b[j].width) > .001f)
                fprintf(stderr, "sample=%d size=%d width=%g line=%d expected=%d,%d,%g actual=%d,%d,%g\n", s, sz, widths[w], j, a[j].start, a[j].end, a[j].width, b[j].start, b[j].end, b[j].width);
            CHECK(a[j].start == b[j].start && a[j].end == b[j].end && fabsf(a[j].width - b[j].width) < .001f);
        }
        CHECK(cui__text_wrap(ui, CUI_FONT_BODY, (float)sz, text, len, widths[w], NULL, 0) == n);
        CHECK(cui__text_wrap(ui, CUI_FONT_BODY, (float)sz, text, len, widths[w], b, 1) == n);
    }
    cui__shape_cache_clear(ui);
    const char *text = samples[0]; int len = (int)strlen(text);
    float minimum = cui__text_min_width(ui, CUI_FONT_BODY, 14, text, len);
    CHECK(ui->shape_cache_count == 1 && !ui->text_paragraph);
    float pref, minc, again, min_again;
    cui__text_intrinsics(ui, CUI_FONT_BODY, 14, text, len, false, &pref, &minc);
    CHECK(minc == minimum && minc > 0 && pref > minc);
    cui_shaped_line *head = ui->shape_cache->lines;
    cui__text_intrinsics(ui, CUI_FONT_BODY, 14, text, len, false, &again, &min_again);
    CHECK(pref == again && minc == min_again && head == ui->shape_cache->lines);
    cui__text_intrinsics(ui, CUI_FONT_BODY, 24, text, len, true, &again, &min_again);
    CHECK(again > pref && min_again > minc);
    cui_font_set_role(ui, CUI_FONT_BODY, CUI_FONT_MONO);
    CHECK(ui->shape_cache_count == 0);
    cui__text_intrinsics(ui, CUI_FONT_BODY, 14, text, len, false, &again, &min_again);
    CHECK(fabsf(again - cui__text_width(ui, CUI_FONT_BODY, 14, text, len, false)) < .001f);
    cui_destroy(ui);
    puts("96 exact-wrap comparisons, paragraph ownership, metrics and font invalidation passed");
    return 0;
}
