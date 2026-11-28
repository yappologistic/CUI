/* Node editor: a canvas of nodes with pins, joined by curved links. It pans
 * and zooms; the caller owns the nodes, pins and links, and the editor
 * reports what the user did (links made and removed, nodes deleted) and moves
 * the node positions it was given.
 *
 * Everything the editor draws itself (cards, titles, labels, pins, wires,
 * the grid) is drawn at the current zoom, so text is rasterized at its zoomed
 * size and stays sharp. Components hosted inside a node are laid out at 100%
 * and scaled with the view; they stay interactive at any zoom (the pointer is
 * mapped into their space while they are built).
 *
 * The first part of the file, the node canvas (cui__nc_*), is what any canvas
 * of boxes needs whatever the boxes hold: the view, id sets, bounds and
 * framing, the marquee, the grid, the minimap, hosted content and paint
 * order. The node editor below adds cards with pins and wires. */
#include "cui_internal.h"

/* ------------------------------------------------------------------------ */
/* Node canvas                                                              */
/* ------------------------------------------------------------------------ */

#define CUI__NC_GRID     24.0f
#define CUI__NC_DRAG     3.0f     /* travel before a press becomes a drag */

typedef struct { int id; float x, y, w, h; } cui__nc_box;   /* canvas units */
typedef struct { int *v; int n, cap; } cui__nc_ints;

/* The view while a canvas builds: last frame's rectangle and where canvas
 * (0, 0) sits on the screen. */
typedef struct {
    cui_node_view *view;
    float z, min_zoom, max_zoom;
    cui_rect r;                           /* the canvas, last frame */
    float ox, oy;                         /* screen position of canvas (0, 0) */
    bool has_rect;
} cui__nc_view;

/* where the minimap was painted, for a press on it in the next frame */
typedef struct { float x, y, s, bx, by; bool valid; cui_rect rect; } cui__nc_minimap;

/* the pointer kept while hosted content builds in its own unscaled space */
typedef struct { float sx, sy; cui_vec2 saved_pointer; float saved_pmx, saved_pmy; } cui__nc_host;

/* the card nodes built this frame and whether each paints raised (frame memory) */
typedef struct { int *idx; bool *raise; int n, cap; } cui__nc_cards;

static void cui__nc_push(cui_ctx *ui, cui__nc_ints *a, int v) {
    CUI__GROW(ui, a->v, a->n, a->cap, 1);
    a->v[a->n++] = v;
}
static bool cui__nc_has(const cui__nc_ints *a, int v) {
    for (int i = 0; i < a->n; i++) if (a->v[i] == v) return true;
    return false;
}
static void cui__nc_remove(cui__nc_ints *a, int v) {
    for (int i = 0; i < a->n; i++) if (a->v[i] == v) { a->v[i] = a->v[--a->n]; return; }
}

static cui_vec2 cui__nc_screen(const cui__nc_view *v, float x, float y) {
    cui_vec2 s = { v->ox + x * v->z, v->oy + y * v->z };
    return s;
}

/* distance from p to the segment a-b, for picking a line under the pointer */
static float cui__nc_seg_dist(cui_vec2 p, cui_vec2 a, cui_vec2 b) {
    float vx = b.x - a.x, vy = b.y - a.y, wx = p.x - a.x, wy = p.y - a.y;
    float l2 = vx * vx + vy * vy;
    float t = l2 > 0 ? cui__clampf((wx * vx + wy * vy) / l2, 0, 1) : 0;
    float ex = a.x + vx * t - p.x, ey = a.y + vy * t - p.y;
    return sqrtf(ex * ex + ey * ey);
}

static void cui__nc_zoom_at(cui__nc_view *v, float nz, float sx, float sy) {
    cui_node_view *view = v->view;
    nz = cui__clampf(nz, v->min_zoom, v->max_zoom);
    float cx = (sx - v->ox) / v->z, cy = (sy - v->oy) / v->z;
    view->x = sx - v->r.x - cx * nz;
    view->y = sy - v->r.y - cy * nz;
    view->zoom = nz;
    v->z = nz;
    v->ox = v->r.x + view->x; v->oy = v->r.y + view->y;
}

/* the wheel aimed at scroll_id zooms around the pointer; a sideways scroll pans */
static void cui__nc_wheel(cui_ctx *ui, cui__nc_view *v, cui_id scroll_id) {
    if (!(v->has_rect && ui->scroll_target == scroll_id && (ui->scroll_dy != 0 || ui->scroll_dx != 0))) return;
    if (ui->scroll_dy != 0) cui__nc_zoom_at(v, v->z * powf(1.12f, ui->scroll_dy), ui->mx, ui->my);
    if (ui->scroll_dx != 0) { v->view->x += ui->scroll_dx * 40.0f; v->ox = v->r.x + v->view->x; }
    ui->scroll_dx = ui->scroll_dy = 0;
}

/* a pan under way follows the pointer from (*last_x, *last_y); false once the buttons are up */
static bool cui__nc_pan(cui_ctx *ui, cui__nc_view *v, float *last_x, float *last_y, cui_id owner) {
    if (!ui->mdown[CUI_MOUSE_MIDDLE] && !ui->mdown[0]) return false;
    v->view->x += ui->mx - *last_x; v->view->y += ui->my - *last_y;
    v->ox = v->r.x + v->view->x; v->oy = v->r.y + v->view->y;
    *last_x = ui->mx; *last_y = ui->my;
    ui->active = owner;
    cui__set_cursor(ui, CUI_CURSOR_MOVE);
    return true;
}

/* the box around n boxes as x0, y0, x1, y1; the empty box (1e9, -1e9) for none */
static void cui__nc_bounds(const cui__nc_box *bx, int n, float out[4]) {
    float bx0 = 1e9f, by0 = 1e9f, bx1 = -1e9f, by1 = -1e9f;
    for (int i = 0; i < n; i++) {
        const cui__nc_box *nd = &bx[i];
        bx0 = CUI_MIN(bx0, nd->x); by0 = CUI_MIN(by0, nd->y);
        bx1 = CUI_MAX(bx1, nd->x + nd->w); by1 = CUI_MAX(by1, nd->y + nd->h);
    }
    out[0] = bx0; out[1] = by0; out[2] = bx1; out[3] = by1;
}

/* zoom and pan so every box shows, with a margin, at most at 100% */
static void cui__nc_frame(cui__nc_view *v, const cui__nc_box *bx, int n) {
    cui_node_view *view = v->view;
    if (n <= 0) return;
    float bb[4];
    cui__nc_bounds(bx, n, bb);
    float bx0 = bb[0], by0 = bb[1], bx1 = bb[2], by1 = bb[3];
    float m = 40;
    float nz = CUI_MIN((v->r.w - 2 * m) / CUI_MAX(1.0f, bx1 - bx0), (v->r.h - 2 * m) / CUI_MAX(1.0f, by1 - by0));
    nz = cui__clampf(CUI_MIN(nz, 1.0f), v->min_zoom, v->max_zoom);
    view->zoom = v->z = nz;
    view->x = v->r.w * 0.5f - (bx0 + bx1) * 0.5f * nz;
    view->y = v->r.h * 0.5f - (by0 + by1) * 0.5f * nz;
    v->ox = v->r.x + view->x; v->oy = v->r.y + view->y;
}

/* the selection a marquee from (x0, y0) to (x1, y1) makes: base, then every box it touches */
static void cui__nc_marquee_select(cui_ctx *ui, cui__nc_ints *sel, const cui__nc_ints *base, const cui__nc_box *bx, int n,
                                   float x0, float y0, float x1, float y1) {
    sel->n = 0;
    for (int i = 0; i < base->n; i++) cui__nc_push(ui, sel, base->v[i]);
    for (int i = 0; i < n; i++) {
        const cui__nc_box *nd = &bx[i];
        bool hit = nd->x < x1 && nd->x + nd->w > x0 && nd->y < y1 && nd->y + nd->h > y0;
        if (hit && !cui__nc_has(sel, nd->id)) cui__nc_push(ui, sel, nd->id);
    }
}

/* dragging on the minimap centres the view on the canvas point under the pointer */
static void cui__nc_minimap_follow(cui_ctx *ui, cui__nc_view *v, const cui__nc_minimap *mm) {
    if (!(mm->valid && mm->s > 0)) return;
    float cx = mm->bx + (ui->mx - mm->x) / mm->s, cy = mm->by + (ui->my - mm->y) / mm->s;
    v->view->x = v->r.w * 0.5f - cx * v->z;
    v->view->y = v->r.h * 0.5f - cy * v->z;
    v->ox = v->r.x + v->view->x; v->oy = v->r.y + v->view->y;
}

