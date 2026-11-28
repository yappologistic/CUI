/* Grove: a mind map. Ideas sit freely on a board that pans and zooms, hold notes, checklists,
 * a picture or a video link, and are joined by lines you style and label. Pull a line from an
 * idea's handle onto another idea to join them, or onto empty space for a new idea; double-click
 * empty space for one too, right-click for a menu, and Alt with the arrows walks along the
 * lines. The panel on the right edits what is selected. It shows the board with its lines,
 * labels and context menu, and the components hosted inside its items. */
#include <cui/cui.h>
#include <cui/cui_app.h>
#include <stdio.h>
#include <string.h>

#define MAX_IDEAS 64
#define MAX_LINES 128
#define MAX_TASKS 6

typedef enum { KIND_NOTE, KIND_TASKS, KIND_PICTURE, KIND_VIDEO } idea_kind;

typedef struct {
    bool alive;
    cui_rect rect;                       /* on the board, in canvas points; h 0 follows the content */
    char title[48];
    char note[160];
    idea_kind kind;
    int tone;                            /* index into tones[] */
    char tasks[MAX_TASKS][40];
    bool done[MAX_TASKS];
    int task_count;
    const char *url;                     /* a video's page, opened in the browser */
} idea;

typedef struct {
    int id, from, to;
    char label[32];
    int path, dash;                      /* cui_board_path, cui_board_dash */
    bool arrow;
} line;

typedef struct {
    cui_node_view view;
    char name[48];
    idea ideas[MAX_IDEAS];
    line lines[MAX_LINES];
    int idea_count, line_count, next_line;
    int selected_idea, selected_line;    /* -1 when not exactly one */
    bool minimap, panel;
    bool started;                        /* the example map is made on the first frame */
    cui_texture picture, video;          /* this frame's, from pictures() */
} app_state;

static const cui_tone tones[] = { CUI_TONE_NEUTRAL, CUI_TONE_ACCENT, CUI_TONE_INFO, CUI_TONE_VIOLET, CUI_TONE_TEAL, CUI_TONE_ROSE, CUI_TONE_WARNING };
static const char *const tone_names[] = { "Plain", "Accent", "Sky", "Violet", "Teal", "Rose", "Amber" };

static int add_idea(app_state *a, float x, float y, const char *title, int tone) {
    if (a->idea_count >= MAX_IDEAS) return -1;
    int i = a->idea_count++;
    idea *d = &a->ideas[i];
    memset(d, 0, sizeof *d);
    d->alive = true;
    d->rect = (cui_rect){ x, y, 180, 0 };
    snprintf(d->title, sizeof d->title, "%s", title);
    d->tone = tone;
    return i;
}

static int add_line(app_state *a, int from, int to, const char *label) {
    if (a->line_count >= MAX_LINES) return -1;
    line *l = &a->lines[a->line_count++];
    memset(l, 0, sizeof *l);
    l->id = a->next_line++; l->from = from; l->to = to;
    l->arrow = true;
    snprintf(l->label, sizeof l->label, "%s", label ? label : "");
    return l->id;
}

static void remove_line(app_state *a, int id) {
    for (int k = 0; k < a->line_count; k++)
        if (a->lines[k].id == id) { a->lines[k] = a->lines[--a->line_count]; return; }
}

static void remove_idea(app_state *a, int i) {
    a->ideas[i].alive = false;
    for (int k = a->line_count - 1; k >= 0; k--)
        if (a->lines[k].from == i || a->lines[k].to == i) a->lines[k] = a->lines[--a->line_count];
}

static void add_task(idea *d, const char *text, bool done) {
    if (d->task_count >= MAX_TASKS) return;
    snprintf(d->tasks[d->task_count], sizeof d->tasks[0], "%s", text);
    d->done[d->task_count++] = done;
}