/* hosted content builds at 100%: the pointer moves into its space, scaled around (sx, sy) */
static void cui__nc_host_begin(cui_ctx *ui, cui__nc_host *h, float sx, float sy, float z) {
    h->sx = sx; h->sy = sy;
    h->saved_pointer = cui_mouse_pos(ui);
    if (!ui->building.screen_pointer) ui->building.screen_pointer = &h->saved_pointer;
    h->saved_pmx = ui->pmx; h->saved_pmy = ui->pmy;
    ui->mx = sx + (ui->mx - sx) / z; ui->my = sy + (ui->my - sy) / z;
    ui->pmx = sx + (ui->pmx - sx) / z; ui->pmy = sy + (ui->pmy - sy) / z;
}

static void cui__nc_host_end(cui_ctx *ui, cui__nc_host *h) {
    if (ui->building.screen_pointer == &h->saved_pointer) ui->building.screen_pointer = NULL;
    ui->mx = h->saved_pointer.x; ui->my = h->saved_pointer.y; ui->pmx = h->saved_pmx; ui->pmy = h->saved_pmy;
}

/* content laid out at 100% and shown at zoom z: scaled around its centre, then
 * moved so its top left stays where the node was placed */
static void cui__nc_scale_box(cui_node *n, float w, float h, float z) {
    if (fabsf(z - 1) > 0.0001f) {
        n->scale = z;
        n->tx = w * 0.5f * (z - 1);
        n->ty = h * 0.5f * (z - 1);
    }
}

static void cui__nc_cards_add(cui_ctx *ui, cui__nc_cards *c, int idx, bool raise) {
    if (c->n >= c->cap) {
        int nc = c->cap ? c->cap * 2 : 32;
        int *nv = cui__frame_alloc(ui, sizeof(int) * (size_t)nc);
        bool *nr = cui__frame_alloc(ui, sizeof(bool) * (size_t)nc);
        if (c->n) { memcpy(nv, c->idx, sizeof(int) * (size_t)c->n); memcpy(nr, c->raise, sizeof(bool) * (size_t)c->n); }
        c->idx = nv; c->raise = nr; c->cap = nc;
    }
    c->idx[c->n] = idx;
    c->raise[c->n] = raise;
    c->n++;
}

/* Relinks the canvas' absolute children in paint order: resting cards, raised
 * cards, then the overlays (top_a, top_b; -1 for none). Later children paint
 * above and take the pointer first. */
static void cui__nc_paint_order(cui_ctx *ui, int canvas, const cui__nc_cards *cards, int top_a, int top_b) {
    cui_node *en = &ui->nodes[canvas];
    int total = 0;
    for (int c = en->abs_first; c >= 0; c = ui->nodes[c].next) total++;
    int *order = cui__frame_alloc(ui, sizeof(int) * (size_t)(total + 1));
    int on = 0;
    for (int pass = 0; pass < 3; pass++)
        for (int c = en->abs_first; c >= 0; c = ui->nodes[c].next) {
            int kind = 0;   /* 0 resting, 1 raised card, 2 overlay */
            if (c == top_a || c == top_b) kind = 2;
            else for (int k = 0; k < cards->n; k++) if (cards->idx[k] == c) { kind = cards->raise[k] ? 1 : 0; break; }
            if (kind == pass) order[on++] = c;
        }
    for (int k = 0; k < on; k++) ui->nodes[order[k]].next = k + 1 < on ? order[k + 1] : -1;
    en->abs_first = on ? order[0] : -1;
    en->abs_last = on ? order[on - 1] : -1;
}

/* the wheel goes to the canvas while the pointer is over its visible part */
static void cui__nc_paint_wheel_hit(cui_ctx *ui, cui_id scroll_id, cui_rect r) {
    cui_rect clip = ui->dl.clip_stack[ui->dl.clip_depth - 1];
    float s = ui->scale;
    cui_rect cp = { clip.x / s, clip.y / s, clip.w / s, clip.h / s };
    cui_scroll_state *ss = cui_state(ui, scroll_id, sizeof *ss);
    ss->view_w = r.w; ss->view_h = r.h;
    ss->content_w = r.w + 1e6f; ss->content_h = r.h + 1e6f;
    cui__paint_add_hit(ui, scroll_id, cui__rect_intersect(r, cp), CUI_NF_SCROLL_X | CUI_NF_SCROLL_Y);
}

/* dots on the grid, doubling their spacing when zoomed far out */
static void cui__nc_paint_grid(cui_ctx *ui, const cui__nc_view *v, cui_rect r) {
    float step = CUI__NC_GRID * v->z;
    while (step < 14.0f) step *= 2;
    cui_color dot = cui_alpha(CUI_C(ui, BORDER_STRONG), ui->mode == CUI_THEME_DARK ? 0.32f : 0.42f);
    float x0 = v->ox + ceilf((r.x - v->ox) / step) * step;
    float y0 = v->oy + ceilf((r.y - v->oy) / step) * step;
    float d = 1.1f;
    for (float y = y0; y < r.y + r.h; y += step)
        for (float x = x0; x < r.x + r.w; x += step)
            cui_draw_rect(ui, (cui_rect){ x - d * 0.5f, y - d * 0.5f, d, d }, CUI_CORNERS(d * 0.5f), dot);
}

/* the marquee between two canvas points */
static void cui__nc_paint_marquee(cui_ctx *ui, const cui__nc_view *v, float ax, float ay, float bx, float by, cui_color acc) {
    float x0 = CUI_MIN(ax, bx), y0 = CUI_MIN(ay, by);
    float x1 = CUI_MAX(ax, bx), y1 = CUI_MAX(ay, by);
    cui_vec2 p0 = cui__nc_screen(v, x0, y0), p1 = cui__nc_screen(v, x1, y1);
    cui_rect mr = { p0.x, p0.y, p1.x - p0.x, p1.y - p0.y };
    bool neutral = ui->accent == CUI_ACCENT_NEUTRAL;
    cui_draw_rect_border(ui, mr, CUI_CORNERS(2), neutral ? cui_alpha(CUI_C(ui, FOREGROUND), 0.04f) : CUI_C(ui, ACCENT_SUBTLE),
                         cui_alpha(acc, 0.55f), 1);
}

/* An overview of the boxes and the view in the canvas' bottom right corner;
 * where it went is kept in *mm for a press on it. */
static void cui__nc_paint_minimap(cui_ctx *ui, const cui__nc_view *v, cui_rect r, const cui__nc_box *bx, int n,
                                  const cui__nc_ints *sel, cui_color acc, cui__nc_minimap *mm_out) {
    float z = v->z;
    /* full size on a roomy canvas; on a narrow one (a phone) it shrinks to
     * a corner instead of covering the graph */
    float k = CUI_CLAMP(r.w * 0.28f / 168.0f, 0.5f, 1.0f);
    float mw = 168 * k, mh = 108 * k, m = 12;
    cui_rect mm = { r.x + r.w - mw - m, r.y + r.h - mh - m, mw, mh };
    if (!(mm.x > r.x + m && mm.y > r.y + m)) return;
    /* bounds of the boxes and the view, in canvas units */
    float bb[4];
    cui__nc_bounds(bx, n, bb);
    float bx0 = bb[0], by0 = bb[1], bx1 = bb[2], by1 = bb[3];
    float vx0 = (r.x - v->ox) / z, vy0 = (r.y - v->oy) / z, vx1 = vx0 + r.w / z, vy1 = vy0 + r.h / z;
    bx0 = CUI_MIN(bx0, vx0); by0 = CUI_MIN(by0, vy0); bx1 = CUI_MAX(bx1, vx1); by1 = CUI_MAX(by1, vy1);
    float pad = 8;
    float s = CUI_MIN((mw - 2 * pad) / CUI_MAX(1.0f, bx1 - bx0), (mh - 2 * pad) / CUI_MAX(1.0f, by1 - by0));
    float offx = mm.x + pad + ((mw - 2 * pad) - (bx1 - bx0) * s) * 0.5f;
    float offy = mm.y + pad + ((mh - 2 * pad) - (by1 - by0) * s) * 0.5f;
    mm_out->valid = true; mm_out->rect = mm; mm_out->s = s; mm_out->x = offx; mm_out->y = offy; mm_out->bx = bx0; mm_out->by = by0;
    cui_color bg = ui->mode == CUI_THEME_DARK ? CUI_C(ui, SURFACE) : CUI_C(ui, SURFACE);
    float rad = CUI__R_ITEM(ui);
    cui_draw_rect_border(ui, mm, CUI_CORNERS(rad), bg, CUI_C(ui, BORDER), 1);
    cui_push_clip(ui, cui__rect_inset(mm, 1));
    for (int i = 0; i < n; i++) {
        const cui__nc_box *nd = &bx[i];
        cui_rect q = { offx + (nd->x - bx0) * s, offy + (nd->y - by0) * s, CUI_MAX(2.0f, nd->w * s), CUI_MAX(2.0f, nd->h * s) };
        bool on = cui__nc_has(sel, nd->id);
        cui_draw_rect(ui, q, CUI_CORNERS(1.5f), on ? cui_alpha(acc, 0.7f) : cui_mix(CUI_C(ui, BORDER), CUI_C(ui, BORDER_STRONG), 0.35f));
    }
    cui_rect vr = { offx + (vx0 - bx0) * s, offy + (vy0 - by0) * s, (vx1 - vx0) * s, (vy1 - vy0) * s };
    cui_draw_rect_border(ui, vr, CUI_CORNERS(2), cui_alpha(CUI_C(ui, FOREGROUND), 0.025f), cui_alpha(CUI_C(ui, FOREGROUND), 0.3f), 1);
    cui_pop_clip(ui);
}

/* ------------------------------------------------------------------------ */
/* Node editor                                                              */
/* ------------------------------------------------------------------------ */

#define CUI__NE_W        184.0f   /* default node width */
#define CUI__NE_HEAD     34.0f    /* title row */
#define CUI__NE_ROW      26.0f    /* pin row */
#define CUI__NE_PAD_TOP  6.0f
#define CUI__NE_PAD_BOT  8.0f
#define CUI__NE_INSET    14.0f    /* labels and hosted content from the card edge */
#define CUI__NE_PIN_R    4.5f
#define CUI__NE_SNAP     18.0f    /* a dropped wire finds a pin this close (screen points) */

enum { NE_IDLE = 0, NE_PAN, NE_PEND_DRAG, NE_DRAG, NE_PEND_MARQUEE, NE_MARQUEE, NE_WIRE, NE_MINIMAP };
enum { NE_IN = 0, NE_OUT = 1, NE_CONTENT = 2 };

typedef struct { int pin, node, kind, type; float x, y; } cui__ne_pin;    /* x, y: center, canvas units */
typedef struct { int id, from, to; } cui__ne_link;
typedef struct { int kind, pin, type; bool connected; const char *label; int label_len; float y, h, hover; } cui__ne_row;

typedef struct {
    cui__nc_box *nodes; int node_n, node_cap;
    cui__ne_pin *pins; int pin_n, pin_cap;
    cui__ne_link *links; int link_n, link_cap;
} cui__ne_set;

typedef struct {
    cui__ne_set set[2]; int cur;          /* set[cur] fills this frame, the other is the last frame */
    cui__ne_row *rows; int row_n, row_cap;
    cui__nc_ints sel;                     /* selected node ids */
    cui__nc_ints mq_base;                 /* selection kept by a Shift marquee */
    int sel_link; bool link_selected;
    int mode;
    float press_x, press_y, last_x, last_y;   /* screen points */
    int press_node; bool press_mod, press_was_sel;
    float mq_x0, mq_y0, mq_x1, mq_y1;     /* marquee, canvas units */
    int wire_pin, wire_kind, wire_type, wire_node, wire_link, wire_orig;
    bool wire_detached, has_target; int target_pin;
    cui__nc_ints ev_new, ev_rm, ev_del; int ev_new_read, ev_rm_read, ev_del_read;
    float move_dx, move_dy;               /* canvas units, applied to the selected nodes this frame */
    bool frame_all;
    int kb_node; bool kb_has;             /* node whose card has keyboard focus */
    cui_spring lift;
    cui__nc_minimap mm;                   /* minimap mapping, from the last paint */
} cui__ne_state;

typedef struct cui__ne_build {
    cui_id id;
    uint64_t frame;
    cui__ne_state *st;
    cui__nc_view v;
    int editor_node;
    bool grid, minimap, consumed, bg_focus, disabled;
    int inert, inert_content;             /* nested or misplaced begin calls */
    /* node being built */
    bool in_node;
    int node_id, card_node, row_first, content_index;
    cui_id card_id;
    float nx, ny, nw, row_y, card_sx, card_sy;
    bool node_selected;
    struct cui__ne_card *card_paint;
    int content_node; cui__nc_host host; bool in_content;
    cui_id content_id;
    cui__nc_cards cards;
    /* snapshot for painting */
    cui__ne_pin *pins; int pin_n;
    cui__ne_link *links; int link_n;
    cui__nc_box *nodes; int node_n;
    cui__ne_row *rows;
    struct cui__ne_build *prev;
} cui__ne_build;

typedef struct cui__ne_card {
    cui__ne_build *b;
    int row_first, row_n;
    const char *title; int title_len;
    const char *icon;
    bool selected;
    float hover, lift, focus;
} cui__ne_card;

typedef struct { cui__ne_build *cur; cui_id last; uint64_t last_frame; } cui__ne_reg;

static cui__ne_reg *cui__ne_registry(cui_ctx *ui) {
    return cui_state(ui, cui__hash_str("cui.node_editor", 0x4E0Du), sizeof(cui__ne_reg));
}

static cui__ne_build *cui__ne_cur(cui_ctx *ui) {
    cui__ne_reg *reg = cui__ne_registry(ui);
    if (reg->cur && reg->cur->frame != ui->frame) reg->cur = NULL;
    return reg->cur;
}

static void cui__ne_dtor(cui_ctx *ui, void *data) {
    cui__ne_state *st = data;
    for (int k = 0; k < 2; k++) { cui__free(ui, st->set[k].nodes); cui__free(ui, st->set[k].pins); cui__free(ui, st->set[k].links); }
    cui__free(ui, st->rows);
    cui__free(ui, st->sel.v); cui__free(ui, st->mq_base.v);
    cui__free(ui, st->ev_new.v); cui__free(ui, st->ev_rm.v); cui__free(ui, st->ev_del.v);
}

static cui_id cui__ne_card_id(cui_id eid, int node) { return cui__hash(&node, sizeof node, eid ^ 0x4E0Du); }
static cui_id cui__ne_pin_id(cui_id eid, int pin) { return cui__hash(&pin, sizeof pin, eid ^ 0x9113u); }
static cui_id cui__ne_scroll_id(cui_id eid) { return eid ^ 0x5C2E11u; }
static cui_id cui__ne_minimap_id(cui_id eid) { return eid ^ 0x313A9u; }

static const cui__ne_pin *cui__ne_find_pin(const cui__ne_pin *p, int n, int pin) {
    for (int i = 0; i < n; i++) if (p[i].pin == pin) return &p[i];
    return NULL;
}
static const cui__nc_box *cui__ne_find_node(const cui__nc_box *p, int n, int id) {
    for (int i = 0; i < n; i++) if (p[i].id == id) return &p[i];
    return NULL;
}

static cui_color cui__ne_type_color(cui_ctx *ui, int type) {
    if (type <= 0) return CUI_C(ui, TEXT_MUTED);
    return ui->theme.colors[CUI_TOKEN_SERIES_1 + (type - 1) % 6];
}
static cui_color cui__ne_accent(cui_ctx *ui) { return CUI_C(ui, ACCENT); }

/* Bezier from an output (a) to an input (d): both ends leave horizontally. */
static int cui__ne_curve(cui_vec2 a, cui_vec2 d, float z, cui_vec2 *out, int max) {
    float dx = d.x - a.x, dy = d.y - a.y;
    /* tangents grow with the run, so short links stay a gentle curve, not a loop */
    float k = CUI_MAX(fabsf(dx) * 0.5f, CUI_MIN(40.0f * z, sqrtf(dx * dx + dy * dy) * 0.5f));
    if (dx < 0) k = CUI_MAX(k, CUI_MIN(fabsf(dx) * 0.5f + fabsf(dy) * 0.25f, 160.0f * z));   /* backwards: loop round */
    cui_vec2 b = { a.x + k, a.y }, c = { d.x - k, d.y };
    float len = sqrtf(dx * dx + dy * dy) + 2 * k;
    int n = (int)CUI_CLAMP(len / 5.0f, 12.0f, (float)(max - 1));
    for (int i = 0; i <= n; i++) {
        float t = (float)i / (float)n, u = 1 - t;
        float w0 = u * u * u, w1 = 3 * u * u * t, w2 = 3 * u * t * t, w3 = t * t * t;
        out[i].x = w0 * a.x + w1 * b.x + w2 * c.x + w3 * d.x;
        out[i].y = w0 * a.y + w1 * b.y + w2 * c.y + w3 * d.y;
    }
    return n + 1;
}

static bool cui__ne_compatible(const cui__ne_state *st, const cui__ne_pin *p) {
    if (p->kind == st->wire_kind || p->node == st->wire_node) return false;
    return st->wire_type <= 0 || p->type <= 0 || p->type == st->wire_type;
}

static void cui__ne_select_only(cui_ctx *ui, cui__ne_state *st, int node) {
    st->sel.n = 0;
    cui__nc_push(ui, &st->sel, node);
    st->link_selected = false;
}

/* ------------------------------------------------------------------------ */
/* Painting                                                                 */
/* ------------------------------------------------------------------------ */

static void cui__ne_draw_wire(cui_ctx *ui, cui_vec2 a, cui_vec2 d, float z, float thick, cui_color c) {
    cui_vec2 pts[96];
    int n = cui__ne_curve(a, d, z, pts, 96);
    cui_draw_polyline(ui, pts, n, thick, c, false);
}

static float cui__ne_wire_thickness(float z) { return cui__clampf(1.75f * z, 1.1f, 3.0f); }