static void seed(app_state *a) {
    a->minimap = a->panel = true;
    a->next_line = 1;
    snprintf(a->name, sizeof a->name, "Summer festival");
    int c = add_idea(a, 290, 210, "Summer festival", 1);
    snprintf(a->ideas[c].note, sizeof a->ideas[c].note, "Two days in the park, end of July");
    int p = add_idea(a, 30, 30, "Program", 2);
    add_task(&a->ideas[p], "Book two bands", true);
    add_task(&a->ideas[p], "Open mic on Sunday", false);
    add_task(&a->ideas[p], "Kids' corner", false);
    a->ideas[p].kind = KIND_TASKS;
    int v = add_idea(a, 540, 20, "Venue", 4);
    a->ideas[v].kind = KIND_PICTURE;
    snprintf(a->ideas[v].note, sizeof a->ideas[v].note, "The meadow by the lake");
    int m = add_idea(a, 540, 280, "Teaser video", 5);
    a->ideas[m].kind = KIND_VIDEO;
    a->ideas[m].url = "https://www.youtube.com/results?search_query=summer+festival";
    int f = add_idea(a, 30, 330, "Food", 6);
    snprintf(a->ideas[f].note, sizeof a->ideas[f].note, "Three trucks, one of them vegan");
    int s = add_idea(a, 250, 440, "Ask the city about permits", 0);
    a->ideas[s].rect.w = 250;
    add_line(a, c, p, "first");
    add_line(a, c, v, NULL);
    add_line(a, c, m, "after the venue");
    add_line(a, c, f, NULL);
    add_line(a, f, s, NULL);
    a->lines[a->line_count - 1].dash = CUI_BOARD_DASHED;
    a->lines[a->line_count - 1].arrow = false;
}

/* a small picture drawn in code, so the example needs no files: a sky, a sun and two hills */
static cui_texture make_picture(cui_ctx *ui, bool night) {
    enum { W = 160, H = 90 };
    static unsigned char px[W * H * 4];
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            unsigned char *p = &px[(y * W + x) * 4];
            float t = (float)y / H;
            int r = night ? (int)(30 + 40 * t) : (int)(120 + 100 * t), g = night ? (int)(30 + 30 * t) : (int)(170 + 60 * t), b = night ? (int)(70 + 40 * t) : 235;
            float sx = x - 118.0f, sy = y - 26.0f;
            if (sx * sx + sy * sy < 120) { r = 255; g = night ? 240 : 210; b = night ? 200 : 90; }
            float hill1 = 62 + 10 * (float)((x - 40) * (x - 40)) / 1600.0f, hill2 = 70 + 8 * (float)((x - 120) * (x - 120)) / 1600.0f;
            if (y > hill1) { r = night ? 30 : 90; g = night ? 60 : 150; b = night ? 40 : 80; }
            if (y > hill2) { r = night ? 20 : 60; g = night ? 45 : 120; b = night ? 30 : 60; }
            p[0] = (unsigned char)r; p[1] = (unsigned char)g; p[2] = (unsigned char)b; p[3] = 255;
        }
    return cui_image_create(ui, W, H, px);
}

/* The two pictures belong to one context, so they live in its state store and are freed with it. */
typedef struct { cui_texture picture, video; } pictures_held;
static void pictures_free(cui_ctx *ui, void *data) {
    pictures_held *p = data;
    cui_image_destroy(ui, p->picture);
    cui_image_destroy(ui, p->video);
}
static pictures_held *pictures(cui_ctx *ui) {
    cui_id id = cui_make_id(ui, "grove-pictures");
    pictures_held *p = cui_state(ui, id, sizeof *p);
    if (!p->picture) {
        p->picture = make_picture(ui, false);
        p->video = make_picture(ui, true);
        cui_state_on_free(ui, id, pictures_free);
    }
    return p;
}

/* what goes inside an idea: its title, then what its kind holds */
static void idea_content(cui_ctx *ui, app_state *a, idea *d) {
    cui_inline_edit(ui, "##title", d->title, sizeof d->title, .width = CUI_FILL);
    switch (d->kind) {
    case KIND_TASKS:
        for (int t = 0; t < d->task_count; t++) { cui_push_id_int(ui, t); cui_checkbox(ui, d->tasks[t], &d->done[t]); cui_pop_id(ui); }
        break;
    case KIND_PICTURE:
        cui_image(ui, a->picture, .width = CUI_FILL, .height = CUI_PX(88), .radius = 6);
        break;
    case KIND_VIDEO:
        cui_box(ui, .dir = CUI_STACK, .width = CUI_FILL, .height = CUI_PX(88), .align = CUI_ALIGN_CENTER, .justify = CUI_JUSTIFY_CENTER) {
            cui_image(ui, a->video, .width = CUI_FILL, .height = CUI_PX(88), .radius = 6);
            cui_box(ui, .pad = 8, .radius = 18, .bg = CUI_RGBA(0x00000096)) { cui_icon(ui, CUI_ICON_PLAY, 18, CUI_RGB(0xffffff)); }
        }
        if (cui_button(ui, "Watch on YouTube", .size = CUI_SIZE_SM, .icon = CUI_ICON_EXTERNAL_LINK, .variant = CUI_BUTTON_OUTLINE)) cui_app_open_url(d->url);
        break;
    default: break;
    }
    if (d->note[0]) cui_text(ui, d->note, .size = 12, .color = CUI_COLOR_TEXT_SECONDARY);
}