/** Paint the canvas and expose its wheel hit target while interactive. */
static void cui__paint_ne_canvas(cui_ctx *ui, cui_node *n, void *data) {
    cui__ne_build *b = data;
    cui__ne_state *st = b->st;
    cui_rect r = n->rect;
    if (!b->disabled) cui__nc_paint_wheel_hit(ui, cui__ne_scroll_id(b->id), r);
    float z = b->v.z;
    cui_push_clip(ui, cui__rect_inset(r, 1));   /* the node's own clip starts after this paint */
    if (b->grid) cui__nc_paint_grid(ui, &b->v, r);
    /* links, under the nodes */
    float th = cui__ne_wire_thickness(z);
    for (int i = 0; i < b->link_n; i++) {
        const cui__ne_link *l = &b->links[i];
        if (st->mode == NE_WIRE && st->wire_detached && l->id == st->wire_link) continue;
        const cui__ne_pin *pa = cui__ne_find_pin(b->pins, b->pin_n, l->from);
        const cui__ne_pin *pd = cui__ne_find_pin(b->pins, b->pin_n, l->to);
        if (!pa || !pd) continue;
        bool sel = st->link_selected && st->sel_link == l->id;
        cui_color c = sel ? cui__ne_accent(ui) : cui_alpha(cui__ne_type_color(ui, pa->type), 0.85f);
        cui__ne_draw_wire(ui, cui__nc_screen(&b->v, pa->x, pa->y), cui__nc_screen(&b->v, pd->x, pd->y), z, sel ? th + 0.5f : th, c);
    }
    cui_pop_clip(ui);
}

/* Over the nodes: the wire being dragged, the marquee and the minimap. */
static void cui__paint_ne_overlay(cui_ctx *ui, cui_node *n, void *data) {
    cui__ne_build *b = data;
    cui__ne_state *st = b->st;
    float z = b->v.z;
    cui_rect r = ui->nodes[b->editor_node].rect;
    cui_color acc = cui__ne_accent(ui);
    if (st->mode == NE_WIRE) {
        const cui__ne_pin *src = cui__ne_find_pin(b->pins, b->pin_n, st->wire_pin);
        if (src) {
            cui_vec2 a = cui__nc_screen(&b->v, src->x, src->y), m = { ui->mx, ui->my };
            const cui__ne_pin *tp = st->has_target ? cui__ne_find_pin(b->pins, b->pin_n, st->target_pin) : NULL;
            if (tp) m = cui__nc_screen(&b->v, tp->x, tp->y);
            if (src->kind == NE_OUT) cui__ne_draw_wire(ui, a, m, z, cui__ne_wire_thickness(z), acc);
            else cui__ne_draw_wire(ui, m, a, z, cui__ne_wire_thickness(z), acc);
            if (!tp) cui_draw_circle(ui, m, CUI_MAX(2.5f, 3.0f * z), acc);
        }
    }
    if (st->mode == NE_MARQUEE) cui__nc_paint_marquee(ui, &b->v, st->mq_x0, st->mq_y0, st->mq_x1, st->mq_y1, acc);
    if (b->bg_focus) {
        float rad = ui->theme.radius_panel;
        cui_draw_rect_border(ui, cui__rect_inset(r, 1), CUI_CORNERS(rad - 1), cui__rgba(0, 0, 0, 0), CUI_C(ui, FOCUS_RING), 2);
    }
    st->mm.valid = false;
    if (b->minimap && b->node_n > 0) cui__nc_paint_minimap(ui, &b->v, r, b->nodes, b->node_n, &st->sel, acc, &st->mm);
    (void)n;
}

static void cui__ne_draw_pin(cui_ctx *ui, const cui__ne_build *b, const cui__ne_row *row, cui_vec2 c, cui_color card_bg) {
    const cui__ne_state *st = b->st;
    float z = b->v.z;
    float pr = CUI_MAX(2.5f, CUI__NE_PIN_R * z) * (1.0f + 0.22f * row->hover);
    cui_color tc = cui__ne_type_color(ui, row->type);
    cui_color acc = cui__ne_accent(ui);
    bool wiring = st->mode == NE_WIRE;
    const cui__ne_pin *p = cui__ne_find_pin(b->pins, b->pin_n, row->pin);
    bool compat = wiring && p && cui__ne_compatible(st, p);
    bool target = wiring && st->has_target && st->target_pin == row->pin;
    bool source = wiring && st->wire_pin == row->pin;
    if (compat && !target) {
        /* a quiet ring says "this one fits" */
        cui_draw_rect_border(ui, (cui_rect){ c.x - pr - 3, c.y - pr - 3, (pr + 3) * 2, (pr + 3) * 2 }, CUI_CORNERS(pr + 3),
                             cui__rgba(0, 0, 0, 0), cui_alpha(acc, 0.45f), 1);
    }
    if (target) {
        float tr = pr * 1.3f;
        cui_draw_circle(ui, c, tr + 2.5f, cui_alpha(acc, 0.18f));
        cui_draw_circle(ui, c, tr, acc);
        return;
    }
    bool filled = row->connected || source;
    float bw = CUI_MAX(1.0f, 1.5f * z);
    cui_rect pr_r = { c.x - pr, c.y - pr, pr * 2, pr * 2 };
    if (filled) cui_draw_rect_border(ui, pr_r, CUI_CORNERS(pr), source ? acc : tc, card_bg, CUI_MIN(bw, 1.25f));
    else cui_draw_rect_border(ui, pr_r, CUI_CORNERS(pr), card_bg, tc, bw);
}

static void cui__paint_ne_card(cui_ctx *ui, cui_node *n, void *data) {
    cui__ne_card *cp = data;
    cui__ne_build *b = cp->b;
    float z = b->v.z;
    cui_rect r = n->rect;
    float rad = ui->theme.radius_control * z;
    cui_corners rc = CUI_CORNERS(rad);
    bool dark = ui->mode == CUI_THEME_DARK;
    cui_color bg = dark ? CUI_C(ui, SURFACE_RAISED) : CUI_C(ui, SURFACE);
    cui_color acc = cui__ne_accent(ui);
    if (cp->lift > 0.01f) cui__draw_elevation(ui, r, rc, CUI_ELEVATION_FLOATING, cp->lift);
    cui_color edge = cp->selected ? acc : cui_mix(CUI_C(ui, BORDER), CUI_C(ui, BORDER_STRONG), cp->hover * 0.7f);
    cui_draw_rect_border(ui, r, rc, bg, edge, 1.0f);
    if (dark) cui__draw_top_light(ui, r, rc, 0.045f);
    /* title */
    float head = CUI__NE_HEAD * z;
    float ts = cui__fs(ui, ui->theme.text_body) * z;
    float x = r.x + 12 * z;
    if (cp->icon) {
        float is = 14 * z;
        if (is >= 4) cui_draw_icon(ui, cp->icon, (cui_rect){ x, r.y, is, head }, is, CUI_C(ui, TEXT_SECONDARY));
        x += is + 7 * z;
    }
    if (ts >= 3) {
        float fh = cui__font_line_height(ui, CUI_FONT_MEDIUM, ts);
        cui__draw_text_ellipsis(ui, CUI_FONT_MEDIUM, ts, x, r.y + (head - fh) * 0.5f, r.x + r.w - 12 * z - x,
                                CUI_C(ui, FOREGROUND), cp->title, cp->title_len, false);
    }
    float hair = 1.0f;
    cui_draw_rect(ui, (cui_rect){ r.x + 1, r.y + head, r.w - 2, hair }, CUI_CORNERS(0), CUI_C(ui, BORDER));
    /* rows */
    float ls = cui__fs(ui, ui->theme.text_sm) * z;
    float lfh = cui__font_line_height(ui, CUI_FONT_BODY, ls);
    float inset = CUI__NE_INSET * z;
    for (int k = 0; k < cp->row_n; k++) {
        const cui__ne_row *row = &b->rows[cp->row_first + k];
        if (row->kind == NE_CONTENT) continue;
        float cy = r.y + (row->y + row->h * 0.5f) * z;
        if (ls >= 3 && row->label_len > 0) {
            cui_color lc = CUI_C(ui, TEXT_SECONDARY);
            float maxw = r.w - 2 * inset;
            if (row->kind == NE_IN) {
                cui__draw_text_ellipsis(ui, CUI_FONT_BODY, ls, r.x + inset, cy - lfh * 0.5f, maxw, lc, row->label, row->label_len, false);
            } else {
                float w = CUI_MIN(maxw, cui__text_width(ui, CUI_FONT_BODY, ls, row->label, row->label_len, false));
                cui__draw_text_ellipsis(ui, CUI_FONT_BODY, ls, r.x + r.w - inset - w, cy - lfh * 0.5f, maxw, lc, row->label, row->label_len, false);
            }
        }
        cui_vec2 pc = { row->kind == NE_IN ? r.x : r.x + r.w, cy };
        cui__ne_draw_pin(ui, b, row, pc, bg);
    }
    if (cp->focus > 0.01f) cui__draw_focus_ring(ui, r, rc, cp->focus);
}

/* ------------------------------------------------------------------------ */
/* Editor                                                                   */
/* ------------------------------------------------------------------------ */

static void cui__ne_delete_selection(cui_ctx *ui, cui__ne_state *st, const cui__ne_set *prev) {
    for (int i = 0; i < st->sel.n; i++)
        if (cui__ne_find_node(prev->nodes, prev->node_n, st->sel.v[i])) cui__nc_push(ui, &st->ev_del, st->sel.v[i]);
    for (int i = 0; i < prev->link_n; i++) {
        const cui__ne_link *l = &prev->links[i];
        const cui__ne_pin *pa = cui__ne_find_pin(prev->pins, prev->pin_n, l->from);
        const cui__ne_pin *pd = cui__ne_find_pin(prev->pins, prev->pin_n, l->to);
        bool gone = (pa && cui__nc_has(&st->sel, pa->node)) || (pd && cui__nc_has(&st->sel, pd->node)) ||
                    (st->link_selected && st->sel_link == l->id);
        if (gone) cui__nc_push(ui, &st->ev_rm, l->id);
    }
    st->sel.n = 0;
    st->link_selected = false;
}

static void cui__ne_finish_wire(cui_ctx *ui, cui__ne_state *st, const cui__ne_set *prev) {
    float dx = st->last_x - st->press_x, dy = st->last_y - st->press_y;
    if (dx * dx + dy * dy < CUI__NC_DRAG * CUI__NC_DRAG) return;   /* a click on a pin changes nothing */
    const cui__ne_pin *tgt = st->has_target ? cui__ne_find_pin(prev->pins, prev->pin_n, st->target_pin) : NULL;
    if (st->wire_detached && tgt && tgt->pin == st->wire_orig) return;   /* put back where it was */
    if (st->wire_detached) cui__nc_push(ui, &st->ev_rm, st->wire_link);
    if (!tgt) return;
    int out = st->wire_kind == NE_OUT ? st->wire_pin : tgt->pin;
    int in = st->wire_kind == NE_OUT ? tgt->pin : st->wire_pin;
    bool dup = false;
    for (int i = 0; i < prev->link_n; i++) {
        const cui__ne_link *l = &prev->links[i];
        if (st->wire_detached && l->id == st->wire_link) continue;
        if (l->to == in && l->from == out) dup = true;
        else if (l->to == in && !cui__nc_has(&st->ev_rm, l->id)) cui__nc_push(ui, &st->ev_rm, l->id);   /* an input takes one link */
    }
    if (!dup) { cui__nc_push(ui, &st->ev_new, out); cui__nc_push(ui, &st->ev_new, in); }
}

/* keys while the editor or one of its cards has focus: Delete, F frames everything, Ctrl+A,
 * Escape, arrows move the selection, + - and 0 zoom */
static void cui__ne_keys(cui_ctx *ui, cui__ne_build *b, const cui__ne_set *prev) {
    cui__ne_state *st = b->st;
    bool prim = (ui->mods & CUI_MOD_PRIMARY) != 0;
    if (cui_key_pressed(ui, CUI_KEY_DELETE) || cui_key_pressed(ui, CUI_KEY_BACKSPACE)) {
        cui__ne_delete_selection(ui, st, prev);
        ui->kpressed[CUI_KEY_DELETE] = ui->kpressed[CUI_KEY_BACKSPACE] = 0;
    }
    if (cui_key_pressed(ui, CUI_KEY_F) && !prim) { st->frame_all = true; ui->kpressed[CUI_KEY_F] = 0; }
    if (prim && cui_key_pressed(ui, CUI_KEY_A)) {
        st->sel.n = 0;
        for (int i = 0; i < prev->node_n; i++) cui__nc_push(ui, &st->sel, prev->nodes[i].id);
        ui->kpressed[CUI_KEY_A] = 0;
    }
    if (cui_key_pressed(ui, CUI_KEY_ESCAPE)) {
        if (st->mode == NE_WIRE || st->mode == NE_MARQUEE) st->mode = NE_IDLE;
        else { st->sel.n = 0; st->link_selected = false; }
        ui->kpressed[CUI_KEY_ESCAPE] = 0;
    }
    if (st->sel.n > 0) {
        float step = (ui->mods & CUI_MOD_SHIFT) ? 1.0f : 10.0f;
        if (cui_key_pressed(ui, CUI_KEY_LEFT)) { st->move_dx -= step; ui->kpressed[CUI_KEY_LEFT] = 0; }
        if (cui_key_pressed(ui, CUI_KEY_RIGHT)) { st->move_dx += step; ui->kpressed[CUI_KEY_RIGHT] = 0; }
        if (cui_key_pressed(ui, CUI_KEY_UP)) { st->move_dy -= step; ui->kpressed[CUI_KEY_UP] = 0; }
        if (cui_key_pressed(ui, CUI_KEY_DOWN)) { st->move_dy += step; ui->kpressed[CUI_KEY_DOWN] = 0; }
    }
    float cx = b->v.r.x + b->v.r.w * 0.5f, cy = b->v.r.y + b->v.r.h * 0.5f;
    if (cui_key_pressed(ui, CUI_KEY_EQUAL)) { cui__nc_zoom_at(&b->v, b->v.z * 1.25f, cx, cy); ui->kpressed[CUI_KEY_EQUAL] = 0; }
    if (cui_key_pressed(ui, CUI_KEY_MINUS)) { cui__nc_zoom_at(&b->v, b->v.z / 1.25f, cx, cy); ui->kpressed[CUI_KEY_MINUS] = 0; }
    if (cui_key_pressed(ui, CUI_KEY_0)) { cui__nc_zoom_at(&b->v, 1.0f, cx, cy); ui->kpressed[CUI_KEY_0] = 0; }
}

/* dragging the selected nodes, after the press has moved far enough to be a drag */
static void cui__ne_drag_nodes(cui_ctx *ui, cui__ne_build *b) {
    cui__ne_state *st = b->st;
    float mx = ui->mx, my = ui->my;
    if (st->mode == NE_PEND_DRAG && ui->mdown[0]) {
        float dx = mx - st->press_x, dy = my - st->press_y;
        if (dx * dx + dy * dy > CUI__NC_DRAG * CUI__NC_DRAG) st->mode = NE_DRAG;
    }
    if (st->mode == NE_DRAG) {
        st->move_dx += (mx - st->last_x) / b->v.z;
        st->move_dy += (my - st->last_y) / b->v.z;
        cui__set_cursor(ui, CUI_CURSOR_MOVE);
        ui->animating = true;
    }
    st->last_x = mx; st->last_y = my;
    if (!ui->mdown[0]) {
        /* a plain click on one of several selected nodes selects just that one */
        if (st->mode == NE_PEND_DRAG && !st->press_mod && st->press_was_sel) cui__ne_select_only(ui, st, st->press_node);
        st->mode = NE_IDLE;
    }
}

/* a marquee: selects the nodes it touches, added to what was selected when it started */
static void cui__ne_marquee(cui_ctx *ui, cui__ne_build *b, const cui__ne_set *prev) {
    cui__ne_state *st = b->st;
    float mx = ui->mx, my = ui->my;
    if (st->mode == NE_PEND_MARQUEE && ui->mdown[0]) {
        float dx = mx - st->press_x, dy = my - st->press_y;
        if (dx * dx + dy * dy > CUI__NC_DRAG * CUI__NC_DRAG) st->mode = NE_MARQUEE;
    }
    st->mq_x1 = (mx - b->v.ox) / b->v.z; st->mq_y1 = (my - b->v.oy) / b->v.z;
    if (st->mode == NE_MARQUEE) {
        float x0 = CUI_MIN(st->mq_x0, st->mq_x1), y0 = CUI_MIN(st->mq_y0, st->mq_y1);
        float x1 = CUI_MAX(st->mq_x0, st->mq_x1), y1 = CUI_MAX(st->mq_y0, st->mq_y1);
        cui__nc_marquee_select(ui, &st->sel, &st->mq_base, prev->nodes, prev->node_n, x0, y0, x1, y1);
        ui->animating = true;
    }
    if (!ui->mdown[0]) st->mode = NE_IDLE;
}

/* a wire being pulled from a pin: it snaps to the nearest compatible pin; the release
 * connects (or disconnects) */
static void cui__ne_wire(cui_ctx *ui, cui__ne_build *b, const cui__ne_set *prev) {
    cui__ne_state *st = b->st;
    float mx = ui->mx, my = ui->my;
    st->last_x = mx; st->last_y = my;
    st->has_target = false;
    float best = CUI__NE_SNAP;
    for (int i = 0; i < prev->pin_n; i++) {
        const cui__ne_pin *p = &prev->pins[i];
        if (!cui__ne_compatible(st, p)) continue;
        cui_vec2 s = cui__nc_screen(&b->v, p->x, p->y);
        float dd = hypotf(s.x - mx, s.y - my);
        if (dd < best) { best = dd; st->has_target = true; st->target_pin = p->pin; }
    }
    ui->animating = true;
    if (!ui->mdown[0]) {
        cui__ne_finish_wire(ui, st, prev);
        st->mode = NE_IDLE;
        st->has_target = false;
    }
}