static void board(cui_ctx *ui, app_state *a) {
    a->selected_idea = a->selected_line = -1;
    int selected = 0;
    if (cui_board_begin(ui, "map", &a->view, .height = CUI_FILL, .minimap = a->minimap, .snap = 4)) {
        /* what the person did, first, so a deleted idea is not built: the board reports it, the app changes its data */
        int id, from, to;
        float x, y;
        while (cui_board_item_deleted(ui, &id)) a->ideas[id].alive = false;
        while (cui_board_link_removed(ui, &id)) remove_line(a, id);
        while (cui_board_link_created(ui, &from, &to)) add_line(a, from, to, NULL);
        while (cui_board_link_dropped(ui, &from, &x, &y)) {
            int n = add_idea(a, x - 90, y - 20, "New idea", a->ideas[from].tone);
            if (n >= 0) add_line(a, from, n, NULL);
        }
        while (cui_board_activated(ui, &id, &x, &y)) if (id < 0) add_idea(a, x - 90, y - 20, "New idea", 0);
        for (int i = 0; i < a->idea_count; i++) {
            idea *d = &a->ideas[i];
            if (!d->alive) continue;
            cui_board_item(ui, i, &d->rect, .tone = tones[d->tone], .radius = d->kind == KIND_NOTE && !d->note[0] ? -1.0f : 0.0f) {
                cui_push_id_int(ui, i);
                idea_content(ui, a, d);
                cui_pop_id(ui);
            }
            if (cui_board_selected(ui, i)) { selected++; a->selected_idea = i; }
        }
        if (selected != 1) a->selected_idea = -1;
        for (int k = 0; k < a->line_count; k++) {
            line *l = &a->lines[k];
            if (cui_board_link(ui, l->id, l->from, l->to, .path = (cui_board_path)l->path, .dash = (cui_board_dash)l->dash,
                               .end_cap = l->arrow ? CUI_BOARD_CAP_ARROW : CUI_BOARD_CAP_NONE, .tone = tones[a->ideas[l->to].tone],
                               .label_edit = l->label, .label_cap = sizeof l->label))
                a->selected_line = l->id;
        }
        cui_board_end(ui);
    }
    cui_board_target t;
    cui_board_menu(ui, "map", &t) {
        if (t.item >= 0) {
            if (cui_menu_item(ui, "Add an idea joined to it", .icon = CUI_ICON_PLUS)) {
                idea *d = &a->ideas[t.item];
                int n = add_idea(a, d->rect.x + d->rect.w + 60, d->rect.y, "New idea", d->tone);
                if (n >= 0) add_line(a, t.item, n, NULL);
            }
            if (cui_menu_item(ui, "Delete idea", .icon = CUI_ICON_TRASH_2, .danger = true)) remove_idea(a, t.item);
        } else if (t.link >= 0) {
            if (cui_menu_item(ui, "Delete line", .icon = CUI_ICON_TRASH_2, .danger = true)) remove_line(a, t.link);
        } else if (cui_menu_item(ui, "New idea here", .icon = CUI_ICON_PLUS)) {
            add_idea(a, t.x - 90, t.y - 20, "New idea", 0);
        }
    }
    cui_canvas_controls(ui, "zoom", &a->view, .content = (cui_rect){ 30, 20, 690, 500 });
}