/* the gesture in progress: pan, drag nodes, marquee, wire, or the minimap */
static void cui__ne_gesture(cui_ctx *ui, cui__ne_build *b, const cui__ne_set *prev, cui_id mmid) {
    cui__ne_state *st = b->st;
    switch (st->mode) {
    case NE_PAN:
        if (!cui__nc_pan(ui, &b->v, &st->last_x, &st->last_y, b->id)) st->mode = NE_IDLE;
        break;
    case NE_PEND_DRAG:
    case NE_DRAG:
        cui__ne_drag_nodes(ui, b);
        break;
    case NE_PEND_MARQUEE:
    case NE_MARQUEE:
        cui__ne_marquee(ui, b, prev);
        break;
    case NE_WIRE:
        cui__ne_wire(ui, b, prev);
        break;
    case NE_MINIMAP:
        if (!ui->mdown[0]) { st->mode = NE_IDLE; break; }
        cui__nc_minimap_follow(ui, &b->v, &st->mm);
        ui->active = mmid;
        break;
    default: break;
    }
}

/* the link within a few points of (mx, my), as an index into prev->links, or -1 */
static int cui__ne_link_at(const cui__ne_build *b, const cui__ne_set *prev, float mx, float my) {
    int hit_link = -1;
    float best = 5.0f;
    for (int i = 0; i < prev->link_n; i++) {
        const cui__ne_pin *pa = cui__ne_find_pin(prev->pins, prev->pin_n, prev->links[i].from);
        const cui__ne_pin *pd = cui__ne_find_pin(prev->pins, prev->pin_n, prev->links[i].to);
        if (!pa || !pd) continue;
        cui_vec2 pts[96];
        int n = cui__ne_curve(cui__nc_screen(&b->v, pa->x, pa->y), cui__nc_screen(&b->v, pd->x, pd->y), b->v.z, pts, 96);
        for (int k = 0; k + 1 < n; k++) {
            float dd = cui__nc_seg_dist((cui_vec2){ mx, my }, pts[k], pts[k + 1]);
            if (dd < best) { best = dd; hit_link = i; }
        }
    }
    return hit_link;
}

/* a press on the empty canvas: a wire under it is selected, otherwise a marquee starts
 * (Shift or Ctrl adds to the selection) */
static void cui__ne_press_canvas(cui_ctx *ui, cui__ne_build *b, const cui__ne_set *prev) {
    cui__ne_state *st = b->st;
    float mx = ui->mx, my = ui->my;
    b->consumed = true;
    bool add = (ui->mods & (CUI_MOD_SHIFT | CUI_MOD_PRIMARY)) != 0;
    int hit_link = cui__ne_link_at(b, prev, mx, my);
    if (hit_link >= 0) {
        if (!add) st->sel.n = 0;
        st->link_selected = true;
        st->sel_link = prev->links[hit_link].id;
    } else {
        st->link_selected = false;
        st->mq_base.n = 0;
        if (add) for (int i = 0; i < st->sel.n; i++) cui__nc_push(ui, &st->mq_base, st->sel.v[i]);
        else st->sel.n = 0;
        st->mode = NE_PEND_MARQUEE;
        st->press_x = mx; st->press_y = my;
        st->mq_x0 = st->mq_x1 = (mx - b->v.ox) / b->v.z;
        st->mq_y0 = st->mq_y1 = (my - b->v.oy) / b->v.z;
    }
}

/* the canvas node: the grid, wires and minimap are painted from b */
static void cui__ne_build_canvas(cui_ctx *ui, cui__ne_build *b, const cui_node_editor_opts *o, bool focus_visible, bool hot_mine) {
    int en = cui__node_new(ui, b->id);
    cui_node *n = &ui->nodes[en];
    bool dark = ui->mode == CUI_THEME_DARK;
    n->w = o->width.kind == CUI_SIZE_FIT && o->width.value == 0 ? CUI_FILL : o->width;
    n->h = o->height.kind == CUI_SIZE_FIT && o->height.value == 0 ? CUI_PX(440) : o->height;
    n->bg = dark ? cui_mix(CUI_C(ui, BACKGROUND), CUI_C(ui, SURFACE), 0.45f) : CUI_C(ui, SURFACE_MUTED);
    n->border = 1;
    n->border_color = CUI_C(ui, BORDER);
    n->radius = CUI_CORNERS(ui->theme.radius_panel);
    n->flags |= CUI_NF_CLIP;
    n->pad[0] = n->pad[1] = n->pad[2] = n->pad[3] = 12;
    n->gap = 8;
    n->align = CUI_ALIGN_START;
    n->paint = cui__paint_ne_canvas;
    n->paint_data = b;
    cui__mark_interactive(ui, en, CUI_IF_FOCUSABLE);
    b->bg_focus = focus_visible;
    b->editor_node = en;
    if (hot_mine && b->st->mode == NE_IDLE && cui_key_action_down(ui, CUI_KEY_SPACE)) cui__set_cursor(ui, CUI_CURSOR_MOVE);
    cui__open(ui, en);
}

/** Build the editor canvas and process gestures while it is enabled. */
bool cui_node_editor_begin_ex(cui_ctx *ui, const char *id_str, cui_node_view *view, const cui_node_editor_opts *o) {
    cui_node_editor_opts d;
    memset(&d, 0, sizeof d);
    if (!o) o = &d;
    if (!view || cui__ne_cur(ui)) return false;   /* editors do not nest */
    cui__ne_reg *reg = cui__ne_registry(ui);
    cui__ne_build *b = cui__frame_alloc(ui, sizeof *b);
    memset(b, 0, sizeof *b);
    b->id = cui_make_id(ui, id_str ? id_str : "node-editor");
    b->frame = ui->frame;
    b->v.view = view;
    b->disabled = cui_is_disabled(ui);
    b->v.min_zoom = o->min_zoom > 0 ? o->min_zoom : 0.25f;
    b->v.max_zoom = o->max_zoom > b->v.min_zoom ? o->max_zoom : CUI_MAX(2.0f, b->v.min_zoom);
    b->grid = !o->no_grid;
    b->minimap = o->minimap;
    cui__ne_state *st = cui_state(ui, b->id, sizeof *st);
    cui__state_set_dtor(ui, b->id, cui__ne_dtor);
    b->st = st;
    /* swap the frame sets: the other one now holds last frame */
    st->cur ^= 1;
    cui__ne_set *cur = &st->set[st->cur], *prev = &st->set[st->cur ^ 1];
    cur->node_n = cur->pin_n = cur->link_n = 0;
    st->row_n = 0;
    st->ev_new.n = st->ev_rm.n = st->ev_del.n = 0;
    st->ev_new_read = st->ev_rm_read = st->ev_del_read = 0;
    st->move_dx = st->move_dy = 0;
    if (view->zoom <= 0) view->zoom = 1;
    view->zoom = cui__clampf(view->zoom, b->v.min_zoom, b->v.max_zoom);
    b->v.has_rect = cui__has_prev_rect(ui, b->id);
    b->v.r = cui__prev_rect(ui, b->id);
    b->v.z = view->zoom;
    b->v.ox = b->v.r.x + view->x; b->v.oy = b->v.r.y + view->y;

    /* is the pointer, or the focus, on this editor or one of its cards and pins? */
    cui_interact bi = cui__interact(ui, b->id, CUI_IF_FOCUSABLE | CUI_IF_DRAG);
    cui_id mmid = cui__ne_minimap_id(b->id);
    cui_interact mi = cui__interact(ui, mmid, CUI_IF_DRAG | CUI_IF_KEEP_FOCUS);
    bool hot_mine = ui->hot == b->id || ui->hot == mmid;
    bool focus_mine = ui->focus == b->id;
    for (int i = 0; i < prev->node_n; i++) {
        cui_id cid = cui__ne_card_id(b->id, prev->nodes[i].id);
        if (ui->hot == cid) hot_mine = true;
        if (ui->focus == cid) focus_mine = true;
    }
    for (int i = 0; i < prev->pin_n && !hot_mine; i++) if (ui->hot == cui__ne_pin_id(b->id, prev->pins[i].pin)) hot_mine = true;
    float mx = ui->mx, my = ui->my;

    /* input */
    if (b->disabled) {
        st->mode = NE_IDLE;
        st->has_target = false;
    } else {
        cui__nc_wheel(ui, &b->v, cui__ne_scroll_id(b->id));
        if (focus_mine && !ui->edit_prev) cui__ne_keys(ui, b, prev);
    }
    if (st->frame_all && b->v.has_rect) {
        st->frame_all = false;
        cui__nc_frame(&b->v, prev->nodes, prev->node_n);
    }
    /* pan: the middle button, or Space and drag, anywhere over the canvas */
    bool over = b->v.has_rect && cui__rect_contains(b->v.r, mx, my) && ui->hot_layer == ui->current_layer && (hot_mine || ui->hot != 0);
    if (!b->disabled && over && st->mode == NE_IDLE && !ui->edit_prev &&
        (ui->mpressed[CUI_MOUSE_MIDDLE] || (ui->mpressed[0] && cui_key_action_down(ui, CUI_KEY_SPACE)))) {
        st->mode = NE_PAN; st->last_x = mx; st->last_y = my;
        b->consumed = true;
    }
    if (!b->disabled) cui__ne_gesture(ui, b, prev, mmid);
    if (mi.hovered && ui->mpressed[0] && !b->consumed && st->mode == NE_IDLE) {
        st->mode = NE_MINIMAP;
        b->consumed = true;
    }
    if (bi.hovered && ui->mpressed[0] && !b->consumed && st->mode == NE_IDLE) cui__ne_press_canvas(ui, b, prev);
    if (st->mode != NE_IDLE && st->mode != NE_PAN && st->mode != NE_MINIMAP && ui->mdown[0] && ui->active == 0) ui->active = b->id;
    cui__spring(ui, &st->lift, st->mode == NE_DRAG ? 1.0f : 0.0f, CUI_SPRING_SNAPPY);

    /* build */
    cui__ne_build_canvas(ui, b, o, bi.focus_visible, hot_mine && !b->disabled);
    cui_push_id(ui, id_str ? id_str : "node-editor");
    b->prev = reg->cur;
    reg->cur = b;
    return true;
}

void cui_node_editor_end(cui_ctx *ui) {
    cui__ne_build *b = cui__ne_cur(ui);
    if (!b) return;
    cui__ne_state *st = b->st;
    if (b->in_node) cui_node_end(ui);
    cui_pop_id(ui);
    /* the wire, marquee and minimap paint over the nodes */
    int ov = cui__absolute(ui, 0, 0, 0, 0, 0);
    ui->nodes[ov].paint = cui__paint_ne_overlay;
    ui->nodes[ov].paint_data = b;
    int mm = -1;
    if (b->minimap && st->mm.valid) {
        mm = cui__absolute(ui, cui__ne_minimap_id(b->id), st->mm.rect.x - b->v.r.x, st->mm.rect.y - b->v.r.y, st->mm.rect.w, st->mm.rect.h);
        cui__mark_interactive(ui, mm, 0);
    }
    /* selected nodes paint (and take the pointer) above the rest */
    cui__nc_paint_order(ui, b->editor_node, &b->cards, ov, mm);
    cui__close(ui);   /* the canvas */
    /* forget selected nodes that are gone */
    cui__ne_set *cur = &st->set[st->cur];
    for (int i = st->sel.n - 1; i >= 0; i--)
        if (!cui__ne_find_node(cur->nodes, cur->node_n, st->sel.v[i])) st->sel.v[i] = st->sel.v[--st->sel.n];
    if (st->link_selected) {
        bool found = false;
        for (int i = 0; i < cur->link_n; i++) if (cur->links[i].id == st->sel_link) found = true;
        if (!found) st->link_selected = false;
    }
    if (st->mode == NE_WIRE && !cui__ne_find_pin(cur->pins, cur->pin_n, st->wire_pin)) st->mode = NE_IDLE;
    b->pins = cur->pins; b->pin_n = cur->pin_n;
    b->links = cur->links; b->link_n = cur->link_n;
    b->nodes = cur->nodes; b->node_n = cur->node_n;
    b->rows = st->rows;
    cui__ne_reg *reg = cui__ne_registry(ui);
    reg->cur = b->prev;
    reg->last = b->id;
    reg->last_frame = ui->frame;
    ui->last_id = b->id;
    ui->last_node = b->editor_node;
}

/* ------------------------------------------------------------------------ */
/* Nodes                                                                    */
/* ------------------------------------------------------------------------ */

bool cui_node_begin_ex(cui_ctx *ui, int node_id, float *x, float *y, const char *title, const cui_node_opts *o) {
    cui_node_opts d;
    memset(&d, 0, sizeof d);
    if (!o) o = &d;
    cui__ne_build *b = cui__ne_cur(ui);
    if (!b || b->in_node || !x || !y) {
        /* outside an editor, nested or without a position: an inert box keeps the scope balanced */
        cui__open(ui, cui__node_new(ui, 0));
        cui_push_id_int(ui, node_id);
        if (b) b->inert++;
        return false;
    }
    cui__ne_state *st = b->st;
    float z = b->v.z;
    bool selected = cui__nc_has(&st->sel, node_id);
    if (selected && (st->move_dx != 0 || st->move_dy != 0)) { *x += st->move_dx; *y += st->move_dy; }
    cui_id cid = cui__ne_card_id(b->id, node_id);
    cui_interact r = cui__interact(ui, cid, CUI_IF_FOCUSABLE | CUI_IF_DRAG);
    if (r.hovered && ui->mpressed[0] && !b->consumed && st->mode == NE_IDLE) {
        b->consumed = true;
        bool toggle = (ui->mods & CUI_MOD_PRIMARY) != 0, add = (ui->mods & CUI_MOD_SHIFT) != 0;
        st->press_was_sel = selected;
        st->press_mod = toggle || add;
        if (toggle && selected) cui__nc_remove(&st->sel, node_id);
        else if ((toggle || add) && !selected) cui__nc_push(ui, &st->sel, node_id);
        else if (!selected) cui__ne_select_only(ui, st, node_id);
        st->link_selected = false;
        selected = cui__nc_has(&st->sel, node_id);
        if (selected) {
            st->mode = NE_PEND_DRAG;
            st->press_node = node_id;
            st->press_x = st->last_x = ui->mx; st->press_y = st->last_y = ui->my;
        }
    }
    /* keyboard focus arriving on a card selects it */
    if (r.focus_visible && (!st->kb_has || st->kb_node != node_id)) {
        if (!selected) { cui__ne_select_only(ui, st, node_id); selected = true; }
    }
    if (r.focused) { st->kb_has = true; st->kb_node = node_id; }
    else if (st->kb_has && st->kb_node == node_id) st->kb_has = false;
    float *hover = cui_state(ui, cid ^ 0x51u, sizeof(float));
    cui__tween(ui, hover, r.hovered && st->mode == NE_IDLE ? 1.0f : 0.0f, CUI__T_HOVER);
    float *focus = cui_state(ui, cid ^ 0x52u, sizeof(float));
    cui__tween(ui, focus, r.focus_visible ? 1.0f : 0.0f, CUI__T_FOCUS);
    if (r.hovered && st->mode == NE_IDLE) cui__set_cursor(ui, CUI_CURSOR_HAND);

    float w = o->width > 0 ? o->width : CUI__NE_W;
    b->in_node = true;
    b->node_id = node_id;
    b->nx = *x; b->ny = *y; b->nw = w;
    b->card_id = cid;
    b->node_selected = selected;
    b->row_y = CUI__NE_HEAD + CUI__NE_PAD_TOP;
    b->row_first = st->row_n;
    b->content_index = 0;
    cui_vec2 s = cui__nc_screen(&b->v, *x, *y);
    b->card_sx = s.x; b->card_sy = s.y;
    int cn = cui__absolute(ui, cid, s.x - b->v.r.x, s.y - b->v.r.y, w * z, 0);
    b->card_node = cn;
    cui__ne_card *cp = cui__frame_alloc(ui, sizeof *cp);
    memset(cp, 0, sizeof *cp);
    cp->b = b;
    cp->title = title ? title : "";
    cp->title_len = title ? cui__display_len(title) : 0;
    cp->icon = o->icon;
    cp->selected = selected;
    cp->hover = *hover;
    cp->focus = *focus;
    cp->lift = (selected && st->lift.x > 0.01f) ? st->lift.x : 0;
    cp->row_first = st->row_n;
    b->card_paint = cp;
    ui->nodes[cn].paint = cui__paint_ne_card;
    ui->nodes[cn].paint_data = cp;
    ui->nodes[cn].radius = CUI_CORNERS(ui->theme.radius_control * z);
    cui__mark_interactive(ui, cn, CUI_IF_FOCUSABLE);
    cui__nc_cards_add(ui, &b->cards, cn, selected);
    cui__open(ui, cn);
    cui_push_id_int(ui, node_id);
    ui->last_id = cid;
    return selected;
}

void cui_node_end(cui_ctx *ui) {
    cui__ne_build *b = cui__ne_cur(ui);
    if (!b || b->inert > 0 || !b->in_node) {
        if (b && b->inert > 0) b->inert--;
        cui_pop_id(ui);
        cui__close(ui);
        return;
    }
    if (b->in_content) cui_node_content_end(ui);
    cui_pop_id(ui);
    cui__ne_state *st = b->st;
    float h = b->row_y + CUI__NE_PAD_BOT;
    if (st->row_n == b->row_first) h = CUI__NE_HEAD + 2;
    ui->nodes[b->card_node].h = CUI_PX(h * b->v.z);
    b->card_paint->row_n = st->row_n - b->row_first;
    cui__ne_set *cur = &st->set[st->cur];
    CUI__GROW(ui, cur->nodes, cur->node_n, cur->node_cap, 1);
    cui__nc_box *nd = &cur->nodes[cur->node_n++];
    nd->id = b->node_id; nd->x = b->nx; nd->y = b->ny; nd->w = b->nw; nd->h = h;
    cui__close(ui);
    b->in_node = false;
    ui->last_node = b->card_node;   /* the card is the previous component, for a selection ring */
    ui->last_id = b->card_id;
}