/* the panel: what is selected, or how to use the board */
static void panel(cui_ctx *ui, app_state *a) {
    static const char *const kinds[] = { "Text", "List", "Image", "Video" };
    static const char *const paths[] = { "Curve", "Straight", "Elbow" };
    static const char *const dashes[] = { "Solid", "Dashed", "Dotted" };
    if (a->selected_idea >= 0) {
        idea *d = &a->ideas[a->selected_idea];
        cui_heading(ui, "Idea", 4);
        cui_input(ui, "title", d->title, sizeof d->title, .label = "Title");
        cui_textarea(ui, "note", d->note, sizeof d->note, .label = "Note", .placeholder = "A line or two");
        cui_label(ui, "Holds");
        int kind = (int)d->kind;
        if (cui_segmented(ui, "kind", kinds, 4, &kind, .full_width = true)) {
            d->kind = (idea_kind)kind;
            if (d->kind == KIND_TASKS && d->task_count == 0) add_task(d, "First step", false);
            if (d->kind == KIND_VIDEO && !d->url) d->url = "https://www.youtube.com/";
        }
        if (d->kind == KIND_TASKS && d->task_count < MAX_TASKS && cui_button(ui, "Add a task", .icon = CUI_ICON_PLUS, .size = CUI_SIZE_SM))
            add_task(d, "Next step", false);
        cui_label(ui, "Color");
        cui_row(ui, .gap = 6, .wrap = true) {
            for (int k = 0; k < (int)(sizeof tones / sizeof tones[0]); k++) {
                cui_push_id_int(ui, k);
                if (cui_button(ui, tone_names[k], .size = CUI_SIZE_SM, .selected = d->tone == k, .variant = CUI_BUTTON_OUTLINE)) d->tone = k;
                cui_pop_id(ui);
            }
        }
        if (cui_button(ui, "Delete idea", .icon = CUI_ICON_TRASH_2, .variant = CUI_BUTTON_GHOST)) remove_idea(a, a->selected_idea);
    } else if (a->selected_line >= 0) {
        line *l = NULL;
        for (int k = 0; k < a->line_count; k++) if (a->lines[k].id == a->selected_line) l = &a->lines[k];
        if (!l) return;
        cui_heading(ui, "Line", 4);
        cui_input(ui, "label", l->label, sizeof l->label, .label = "Label", .placeholder = "What joins them");
        cui_label(ui, "Path");
        cui_segmented(ui, "path", paths, 3, &l->path, .full_width = true);
        cui_label(ui, "Stroke");
        cui_segmented(ui, "dash", dashes, 3, &l->dash, .full_width = true);
        cui_checkbox(ui, "Arrowhead", &l->arrow);
        if (cui_button(ui, "Delete line", .icon = CUI_ICON_TRASH_2, .variant = CUI_BUTTON_GHOST)) remove_line(a, l->id);
    } else {
        int ideas = 0;
        for (int i = 0; i < a->idea_count; i++) ideas += a->ideas[i].alive;
        char count[64];
        snprintf(count, sizeof count, "%d ideas, %d lines", ideas, a->line_count);
        cui_heading(ui, "Map", 4);
        cui_secondary(ui, count);
        cui_separator(ui);
        static const char *const tips[] = {
            "Pull a line from an idea's handle onto another idea, or onto empty space for a new one",
            "Double-click empty space for a new idea",
            "Click a title or a label to rename it",
            "Right-click for a menu",
            "Drag empty space to pan, Shift and drag to select several",
            "Alt and the arrows walk along the lines",
        };
        for (int k = 0; k < (int)(sizeof tips / sizeof tips[0]); k++) cui_text(ui, tips[k], .size = 13, .color = CUI_COLOR_TEXT_SECONDARY);
    }
}

static void frame(cui_ctx *ui, void *user) {
    app_state *a = user;
    if (!a->started) { seed(a); a->started = true; }
    pictures_held *held = pictures(ui);
    a->picture = held->picture; a->video = held->video;
    bool narrow = cui_viewport_size(ui).x < 800;   /* a phone or a small window: the board alone */
    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL, .bg = CUI_COLOR_BACKGROUND) {
        cui_row(ui, .width = CUI_FILL, .pad_x = 16, .pad_y = 10, .gap = 10, .align = CUI_ALIGN_CENTER) {
            cui_icon(ui, CUI_ICON_SPARKLES, 18, CUI_COLOR_ACCENT);
            cui_text(ui, "Grove", .font = CUI_FONT_MEDIUM);
            cui_box(ui, .width = CUI_FILL) { cui_inline_edit(ui, "name", a->name, sizeof a->name); }
            if (cui_button(ui, "New idea", .icon = CUI_ICON_PLUS, .variant = CUI_BUTTON_PRIMARY)) {
                cui_vec2 c = { (300 - a->view.x) / (a->view.zoom > 0 ? a->view.zoom : 1), (200 - a->view.y) / (a->view.zoom > 0 ? a->view.zoom : 1) };
                add_idea(a, c.x, c.y, "New idea", 0);
            }
            if (cui_icon_button(ui, CUI_ICON_COMPASS, .tooltip = "Minimap", .selected = a->minimap)) a->minimap = !a->minimap;
            if (!narrow && cui_icon_button(ui, CUI_ICON_PANEL_LEFT, .tooltip = "Panel", .selected = a->panel)) a->panel = !a->panel;
        }
        cui_separator(ui);
        cui_row(ui, .width = CUI_FILL, .height = CUI_FILL) {
            cui_box(ui, .width = CUI_FILL, .height = CUI_FILL, .pad = 12, .dir = CUI_STACK) { board(ui, a); }
            if (a->panel && !narrow) {
                cui_separator(ui);
                cui_column(ui, .width = CUI_PX(320), .height = CUI_FILL, .pad = 16, .gap = 10, .scroll_y = true) { panel(ui, a); }
            }
        }
    }
}

int main(void) {
    static app_state a;
    return cui_app_run(&(cui_app_desc){ .title = "Grove", .width = 1280, .height = 800 }, frame, &a);
}