static bool cui__ne_pin_row(cui_ctx *ui, int pin, const char *label, const cui_pin_opts *o, int kind) {
    cui_pin_opts d;
    memset(&d, 0, sizeof d);
    if (!o) o = &d;
    cui__ne_build *b = cui__ne_cur(ui);
    if (!b || !b->in_node || b->in_content || b->inert) return false;
    cui__ne_state *st = b->st;
    cui__ne_set *cur = &st->set[st->cur], *prev = &st->set[st->cur ^ 1];
    float z = b->v.z;
    bool connected = false;
    for (int i = 0; i < prev->link_n; i++) {
        const cui__ne_link *l = &prev->links[i];
        if ((kind == NE_IN ? l->to : l->from) == pin && !(st->mode == NE_WIRE && st->wire_detached && l->id == st->wire_link)) connected = true;
    }
    float cy = b->row_y + CUI__NE_ROW * 0.5f;
    float cx = kind == NE_IN ? 0 : b->nw;
    /* the pin's hit area: a little larger than the dot, never tiny */
    cui_id pid = cui__ne_pin_id(b->id, pin);
    cui_interact r = cui__interact(ui, pid, CUI_IF_DRAG | CUI_IF_KEEP_FOCUS);
    float hs = CUI_MAX(14.0f, 18.0f * z);
    int hn = cui__absolute(ui, pid, cx * z - hs * 0.5f, cy * z - hs * 0.5f, hs, hs);
    cui__mark_interactive(ui, hn, 0);
    if (r.hovered && ui->mpressed[0] && !b->consumed && st->mode == NE_IDLE) {
        b->consumed = true;
        st->wire_detached = false;
        st->wire_orig = pin;
        st->wire_pin = pin; st->wire_kind = kind; st->wire_type = o->type; st->wire_node = b->node_id;
        if (kind == NE_IN) {
            /* grabbing a connected input lifts the link off it */
            for (int i = 0; i < prev->link_n; i++) {
                const cui__ne_link *l = &prev->links[i];
                if (l->to != pin) continue;
                const cui__ne_pin *src = cui__ne_find_pin(prev->pins, prev->pin_n, l->from);
                if (!src) continue;
                st->wire_detached = true;
                st->wire_link = l->id;
                st->wire_pin = src->pin; st->wire_kind = NE_OUT; st->wire_type = src->type; st->wire_node = src->node;
                break;
            }
        }
        st->mode = NE_WIRE;
        st->has_target = false;
        st->press_x = st->last_x = ui->mx; st->press_y = st->last_y = ui->my;
    }
    float *hover = cui_state(ui, pid ^ 1u, sizeof(float));
    bool lit = (r.hovered && st->mode == NE_IDLE) || (st->mode == NE_WIRE && st->has_target && st->target_pin == pin);
    cui__tween(ui, hover, lit ? 1.0f : 0.0f, CUI__T_HOVER);
    if (r.hovered && st->mode == NE_IDLE) cui__set_cursor(ui, CUI_CURSOR_HAND);
    CUI__GROW(ui, cur->pins, cur->pin_n, cur->pin_cap, 1);
    cui__ne_pin *p = &cur->pins[cur->pin_n++];
    p->pin = pin; p->node = b->node_id; p->kind = kind; p->type = o->type;
    p->x = b->nx + cx; p->y = b->ny + cy;
    CUI__GROW(ui, st->rows, st->row_n, st->row_cap, 1);
    cui__ne_row *row = &st->rows[st->row_n++];
    memset(row, 0, sizeof *row);
    row->kind = kind; row->pin = pin; row->type = o->type; row->connected = connected;
    row->label = label ? label : ""; row->label_len = label ? cui__display_len(label) : 0;
    row->y = b->row_y; row->h = CUI__NE_ROW; row->hover = *hover;
    b->row_y += CUI__NE_ROW;
    ui->last_id = pid;
    return connected;
}

bool cui_node_input_ex(cui_ctx *ui, int pin, const char *label, const cui_pin_opts *o) { return cui__ne_pin_row(ui, pin, label, o, NE_IN); }
bool cui_node_output_ex(cui_ctx *ui, int pin, const char *label, const cui_pin_opts *o) { return cui__ne_pin_row(ui, pin, label, o, NE_OUT); }

void cui_node_content_begin(cui_ctx *ui) {
    cui__ne_build *b = cui__ne_cur(ui);
    if (!b || !b->in_node || b->in_content || b->inert) { cui__open(ui, cui__node_new(ui, 0)); if (b) b->inert_content++; return; }
    float z = b->v.z;
    int k = b->content_index++;
    cui_id id = cui__hash(&k, sizeof k, b->card_id ^ 0xC0E7u);
    float inner = b->nw - 2 * CUI__NE_INSET;
    float fx = CUI__NE_INSET * z, fy = (b->row_y + 3) * z;
    float ph = cui__has_prev_rect(ui, id) ? cui__prev_rect(ui, id).h : 30;
    int cn = cui__absolute(ui, id, fx, fy, inner, 0);
    cui_node *n = &ui->nodes[cn];
    n->h = CUI_FIT;
    n->gap = 6;
    cui__nc_scale_box(n, inner, ph, z);   /* its top left stays at (fx, fy) */
    b->content_node = cn;
    b->content_id = id;
    /* components inside see the pointer in their own (unscaled) space */
    cui__nc_host_begin(ui, &b->host, b->card_sx + fx, b->card_sy + fy, z);
    b->in_content = true;
    cui__open(ui, cn);
    cui_push_id_int(ui, 0x7000 + k);
}

void cui_node_content_end(cui_ctx *ui) {
    cui__ne_build *b = cui__ne_cur(ui);
    if (!b || b->inert_content > 0 || !b->in_content) { if (b && b->inert_content > 0) b->inert_content--; cui__close(ui); return; }
    cui_pop_id(ui);
    cui__close(ui);
    cui__nc_host_end(ui, &b->host);
    b->in_content = false;
    cui__ne_state *st = b->st;
    float ph = cui__has_prev_rect(ui, b->content_id) ? cui__prev_rect(ui, b->content_id).h : 30;
    CUI__GROW(ui, st->rows, st->row_n, st->row_cap, 1);
    cui__ne_row *row = &st->rows[st->row_n++];
    memset(row, 0, sizeof *row);
    row->kind = NE_CONTENT;
    row->y = b->row_y; row->h = ph + 6;
    b->row_y += ph + 6;
}

/* ------------------------------------------------------------------------ */
/* Links and events                                                         */
/* ------------------------------------------------------------------------ */

bool cui_node_link(cui_ctx *ui, int link_id, int from_pin, int to_pin) {
    cui__ne_build *b = cui__ne_cur(ui);
    if (!b) return false;
    cui__ne_state *st = b->st;
    cui__ne_set *cur = &st->set[st->cur];
    CUI__GROW(ui, cur->links, cur->link_n, cur->link_cap, 1);
    cui__ne_link *l = &cur->links[cur->link_n++];
    l->id = link_id; l->from = from_pin; l->to = to_pin;
    return st->link_selected && st->sel_link == link_id;
}

static cui__ne_state *cui__ne_events(cui_ctx *ui) {
    cui__ne_build *b = cui__ne_cur(ui);
    if (b) return b->st;
    cui__ne_reg *reg = cui__ne_registry(ui);
    if (reg->last && reg->last_frame == ui->frame) return cui_state(ui, reg->last, sizeof(cui__ne_state));
    return NULL;
}

bool cui_node_link_created(cui_ctx *ui, int *from_pin, int *to_pin) {
    cui__ne_state *st = cui__ne_events(ui);
    if (!st || st->ev_new_read + 2 > st->ev_new.n) return false;
    if (from_pin) *from_pin = st->ev_new.v[st->ev_new_read];
    if (to_pin) *to_pin = st->ev_new.v[st->ev_new_read + 1];
    st->ev_new_read += 2;
    return true;
}

bool cui_node_link_removed(cui_ctx *ui, int *link_id) {
    cui__ne_state *st = cui__ne_events(ui);
    if (!st || st->ev_rm_read >= st->ev_rm.n) return false;
    if (link_id) *link_id = st->ev_rm.v[st->ev_rm_read];
    st->ev_rm_read++;
    return true;
}

bool cui_node_deleted(cui_ctx *ui, int *node_id) {
    cui__ne_state *st = cui__ne_events(ui);
    if (!st || st->ev_del_read >= st->ev_del.n) return false;
    if (node_id) *node_id = st->ev_del.v[st->ev_del_read];
    st->ev_del_read++;
    return true;
}
