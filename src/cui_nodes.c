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

/* ------------------------------------------------------------------------ */
/* Board                                                                    */
/* ------------------------------------------------------------------------ */

/* Boxes placed freely on the node canvas, holding any content, joined by
 * lines from box to box. The caller owns the rectangles and the links; the
 * board moves and resizes the rectangles it is given and reports deletions. */

#define CUI__BD_W         160.0f   /* an item's width when the caller gives none */
#define CUI__BD_PAD       12.0f
#define CUI__BD_MIN_W     64.0f
#define CUI__BD_MIN_H     32.0f
#define CUI__BD_FIT_H     20.0f    /* content height assumed before it was first measured */
#define CUI__BD_GRAB_IN   6.0f     /* resize zone inside an item's edge, screen points */
#define CUI__BD_GRAB_OUT  3.0f     /* and outside it */
#define CUI__BD_LINE_PTS  64

enum { BD_IDLE = 0, BD_PAN, BD_PEND_DRAG, BD_DRAG, BD_PEND_MARQUEE, BD_MARQUEE, BD_RESIZE, BD_MINIMAP };

typedef struct { int id, from, to; cui_board_path path; cui_board_dash dash; cui_board_cap start_cap, end_cap; float width; cui_color color; } cui__bd_link;

typedef struct {
    cui__nc_box *items; int item_n, item_cap;
    cui__nc_ints locked;                  /* ids of items that cannot move, resize or be deleted */
    cui__bd_link *links; int link_n, link_cap;
} cui__bd_set;

typedef struct {
    cui__bd_set set[2]; int cur;          /* set[cur] fills this frame, the other is the last frame */
    cui__nc_ints sel;                     /* selected item ids */
    cui__nc_ints mq_base;                 /* selection kept by an adding marquee */
    int sel_link; bool link_selected;
    int mode;
    float press_x, press_y, last_x, last_y;   /* screen points */
    int press_item; bool press_mod, press_was_sel;
    float mq_x0, mq_y0, mq_x1, mq_y1;     /* marquee, canvas units */
    /* resizing: the item, the edges held, its box at the press and its rectangle now
     * (h 0 when it follows its content and only a side edge is held) */
    int rs_item, rs_edges; cui_rect rs_from, rs_rect; float rs_min_w, rs_min_h; bool rs_fit, rs_apply;
    cui__nc_ints ev_del, ev_rm; int ev_del_read, ev_rm_read;
    /* Delete is resolved at cui_board_end against that frame's items, locks and lines, and its
     * events wait there for the next frame */
    bool del_request; cui__nc_ints pend_del, pend_rm;
    float move_dx, move_dy;               /* canvas units, applied to the selected items this frame */
    bool frame_all, snap_moved;
    int kb_item; bool kb_has;             /* item whose card has keyboard focus */
    cui_spring lift;
    cui__nc_minimap mm;
} cui__bd_state;

typedef struct cui__bd_build {
    cui_id id;
    cui__bd_state *st;
    cui__nc_view v;
    int canvas;
    bool grid, minimap, consumed, bg_focus, disabled, read_only, drag_selects;
    float snap;
    /* the item being built */
    bool in_item;
    int card;
    cui_id card_id;
    cui__nc_host host;
    cui__nc_cards cards;
    /* snapshot for painting, from cui_board_end */
    cui__nc_box *items; int item_n;
    cui__bd_link *links; int link_n;
} cui__bd_build;

typedef struct {
    cui_color bg, edge;
    float bw, rad, hover, focus, lift;
    bool selected;
} cui__bd_card;

static cui__bd_build *cui__bd_cur(cui_ctx *ui) { return (cui__bd_build *)ui->building.board; }

static void cui__bd_dtor(cui_ctx *ui, void *data) {
    cui__bd_state *st = data;
    for (int k = 0; k < 2; k++) { cui__free(ui, st->set[k].items); cui__free(ui, st->set[k].links); cui__free(ui, st->set[k].locked.v); }
    cui__free(ui, st->sel.v); cui__free(ui, st->mq_base.v);
    cui__free(ui, st->ev_del.v); cui__free(ui, st->ev_rm.v);
    cui__free(ui, st->pend_del.v); cui__free(ui, st->pend_rm.v);
}

static cui_id cui__bd_card_id(cui_id bid, int item) { return cui__hash(&item, sizeof item, bid ^ 0xB0A2Du); }
static cui_id cui__bd_scroll_id(cui_id bid) { return bid ^ 0xB5C2E1u; }
static cui_id cui__bd_minimap_id(cui_id bid) { return bid ^ 0xB313A9u; }

static const cui__nc_box *cui__bd_find(const cui__nc_box *p, int n, int id) {
    for (int i = 0; i < n; i++) if (p[i].id == id) return &p[i];
    return NULL;
}

static float cui__bd_snapf(float v, float step) { return step > 0 ? roundf(v / step) * step : v; }
/* where a dropped edge lands: the nearest grid point, or the next one that keeps the minimum size
 * (low: a left or top edge, at most `most`; high: a right or bottom edge, at least `least`) */
static float cui__bd_snap_low(float v, float step, float most) {
    float s = cui__bd_snapf(v, step);
    return s <= most ? s : floorf(most / step) * step;
}
static float cui__bd_snap_high(float v, float step, float least) {
    float s = cui__bd_snapf(v, step);
    return s >= least ? s : ceilf(least / step) * step;
}

/* The view as painted: this frame's canvas rectangle, panned as it was built. The cards are placed
 * by their offset in the canvas, so when the board moved since the last frame the lines, the grid
 * and the marquee follow them this way rather than last frame's rectangle. */
static cui__nc_view cui__bd_paint_view(const cui__nc_view *v, cui_rect r) {
    cui__nc_view p = *v;
    p.ox = r.x + (v->ox - v->r.x); p.oy = r.y + (v->oy - v->r.y);
    p.r = r;
    return p;
}

/* where the ray from the centre c along (dx, dy) leaves the rectangle r */
static cui_vec2 cui__bd_exit(cui_rect r, cui_vec2 c, float dx, float dy) {
    float tx = dx != 0 ? r.w * 0.5f / fabsf(dx) : 1e9f, ty = dy != 0 ? r.h * 0.5f / fabsf(dy) : 1e9f;
    float t = CUI_MIN(tx, ty);
    cui_vec2 p = { c.x + dx * t, c.y + dy * t };
    return p;
}

/* a rounded corner at c between the run from `from` and the run to `to`, appended to out */
static int cui__bd_corner(cui_vec2 *out, int n, int cap, cui_vec2 from, cui_vec2 c, cui_vec2 to, float r) {
    float ix = c.x - from.x, iy = c.y - from.y, ox = to.x - c.x, oy = to.y - c.y;
    float il = hypotf(ix, iy), ol = hypotf(ox, oy);
    if (il < 0.01f || ol < 0.01f || r < 0.5f) { if (n < cap) out[n++] = c; return n; }
    r = CUI_MIN(r, CUI_MIN(il, ol));
    cui_vec2 a = { c.x - ix / il * r, c.y - iy / il * r }, b = { c.x + ox / ol * r, c.y + oy / ol * r };
    for (int i = 0; i <= 4 && n < cap; i++) {
        float u = (float)i / 4.0f, w = 1 - u;
        out[n].x = w * w * a.x + 2 * w * u * c.x + u * u * b.x;
        out[n].y = w * w * a.y + 2 * w * u * c.y + u * u * b.y;
        n++;
    }
    return n;
}

/* The screen points of a line from box a to box b; returns their count (0: nothing to draw). */
static int cui__bd_line_points(const cui__nc_view *v, const cui__nc_box *a, const cui__nc_box *b, cui_board_path path, cui_vec2 *out, int cap) {
    cui_vec2 pa = cui__nc_screen(v, a->x, a->y), pb = cui__nc_screen(v, b->x, b->y);
    cui_rect ra = { pa.x, pa.y, a->w * v->z, a->h * v->z }, rb = { pb.x, pb.y, b->w * v->z, b->h * v->z };
    cui_vec2 ca = { ra.x + ra.w * 0.5f, ra.y + ra.h * 0.5f }, cb = { rb.x + rb.w * 0.5f, rb.y + rb.h * 0.5f };
    float dx = cb.x - ca.x, dy = cb.y - ca.y;
    if (fabsf(dx) < 0.01f && fabsf(dy) < 0.01f) return 0;
    if (path == CUI_BOARD_STRAIGHT) {
        out[0] = cui__bd_exit(ra, ca, dx, dy);
        out[1] = cui__bd_exit(rb, cb, -dx, -dy);
        return 2;
    }
    /* curves and elbows run between the sides that face each other, leaving and entering square to them */
    bool across = fabsf(dx) / CUI_MAX(1.0f, ra.w + rb.w) >= fabsf(dy) / CUI_MAX(1.0f, ra.h + rb.h);
    cui_vec2 p0, p3, t;
    if (across) {
        float s = dx >= 0 ? 1.0f : -1.0f;
        p0.x = s > 0 ? ra.x + ra.w : ra.x; p0.y = ca.y;
        p3.x = s > 0 ? rb.x : rb.x + rb.w; p3.y = cb.y;
        t.x = s; t.y = 0;
    } else {
        float s = dy >= 0 ? 1.0f : -1.0f;
        p0.x = ca.x; p0.y = s > 0 ? ra.y + ra.h : ra.y;
        p3.x = cb.x; p3.y = s > 0 ? rb.y : rb.y + rb.h;
        t.x = 0; t.y = s;
    }
    if (path == CUI_BOARD_ELBOW) {
        /* out to the middle of the gap, across, and in */
        cui_vec2 c1, c2;
        if (across) { float m = (p0.x + p3.x) * 0.5f; c1.x = m; c1.y = p0.y; c2.x = m; c2.y = p3.y; }
        else { float m = (p0.y + p3.y) * 0.5f; c1.x = p0.x; c1.y = m; c2.x = p3.x; c2.y = m; }
        float bend = across ? fabsf(p3.y - p0.y) : fabsf(p3.x - p0.x);
        int n = 0;
        out[n++] = p0;
        if (bend >= 0.5f) {
            float r = CUI_MIN(8.0f * v->z, bend * 0.5f);
            n = cui__bd_corner(out, n, cap - 1, p0, c1, c2, r);
            n = cui__bd_corner(out, n, cap - 1, c1, c2, p3, r);
        }
        out[n++] = p3;
        return n;
    }
    float run = fabsf(across ? p3.x - p0.x : p3.y - p0.y);
    float k = CUI_MAX(run * 0.5f, 24.0f * v->z);
    cui_vec2 p1 = { p0.x + t.x * k, p0.y + t.y * k }, p2 = { p3.x - t.x * k, p3.y - t.y * k };
    float len = hypotf(p3.x - p0.x, p3.y - p0.y) + 2 * k;
    int n = (int)CUI_CLAMP(len / 6.0f, 8.0f, (float)(cap - 1));
    for (int i = 0; i <= n; i++) {
        float u = (float)i / (float)n, w = 1 - u;
        float w0 = w * w * w, w1 = 3 * w * w * u, w2 = 3 * w * u * u, w3 = u * u * u;
        out[i].x = w0 * p0.x + w1 * p1.x + w2 * p2.x + w3 * p3.x;
        out[i].y = w0 * p0.y + w1 * p1.y + w2 * p2.y + w3 * p3.y;
    }
    return n + 1;
}

static float cui__bd_line_width(const cui__bd_link *l, float z) {
    float w = l->width > 0 ? l->width : 1.5f;
    return cui__clampf(w * z, 1.0f, w * 2.0f);
}

/* The point at `dist` along the polyline from its start (back: from its end). */
static cui_vec2 cui__bd_along(const cui_vec2 *p, int n, float dist, bool back) {
    for (int k = 0; k + 1 < n; k++) {
        cui_vec2 a = back ? p[n - 1 - k] : p[k], b = back ? p[n - 2 - k] : p[k + 1];
        float l = hypotf(b.x - a.x, b.y - a.y);
        if (l >= dist && l > 0) { cui_vec2 q = { a.x + (b.x - a.x) * dist / l, a.y + (b.y - a.y) * dist / l }; return q; }
        dist -= l;
    }
    return back ? p[0] : p[n - 1];
}

/* the polyline without `a` points of length at its start and `b` at its end; returns the count in out */
static int cui__bd_trim(const cui_vec2 *p, int n, float a, float b, cui_vec2 *out) {
    float total = 0;
    for (int k = 0; k + 1 < n; k++) total += hypotf(p[k + 1].x - p[k].x, p[k + 1].y - p[k].y);
    if (a + b >= total) return 0;
    int m = 0;
    float s = 0;
    out[m++] = cui__bd_along(p, n, a, false);
    for (int k = 1; k + 1 < n; k++) {
        s += hypotf(p[k].x - p[k - 1].x, p[k].y - p[k - 1].y);
        if (s > a && s < total - b) out[m++] = p[k];
    }
    out[m++] = cui__bd_along(p, n, b, true);
    return m;
}

/* Dashes (or dots) along the polyline, spaced by length along it. It walks the line once; a
 * stretch out of `clip` (the board on the screen) is skipped to its last pattern start, so a long
 * patterned line mostly off screen costs about what its visible part does. The pattern counts
 * from the line's start, so it does not shift as the view pans. */
static void cui__bd_dashes(cui_ctx *ui, const cui_vec2 *p, int n, float th, cui_color c, cui_board_dash dash, cui_rect clip) {
    bool dots = dash == CUI_BOARD_DOTTED;
    float on = dots ? 0 : th * 3.0f + 3.0f, period = on + (dots ? th * 2.2f + 2.5f : th * 2.0f + 2.5f);
    float reach = dots ? th * 0.7f + 0.3f : th;   /* how far a piece paints past its centre line */
    cui_rect seen = { clip.x - reach, clip.y - reach, clip.w + 2 * reach, clip.h + 2 * reach };
    float at[CUI__BD_LINE_PTS];                   /* length along the line at each point */
    float total = 0;
    for (int k = 0; k < n; k++) { at[k] = total; if (k + 1 < n) total += hypotf(p[k + 1].x - p[k].x, p[k + 1].y - p[k].y); }
    cui_vec2 run[CUI__BD_LINE_PTS + 2];
    int k = 0;
    for (float t = 0; t < total; t += period) {
        while (k + 2 < n && at[k + 1] <= t) k++;
        cui_vec2 a = p[k], b = p[k + 1];
        bool visible = CUI_MIN(a.x, b.x) <= seen.x + seen.w && CUI_MAX(a.x, b.x) >= seen.x &&
                       CUI_MIN(a.y, b.y) <= seen.y + seen.h && CUI_MAX(a.y, b.y) >= seen.y;
        if (!visible) {
            /* out of sight: on to the last start whose piece still reaches past this stretch */
            float skip = floorf((at[k + 1] - on - t) / period);
            if (skip >= 2) { t += (skip - 1) * period; continue; }
        }
        float sl = at[k + 1] - at[k], u = sl > 0 ? (t - at[k]) / sl : 0;
        cui_vec2 q = { a.x + (b.x - a.x) * u, a.y + (b.y - a.y) * u };
        if (dots) { if (cui__rect_contains(seen, q.x, q.y)) cui_draw_circle(ui, q, th * 0.7f + 0.3f, c); continue; }
        /* a dash from t to t + on, round the corners it passes */
        float end = CUI_MIN(t + on, total);
        int m = 0, j = k;
        run[m++] = q;
        while (j + 2 < n && at[j + 1] < end) { run[m++] = p[j + 1]; j++; }
        float sj = at[j + 1] - at[j], w = sj > 0 ? (end - at[j]) / sj : 1;
        cui_vec2 e = { p[j].x + (p[j + 1].x - p[j].x) * w, p[j].y + (p[j + 1].y - p[j].y) * w };
        run[m++] = e;
        float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
        for (int i = 0; i < m; i++) { x0 = CUI_MIN(x0, run[i].x); y0 = CUI_MIN(y0, run[i].y); x1 = CUI_MAX(x1, run[i].x); y1 = CUI_MAX(y1, run[i].y); }
        if (x1 >= seen.x && x0 <= seen.x + seen.w && y1 >= seen.y && y0 <= seen.y + seen.h) cui_draw_polyline(ui, run, m, th, c, false);
    }
}

/* an arrowhead with its tip at the polyline's end (back: at its start), pointing out of the line */
static void cui__bd_arrow(cui_ctx *ui, const cui_vec2 *p, int n, float s, cui_color c, bool at_start) {
    cui_vec2 tip = at_start ? p[0] : p[n - 1], from = cui__bd_along(p, n, s, !at_start);
    float dx = tip.x - from.x, dy = tip.y - from.y, l = hypotf(dx, dy);
    if (l < 0.01f) return;
    dx /= l; dy /= l;
    float half = s * 0.5f;
    cui_vec2 tri[3] = { tip, { tip.x - dx * s - dy * half, tip.y - dy * s + dx * half }, { tip.x - dx * s + dy * half, tip.y - dy * s - dx * half } };
    cui_draw_polygon(ui, tri, 3, c);
}

/* A line with its dashes and end caps; the line stops short of an arrowhead so it does not poke through. */
static void cui__bd_stroke(cui_ctx *ui, const cui_vec2 *p, int n, float th, cui_color c, const cui__bd_link *l, cui_rect clip) {
    float s = th * 3.0f + 4.0f;   /* arrowhead length */
    float a = l->start_cap == CUI_BOARD_CAP_ARROW ? s * 0.6f : 0, b = l->end_cap == CUI_BOARD_CAP_ARROW ? s * 0.6f : 0;
    cui_vec2 body[CUI__BD_LINE_PTS];
    const cui_vec2 *q = p;
    int m = n;
    if (a > 0 || b > 0) { m = cui__bd_trim(p, n, a, b, body); q = body; }
    if (m >= 2) {
        if (l->dash == CUI_BOARD_SOLID) cui_draw_polyline(ui, q, m, th, c, false);
        else cui__bd_dashes(ui, q, m, th, c, l->dash, clip);
    }
    float dot = th * 1.2f + 1.8f;
    if (l->start_cap == CUI_BOARD_CAP_ARROW) cui__bd_arrow(ui, p, n, s, c, true);
    else if (l->start_cap == CUI_BOARD_CAP_DOT) cui_draw_circle(ui, p[0], dot, c);
    if (l->end_cap == CUI_BOARD_CAP_ARROW) cui__bd_arrow(ui, p, n, s, c, false);
    else if (l->end_cap == CUI_BOARD_CAP_DOT) cui_draw_circle(ui, p[n - 1], dot, c);
}

/* The link under (mx, my), as an index into s->links, or -1: within its painted stroke or a few
 * points of a thin one, the nearest when several are. */
static int cui__bd_link_at(const cui__nc_view *v, const cui__bd_set *s, float mx, float my) {
    int hit = -1;
    float best = 1e30f;
    cui_vec2 m = { mx, my };
    for (int i = 0; i < s->link_n; i++) {
        const cui__nc_box *a = cui__bd_find(s->items, s->item_n, s->links[i].from);
        const cui__nc_box *b = cui__bd_find(s->items, s->item_n, s->links[i].to);
        if (!a || !b) continue;
        cui_vec2 pts[CUI__BD_LINE_PTS];
        int n = cui__bd_line_points(v, a, b, s->links[i].path, pts, CUI__BD_LINE_PTS);
        float reach = CUI_MAX(5.0f, cui__bd_line_width(&s->links[i], v->z) * 0.5f + 2.0f);
        for (int k = 0; k + 1 < n; k++) {
            float d = cui__nc_seg_dist(m, pts[k], pts[k + 1]);
            if (d < reach && d < best) { best = d; hit = i; }
        }
    }
    return hit;
}

/* ---- painting ---- */

static void cui__paint_bd_canvas(cui_ctx *ui, cui_node *n, void *data) {
    cui__bd_build *b = data;
    cui__bd_state *st = b->st;
    cui_rect r = n->rect;
    cui__nc_view pv = cui__bd_paint_view(&b->v, r);
    if (!b->disabled) cui__nc_paint_wheel_hit(ui, cui__bd_scroll_id(b->id), r);
    cui_push_clip(ui, cui__rect_inset(r, 1));   /* the node's own clip starts after this paint */
    if (b->grid) cui__nc_paint_grid(ui, &pv, r);
    /* lines, under the items; the selected one last, on top */
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < b->link_n; i++) {
            const cui__bd_link *l = &b->links[i];
            bool sel = st->link_selected && st->sel_link == l->id;
            if (sel != (pass == 1)) continue;
            const cui__nc_box *fa = cui__bd_find(b->items, b->item_n, l->from);
            const cui__nc_box *fb = cui__bd_find(b->items, b->item_n, l->to);
            if (!fa || !fb) continue;
            cui_vec2 pts[CUI__BD_LINE_PTS];
            int np = cui__bd_line_points(&pv, fa, fb, l->path, pts, CUI__BD_LINE_PTS);
            if (np < 2) continue;
            float th = cui__bd_line_width(l, pv.z);
            cui__bd_stroke(ui, pts, np, sel ? th + 0.75f : th, sel ? cui__emphasis(ui) : l->color, l, r);
        }
    cui_pop_clip(ui);
}

/* over the items: the marquee, the board's focus ring and the minimap */
static void cui__paint_bd_overlay(cui_ctx *ui, cui_node *n, void *data) {
    cui__bd_build *b = data;
    cui__bd_state *st = b->st;
    cui_rect r = ui->nodes[b->canvas].rect;
    cui__nc_view pv = cui__bd_paint_view(&b->v, r);
    cui_color acc = cui__emphasis(ui);
    if (st->mode == BD_MARQUEE) cui__nc_paint_marquee(ui, &pv, st->mq_x0, st->mq_y0, st->mq_x1, st->mq_y1, acc);
    if (b->bg_focus) {
        float rad = ui->theme.radius_panel;
        cui_draw_rect_border(ui, cui__rect_inset(r, 1), CUI_CORNERS(rad - 1), cui__rgba(0, 0, 0, 0), CUI_C(ui, FOCUS_RING), 2);
    }
    st->mm.valid = false;
    if (b->minimap && b->item_n > 0) cui__nc_paint_minimap(ui, &pv, r, b->items, b->item_n, &st->sel, acc, &st->mm);
    (void)n;
}

static void cui__paint_bd_card(cui_ctx *ui, cui_node *n, void *data) {
    cui__bd_card *cp = data;
    cui_rect r = n->rect;
    cui_corners rc = CUI_CORNERS(cp->rad);
    if (cp->lift > 0.01f) cui__draw_elevation(ui, r, rc, CUI_ELEVATION_FLOATING, cp->lift);
    cui_color edge = cui_mix(cp->edge, CUI_C(ui, FOREGROUND), cp->hover * 0.18f);
    if (cp->bw > 0) cui_draw_rect_border(ui, r, rc, cp->bg, edge, cp->bw);
    else cui_draw_rect(ui, r, rc, cp->bg);
    if (cp->selected) {
        float g = 3;
        cui_rect ring = { r.x - g, r.y - g, r.w + 2 * g, r.h + 2 * g };
        cui_draw_rect_border(ui, ring, CUI_CORNERS(cp->rad + g), cui__rgba(0, 0, 0, 0), cui__emphasis(ui), 2);
    }
    if (cp->focus > 0.01f) cui__draw_focus_ring(ui, r, rc, cp->focus);
}

/* ---- input ---- */

static void cui__bd_select_only(cui_ctx *ui, cui__bd_state *st, int item) {
    st->sel.n = 0;
    cui__nc_push(ui, &st->sel, item);
    st->link_selected = false;
}

/* Delete, resolved against the frame just built (its items, their locks and its lines): the
 * selected items that are not locked, every line that touches one, and the selected line, each
 * once. The events wait for the next frame. */
static void cui__bd_delete_selection(cui_ctx *ui, cui__bd_state *st, const cui__bd_set *cur) {
    cui__nc_ints *gone = &st->pend_del;
    int first = gone->n;
    for (int i = 0; i < st->sel.n; i++) {
        int id = st->sel.v[i];
        if (cui__bd_find(cur->items, cur->item_n, id) && !cui__nc_has(&cur->locked, id)) cui__nc_push(ui, gone, id);
    }
    for (int i = 0; i < cur->link_n; i++) {
        const cui__bd_link *l = &cur->links[i];
        bool touches = false;
        for (int k = first; k < gone->n; k++) if (gone->v[k] == l->from || gone->v[k] == l->to) touches = true;
        if ((touches || (st->link_selected && st->sel_link == l->id)) && !cui__nc_has(&st->pend_rm, l->id)) cui__nc_push(ui, &st->pend_rm, l->id);
    }
    st->sel.n = 0;
    st->link_selected = false;
}

/* keys while the board or one of its items has focus: Delete, F fits, Ctrl+A, Escape,
 * arrows move the selection, + - and 0 zoom */
static void cui__bd_keys(cui_ctx *ui, cui__bd_build *b, const cui__bd_set *prev) {
    cui__bd_state *st = b->st;
    bool prim = (ui->mods & CUI_MOD_PRIMARY) != 0;
    if (!b->read_only && (cui_key_pressed(ui, CUI_KEY_DELETE) || cui_key_pressed(ui, CUI_KEY_BACKSPACE))) {
        st->del_request = true;   /* resolved in cui_board_end */
        ui->kpressed[CUI_KEY_DELETE] = ui->kpressed[CUI_KEY_BACKSPACE] = 0;
    }
    if (cui_key_pressed(ui, CUI_KEY_F) && !prim) { st->frame_all = true; ui->kpressed[CUI_KEY_F] = 0; }
    if (prim && cui_key_pressed(ui, CUI_KEY_A)) {
        st->sel.n = 0;
        for (int i = 0; i < prev->item_n; i++) cui__nc_push(ui, &st->sel, prev->items[i].id);
        ui->kpressed[CUI_KEY_A] = 0;
    }
    if (cui_key_pressed(ui, CUI_KEY_ESCAPE)) {
        if (st->mode == BD_MARQUEE) st->mode = BD_IDLE;
        else { st->sel.n = 0; st->link_selected = false; }
        ui->kpressed[CUI_KEY_ESCAPE] = 0;
    }
    if (st->sel.n > 0 && !b->read_only) {
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

/* dragging the selected items, once the press has moved far enough to be a drag */
static void cui__bd_drag(cui_ctx *ui, cui__bd_build *b) {
    cui__bd_state *st = b->st;
    float mx = ui->mdown[0] ? ui->mx : ui->release_x, my = ui->mdown[0] ? ui->my : ui->release_y;   /* a drop is where it was let go */
    if (st->mode == BD_PEND_DRAG && ui->mdown[0]) {
        float dx = mx - st->press_x, dy = my - st->press_y;
        if (dx * dx + dy * dy > CUI__NC_DRAG * CUI__NC_DRAG) st->mode = BD_DRAG;
    }
    if (st->mode == BD_DRAG) {
        st->move_dx += (mx - st->last_x) / b->v.z;
        st->move_dy += (my - st->last_y) / b->v.z;
        cui__set_cursor(ui, CUI_CURSOR_MOVE);
        ui->animating = true;
    }
    st->last_x = mx; st->last_y = my;
    if (!ui->mdown[0]) {
        /* a plain click on one of several selected items selects just that one */
        if (st->mode == BD_PEND_DRAG && !st->press_mod && st->press_was_sel) cui__bd_select_only(ui, st, st->press_item);
        if (st->mode == BD_DRAG) st->snap_moved = b->snap > 0;
        st->mode = BD_IDLE;
    }
}

/* a marquee: selects the items it touches, added to what was selected when it started */
static void cui__bd_marquee(cui_ctx *ui, cui__bd_build *b, const cui__bd_set *prev) {
    cui__bd_state *st = b->st;
    float mx = ui->mdown[0] ? ui->mx : ui->release_x, my = ui->mdown[0] ? ui->my : ui->release_y;
    if (st->mode == BD_PEND_MARQUEE && ui->mdown[0]) {
        float dx = mx - st->press_x, dy = my - st->press_y;
        if (dx * dx + dy * dy > CUI__NC_DRAG * CUI__NC_DRAG) st->mode = BD_MARQUEE;
    }
    st->mq_x1 = (mx - b->v.ox) / b->v.z; st->mq_y1 = (my - b->v.oy) / b->v.z;
    if (st->mode == BD_MARQUEE) {
        float x0 = CUI_MIN(st->mq_x0, st->mq_x1), y0 = CUI_MIN(st->mq_y0, st->mq_y1);
        float x1 = CUI_MAX(st->mq_x0, st->mq_x1), y1 = CUI_MAX(st->mq_y0, st->mq_y1);
        cui__nc_marquee_select(ui, &st->sel, &st->mq_base, prev->items, prev->item_n, x0, y0, x1, y1);
        ui->animating = true;
    }
    if (!ui->mdown[0]) st->mode = BD_IDLE;
}

/* no diagonal resize cursor on every platform: a corner shows the move cursor */
static cui_cursor cui__bd_resize_cursor(int edges) {
    bool h = (edges & (CUI__GRAB_LEFT | CUI__GRAB_RIGHT)) != 0, v = (edges & (CUI__GRAB_TOP | CUI__GRAB_BOTTOM)) != 0;
    return h && v ? CUI_CURSOR_MOVE : h ? CUI_CURSOR_RESIZE_EW : CUI_CURSOR_RESIZE_NS;
}

/* resizing: the held edges follow the pointer, the others stay; the item keeps its minimum size */
static void cui__bd_resize(cui_ctx *ui, cui__bd_build *b) {
    cui__bd_state *st = b->st;
    float mx = ui->mdown[0] ? ui->mx : ui->release_x, my = ui->mdown[0] ? ui->my : ui->release_y;
    float dx = (mx - st->press_x) / b->v.z, dy = (my - st->press_y) / b->v.z;
    cui_rect f = st->rs_from;
    float x0 = f.x, y0 = f.y, x1 = f.x + f.w, y1 = f.y + f.h;
    int e = st->rs_edges;
    if (e & CUI__GRAB_LEFT) x0 = CUI_MIN(f.x + dx, x1 - st->rs_min_w);
    if (e & CUI__GRAB_RIGHT) x1 = CUI_MAX(x1 + dx, x0 + st->rs_min_w);
    if (e & CUI__GRAB_TOP) y0 = CUI_MIN(f.y + dy, y1 - st->rs_min_h);
    if (e & CUI__GRAB_BOTTOM) y1 = CUI_MAX(y1 + dy, y0 + st->rs_min_h);
    bool done = !ui->mdown[0];
    if (done && b->snap > 0) {
        /* the edges that moved land on the grid, never below the minimum */
        if (e & CUI__GRAB_LEFT) x0 = cui__bd_snap_low(x0, b->snap, x1 - st->rs_min_w);
        if (e & CUI__GRAB_RIGHT) x1 = cui__bd_snap_high(x1, b->snap, x0 + st->rs_min_w);
        if (e & CUI__GRAB_TOP) y0 = cui__bd_snap_low(y0, b->snap, y1 - st->rs_min_h);
        if (e & CUI__GRAB_BOTTOM) y1 = cui__bd_snap_high(y1, b->snap, y0 + st->rs_min_h);
    }
    st->rs_rect.x = x0; st->rs_rect.y = y0; st->rs_rect.w = x1 - x0; st->rs_rect.h = y1 - y0;
    if (st->rs_fit && !(e & (CUI__GRAB_TOP | CUI__GRAB_BOTTOM))) st->rs_rect.h = 0;
    st->rs_apply = true;
    cui__set_cursor(ui, cui__bd_resize_cursor(e));
    ui->animating = true;
    if (done) st->mode = BD_IDLE;
}

/* the gesture in progress: pan, drag items, marquee, resize, or the minimap */
static void cui__bd_gesture(cui_ctx *ui, cui__bd_build *b, const cui__bd_set *prev, cui_id mmid) {
    cui__bd_state *st = b->st;
    switch (st->mode) {
    case BD_PAN:
        if (!cui__nc_pan(ui, &b->v, &st->last_x, &st->last_y, b->id)) st->mode = BD_IDLE;
        break;
    case BD_PEND_DRAG:
    case BD_DRAG:
        cui__bd_drag(ui, b);
        break;
    case BD_PEND_MARQUEE:
    case BD_MARQUEE:
        cui__bd_marquee(ui, b, prev);
        break;
    case BD_RESIZE:
        cui__bd_resize(ui, b);
        break;
    case BD_MINIMAP:
        if (!ui->mdown[0]) { st->mode = BD_IDLE; break; }
        cui__nc_minimap_follow(ui, &b->v, &st->mm);
        ui->active = mmid;
        break;
    default: break;
    }
}

/* A press on the empty canvas: a line under it is selected; otherwise it pans, or with
 * Shift or Ctrl (plain with .drag_selects) a marquee starts. Ctrl, and Shift unless Shift
 * pans, add to the selection. */
static void cui__bd_press_canvas(cui_ctx *ui, cui__bd_build *b, const cui__bd_set *prev) {
    cui__bd_state *st = b->st;
    float mx = ui->mx, my = ui->my;
    b->consumed = true;
    bool shift = (ui->mods & CUI_MOD_SHIFT) != 0, prim = (ui->mods & CUI_MOD_PRIMARY) != 0;
    int hit = cui__bd_link_at(&b->v, prev, mx, my);
    if (hit >= 0) {
        if (!shift && !prim) st->sel.n = 0;
        st->link_selected = true;
        st->sel_link = prev->links[hit].id;
        return;
    }
    bool marquee = b->drag_selects ? !shift : (shift || prim);
    bool add = prim || (shift && !b->drag_selects);
    st->link_selected = false;
    st->mq_base.n = 0;
    if (add) for (int i = 0; i < st->sel.n; i++) cui__nc_push(ui, &st->mq_base, st->sel.v[i]);
    else st->sel.n = 0;
    st->press_x = st->last_x = mx; st->press_y = st->last_y = my;
    if (marquee) {
        st->mode = BD_PEND_MARQUEE;
        st->mq_x0 = st->mq_x1 = (mx - b->v.ox) / b->v.z;
        st->mq_y0 = st->mq_y1 = (my - b->v.oy) / b->v.z;
    } else {
        st->mode = BD_PAN;
    }
}

/* ---- the board ---- */

static void cui__bd_build_canvas(cui_ctx *ui, cui__bd_build *b, const cui_board_opts *o, bool focus_visible, bool hot_mine) {
    int en = cui__node_new(ui, b->id);
    cui_node *n = &ui->nodes[en];
    bool dark = ui->mode == CUI_THEME_DARK;
    n->w = o->width.kind == CUI_SIZE_FIT && o->width.value == 0 ? CUI_FILL : o->width;
    n->h = o->height.kind == CUI_SIZE_FIT && o->height.value == 0 ? CUI_PX(480) : o->height;
    n->bg = dark ? cui_mix(CUI_C(ui, BACKGROUND), CUI_C(ui, SURFACE), 0.45f) : CUI_C(ui, SURFACE_MUTED);
    n->border = 1;
    n->border_color = CUI_C(ui, BORDER);
    n->radius = CUI_CORNERS(ui->theme.radius_panel);
    n->flags |= CUI_NF_CLIP;
    n->align = CUI_ALIGN_START;
    n->paint = cui__paint_bd_canvas;
    n->paint_data = b;
    cui__mark_interactive(ui, en, CUI_IF_FOCUSABLE);
    b->bg_focus = focus_visible;
    b->canvas = en;
    if (hot_mine && b->st->mode == BD_IDLE && cui_key_action_down(ui, CUI_KEY_SPACE)) cui__set_cursor(ui, CUI_CURSOR_MOVE);
    cui__open(ui, en);
}

bool cui_board_begin_ex(cui_ctx *ui, const char *id_str, cui_node_view *view, const cui_board_opts *o) {
    cui_board_opts d;
    memset(&d, 0, sizeof d);
    if (!o) o = &d;
    if (!view || cui__bd_cur(ui)) return false;   /* boards do not nest */
    cui__bd_build *b = cui__frame_alloc(ui, sizeof *b);
    memset(b, 0, sizeof *b);
    b->id = cui_make_id(ui, id_str ? id_str : "board");
    b->v.view = view;
    b->disabled = cui_is_disabled(ui);
    b->read_only = o->read_only;
    b->drag_selects = o->drag_selects;
    b->snap = o->snap > 0 ? o->snap : 0;
    b->v.min_zoom = o->min_zoom > 0 ? o->min_zoom : 0.1f;
    b->v.max_zoom = o->max_zoom > 0 && o->max_zoom >= b->v.min_zoom ? o->max_zoom : CUI_MAX(4.0f, b->v.min_zoom);
    b->grid = !o->no_grid;
    b->minimap = o->minimap;
    cui__bd_state *st = cui_state(ui, b->id, sizeof *st);
    cui__state_set_dtor(ui, b->id, cui__bd_dtor);
    b->st = st;
    /* swap the frame sets: the other one now holds last frame */
    st->cur ^= 1;
    cui__bd_set *cur = &st->set[st->cur], *prev = &st->set[st->cur ^ 1];
    cur->item_n = cur->link_n = cur->locked.n = 0;
    st->ev_del.n = st->ev_rm.n = 0;
    st->ev_del_read = st->ev_rm_read = 0;
    /* what the last frame's Delete resolved to */
    for (int i = 0; i < st->pend_del.n; i++) cui__nc_push(ui, &st->ev_del, st->pend_del.v[i]);
    for (int i = 0; i < st->pend_rm.n; i++) cui__nc_push(ui, &st->ev_rm, st->pend_rm.v[i]);
    st->pend_del.n = st->pend_rm.n = 0;
    st->move_dx = st->move_dy = 0;
    /* read only ends a move or a resize under way: editing again takes a new press */
    if (b->read_only && (st->mode == BD_PEND_DRAG || st->mode == BD_DRAG || st->mode == BD_RESIZE)) { st->mode = BD_IDLE; st->rs_apply = false; }
    if (view->zoom <= 0) view->zoom = 1;
    view->zoom = cui__clampf(view->zoom, b->v.min_zoom, b->v.max_zoom);
    b->v.has_rect = cui__has_prev_rect(ui, b->id);
    b->v.r = cui__prev_rect(ui, b->id);
    b->v.z = view->zoom;
    b->v.ox = b->v.r.x + view->x; b->v.oy = b->v.r.y + view->y;

    /* is the pointer, or the focus, on this board or one of its items? */
    cui_interact bi = cui__interact(ui, b->id, CUI_IF_FOCUSABLE | CUI_IF_DRAG);
    cui_id mmid = cui__bd_minimap_id(b->id);
    cui_interact mi = cui__interact(ui, mmid, CUI_IF_DRAG | CUI_IF_KEEP_FOCUS);
    bool hot_mine = ui->hot == b->id || ui->hot == mmid;
    bool focus_mine = ui->focus == b->id;
    for (int i = 0; i < prev->item_n; i++) {
        cui_id cid = cui__bd_card_id(b->id, prev->items[i].id);
        if (ui->hot == cid) hot_mine = true;
        if (ui->focus == cid) focus_mine = true;
    }
    float mx = ui->mx, my = ui->my;

    /* input */
    if (b->disabled) {
        st->mode = BD_IDLE;
        st->rs_apply = false;
    } else {
        /* no zoom during a resize: its edges follow the pointer's travel from the press, at the press's zoom */
        if (st->mode != BD_RESIZE) cui__nc_wheel(ui, &b->v, cui__bd_scroll_id(b->id));
        /* the keys are not the board's while a popup or a modal it is not in covers it */
        bool covered = cui__keys_covered(ui) || (ui->prev_modal.id && !cui__layer_in_modal(ui, ui->current_layer, ui->prev_modal.id));
        if (focus_mine && !ui->edit_prev && !covered) cui__bd_keys(ui, b, prev);
    }
    if (st->frame_all && b->v.has_rect) {
        st->frame_all = false;
        cui__nc_frame(&b->v, prev->items, prev->item_n);
    }
    /* pan: the middle button, or Space and drag, anywhere over the board */
    bool over = b->v.has_rect && cui__rect_contains(b->v.r, mx, my) && ui->hot_layer == ui->current_layer && (hot_mine || ui->hot != 0);
    if (!b->disabled && over && st->mode == BD_IDLE && !ui->edit_prev &&
        (ui->mpressed[CUI_MOUSE_MIDDLE] || (ui->mpressed[0] && cui_key_action_down(ui, CUI_KEY_SPACE)))) {
        st->mode = BD_PAN; st->last_x = mx; st->last_y = my;
        b->consumed = true;
    }
    if (!b->disabled) cui__bd_gesture(ui, b, prev, mmid);
    if (mi.hovered && ui->mpressed[0] && !b->consumed && st->mode == BD_IDLE && !b->disabled) {
        st->mode = BD_MINIMAP;
        b->consumed = true;
        cui__nc_minimap_follow(ui, &b->v, &st->mm);   /* a click moves there, not only a drag */
        ui->active = mmid;
    }
    if (bi.hovered && ui->mpressed[0] && !b->consumed && st->mode == BD_IDLE && !b->disabled) cui__bd_press_canvas(ui, b, prev);
    if (st->mode != BD_IDLE && st->mode != BD_PAN && st->mode != BD_MINIMAP && ui->mdown[0] && ui->active == 0) ui->active = b->id;
    cui__spring(ui, &st->lift, st->mode == BD_DRAG ? 1.0f : 0.0f, CUI_SPRING_SNAPPY);

    /* build */
    cui__bd_build_canvas(ui, b, o, bi.focus_visible, hot_mine && !b->disabled);
    cui_push_id(ui, id_str ? id_str : "board");
    ui->building.board = b;
    return true;
}

void cui_board_end(cui_ctx *ui) {
    cui__bd_build *b = cui__bd_cur(ui);
    if (!b) return;
    cui__bd_state *st = b->st;
    if (b->in_item) cui_board_item_end(ui);
    cui_pop_id(ui);
    /* the marquee and the minimap paint over the items */
    int ov = cui__absolute(ui, 0, 0, 0, 0, 0);
    ui->nodes[ov].paint = cui__paint_bd_overlay;
    ui->nodes[ov].paint_data = b;
    int mm = -1;
    if (b->minimap && st->mm.valid) {
        mm = cui__absolute(ui, cui__bd_minimap_id(b->id), st->mm.rect.x - b->v.r.x, st->mm.rect.y - b->v.r.y, st->mm.rect.w, st->mm.rect.h);
        cui__mark_interactive(ui, mm, 0);
    }
    cui__nc_paint_order(ui, b->canvas, &b->cards, ov, mm);   /* selected items above the rest */
    cui__close(ui);   /* the canvas */
    cui__bd_set *cur = &st->set[st->cur];
    if (st->del_request) { st->del_request = false; cui__bd_delete_selection(ui, st, cur); }
    /* forget selected items and lines that are gone */
    for (int i = st->sel.n - 1; i >= 0; i--)
        if (!cui__bd_find(cur->items, cur->item_n, st->sel.v[i])) st->sel.v[i] = st->sel.v[--st->sel.n];
    if (st->link_selected) {
        bool found = false;
        for (int i = 0; i < cur->link_n; i++) if (cur->links[i].id == st->sel_link) found = true;
        if (!found) st->link_selected = false;
    }
    if (st->mode == BD_RESIZE && !cui__bd_find(cur->items, cur->item_n, st->rs_item)) st->mode = BD_IDLE;
    if (st->mode != BD_RESIZE) st->rs_apply = false;
    st->snap_moved = false;
    b->items = cur->items; b->item_n = cur->item_n;
    b->links = cur->links; b->link_n = cur->link_n;
    ui->building.board = NULL;
    ui->last_id = b->id;
    ui->last_node = b->canvas;
}

/* the item's colors: a tone's light tint, the caller's fill, or the surface */
static void cui__bd_item_colors(cui_ctx *ui, const cui_board_item_opts *o, cui_color *bg, cui_color *edge) {
    bool dark = ui->mode == CUI_THEME_DARK;
    if (cui__color_set(o->fill)) {
        *bg = cui_color_of(ui, o->fill);
        *edge = cui_mix(*bg, CUI_C(ui, FOREGROUND), 0.22f);
    } else if (o->tone != CUI_TONE_NEUTRAL) {
        cui_color t = cui__tone_color(ui, o->tone);
        *bg = cui_mix(CUI_C(ui, SURFACE), t, dark ? 0.16f : 0.10f);
        *edge = cui_mix(CUI_C(ui, BORDER), t, dark ? 0.55f : 0.45f);
    } else {
        *bg = dark ? CUI_C(ui, SURFACE_RAISED) : CUI_C(ui, SURFACE);
        *edge = CUI_C(ui, BORDER);
    }
    if (cui__color_set(o->border)) *edge = cui_color_of(ui, o->border);
}

/* what an item knows when it is pressed */
typedef struct { int item; cui_rect box; int edges; float min_w, min_h; bool fit, movable; } cui__bd_press;

/* a press on an item: on its edge it starts a resize, inside it selects and may start a drag */
static void cui__bd_press_item(cui_ctx *ui, cui__bd_build *b, const cui__bd_press *p) {
    cui__bd_state *st = b->st;
    bool toggle = (ui->mods & CUI_MOD_PRIMARY) != 0, add = (ui->mods & CUI_MOD_SHIFT) != 0;
    bool selected = cui__nc_has(&st->sel, p->item);
    b->consumed = true;
    st->press_x = st->last_x = ui->mx; st->press_y = st->last_y = ui->my;
    st->link_selected = false;
    if (p->edges) {
        if (!selected) cui__bd_select_only(ui, st, p->item);
        st->mode = BD_RESIZE;
        st->rs_item = p->item; st->rs_edges = p->edges; st->rs_from = p->box;
        st->rs_min_w = p->min_w; st->rs_min_h = p->min_h; st->rs_fit = p->fit;
        return;
    }
    st->press_was_sel = selected;
    st->press_mod = toggle || add;
    if (toggle && selected) cui__nc_remove(&st->sel, p->item);
    else if ((toggle || add) && !selected) cui__nc_push(ui, &st->sel, p->item);
    else if (!selected) cui__bd_select_only(ui, st, p->item);
    if (cui__nc_has(&st->sel, p->item) && p->movable) {
        st->mode = BD_PEND_DRAG;
        st->press_item = p->item;
    }
}

bool cui_board_item_begin_ex(cui_ctx *ui, int item_id, cui_rect *rect, const cui_board_item_opts *o) {
    cui_board_item_opts d;
    memset(&d, 0, sizeof d);
    if (!o) o = &d;
    cui__bd_build *b = cui__bd_cur(ui);
    if (!b || b->in_item || !rect) return false;
    cui__bd_state *st = b->st;
    cui__bd_set *cur = &st->set[st->cur];
    float z = b->v.z;
    bool movable = !o->locked && !b->read_only;
    bool resizable = movable && !o->fixed_size;
    float pad = o->pad > 0 ? o->pad : CUI__BD_PAD;
    float min_w = o->min_width > 0 ? o->min_width : CUI__BD_MIN_W, min_h = o->min_height > 0 ? o->min_height : CUI__BD_MIN_H;
    cui_id cid = cui__bd_card_id(b->id, item_id);
    cui_id content_id = cid ^ 0xC0E7Bu;

    /* what the gestures did to the rectangle */
    bool selected = cui__nc_has(&st->sel, item_id);
    if (selected && movable) {
        rect->x += st->move_dx; rect->y += st->move_dy;
        if (st->snap_moved) { rect->x = cui__bd_snapf(rect->x, b->snap); rect->y = cui__bd_snapf(rect->y, b->snap); }
    }
    if (st->rs_apply && st->rs_item == item_id && resizable) *rect = st->rs_rect;

    /* its box: a height of 0 follows the content, as last measured */
    float *fit_h = cui_state(ui, cid ^ 0xF17Bu, sizeof(float));
    if (cui__has_prev_rect(ui, content_id)) *fit_h = cui__prev_rect(ui, content_id).h;
    float ph = *fit_h > 0 ? *fit_h : CUI__BD_FIT_H;
    bool fit = rect->h <= 0;
    float w = rect->w > 0 ? rect->w : CUI__BD_W;
    float h = fit ? CUI_MAX(min_h, ph + 2 * pad) : rect->h;
    CUI__GROW(ui, cur->items, cur->item_n, cur->item_cap, 1);
    cui__nc_box *box = &cur->items[cur->item_n++];
    box->id = item_id; box->x = rect->x; box->y = rect->y; box->w = w; box->h = h;
    if (o->locked) cui__nc_push(ui, &cur->locked, item_id);
    cui_vec2 s = cui__nc_screen(&b->v, rect->x, rect->y);
    cui_rect sr = { s.x, s.y, w * z, h * z };
    if (b->v.has_rect) {
        cui_rect seen = cui__rect_intersect(sr, b->v.r);
        if (seen.w <= 0 || seen.h <= 0) return false;   /* out of view: only its box counts */
    }

    /* input */
    cui_interact r = cui__interact(ui, cid, CUI_IF_FOCUSABLE | CUI_IF_DRAG);
    int edges = 0;
    if (resizable && st->mode == BD_IDLE && !b->disabled && (r.hovered || ui->hot == b->id))
        edges = cui__grab_edges(sr, ui->mx, ui->my, CUI__BD_GRAB_IN, CUI__BD_GRAB_OUT);
    if (edges) cui__set_cursor(ui, cui__bd_resize_cursor(edges));
    if ((r.hovered || edges) && ui->mpressed[0] && !b->consumed && st->mode == BD_IDLE && !b->disabled) {
        cui__bd_press p = { item_id, { rect->x, rect->y, w, h }, edges, min_w, min_h, fit, movable };
        cui__bd_press_item(ui, b, &p);
    }
    /* keyboard focus arriving on an item selects it */
    if (r.focus_visible && (!st->kb_has || st->kb_item != item_id) && !cui__nc_has(&st->sel, item_id)) cui__bd_select_only(ui, st, item_id);
    if (r.focused) { st->kb_has = true; st->kb_item = item_id; }
    else if (st->kb_has && st->kb_item == item_id) st->kb_has = false;
    selected = cui__nc_has(&st->sel, item_id);
    float *hover = cui_state(ui, cid ^ 0x51Bu, sizeof(float));
    cui__tween(ui, hover, r.hovered && st->mode == BD_IDLE ? 1.0f : 0.0f, CUI__T_HOVER);
    float *focus = cui_state(ui, cid ^ 0x52Bu, sizeof(float));
    cui__tween(ui, focus, r.focus_visible ? 1.0f : 0.0f, CUI__T_FOCUS);

    /* build: the card, and the content inside it at 100%, scaled with the view */
    cui__bd_card *cp = cui__frame_alloc(ui, sizeof *cp);
    memset(cp, 0, sizeof *cp);
    cui__bd_item_colors(ui, o, &cp->bg, &cp->edge);
    float bw = o->border_width < 0 ? 0 : o->border_width > 0 ? o->border_width : 1.0f;
    cp->bw = bw > 0 ? CUI_MAX(1.0f, bw * z) : 0;
    cp->rad = o->radius < 0 ? CUI_MIN(sr.w, sr.h) * 0.5f : (o->radius > 0 ? o->radius : ui->theme.radius_control) * z;
    cp->selected = selected;
    cp->hover = *hover;
    cp->focus = *focus;
    cp->lift = selected && movable && st->lift.x > 0.01f ? st->lift.x : 0;
    int cn = cui__absolute(ui, cid, s.x - b->v.r.x, s.y - b->v.r.y, sr.w, sr.h);
    cui_node *n = &ui->nodes[cn];
    n->flags |= CUI_NF_CLIP;
    n->radius = CUI_CORNERS(cp->rad);
    n->paint = cui__paint_bd_card;
    n->paint_data = cp;
    cui__mark_interactive(ui, cn, CUI_IF_FOCUSABLE);
    cui__nc_cards_add(ui, &b->cards, cn, selected);
    cui__open(ui, cn);
    cui_push_id_int(ui, item_id);
    float inner = CUI_MAX(1.0f, w - 2 * pad);
    int kn = cui__absolute(ui, content_id, pad * z, pad * z, inner, 0);
    ui->nodes[kn].h = CUI_FIT;
    ui->nodes[kn].gap = 6;
    cui__nc_scale_box(&ui->nodes[kn], inner, ph, z);
    /* components inside see the pointer in their own (unscaled) space */
    cui__nc_host_begin(ui, &b->host, s.x + pad * z, s.y + pad * z, z);
    cui__open(ui, kn);
    b->in_item = true;
    b->card = cn;
    b->card_id = cid;
    return true;
}

void cui_board_item_end(cui_ctx *ui) {
    cui__bd_build *b = cui__bd_cur(ui);
    if (!b || !b->in_item) return;
    cui__close(ui);   /* the content */
    cui__nc_host_end(ui, &b->host);
    cui_pop_id(ui);
    cui__close(ui);   /* the card */
    b->in_item = false;
    ui->last_node = b->card;   /* the card is the previous component, for a selection ring */
    ui->last_id = b->card_id;
}

bool cui_board_selected(cui_ctx *ui, int item_id) {
    cui__bd_build *b = cui__bd_cur(ui);
    return b && cui__nc_has(&b->st->sel, item_id);
}

bool cui_board_link_ex(cui_ctx *ui, int link_id, int from_item, int to_item, const cui_board_link_opts *o) {
    cui_board_link_opts d;
    memset(&d, 0, sizeof d);
    if (!o) o = &d;
    cui__bd_build *b = cui__bd_cur(ui);
    if (!b) return false;
    cui__bd_state *st = b->st;
    cui__bd_set *cur = &st->set[st->cur];
    CUI__GROW(ui, cur->links, cur->link_n, cur->link_cap, 1);
    cui__bd_link *l = &cur->links[cur->link_n++];
    l->id = link_id; l->from = from_item; l->to = to_item;
    l->path = o->path == CUI_BOARD_STRAIGHT || o->path == CUI_BOARD_ELBOW ? o->path : CUI_BOARD_CURVE;
    l->dash = o->dash == CUI_BOARD_DASHED || o->dash == CUI_BOARD_DOTTED ? o->dash : CUI_BOARD_SOLID;
    l->start_cap = o->start_cap == CUI_BOARD_CAP_ARROW || o->start_cap == CUI_BOARD_CAP_DOT ? o->start_cap : CUI_BOARD_CAP_NONE;
    l->end_cap = o->end_cap == CUI_BOARD_CAP_ARROW || o->end_cap == CUI_BOARD_CAP_DOT ? o->end_cap : CUI_BOARD_CAP_NONE;
    l->width = o->width;
    if (cui__color_set(o->color)) l->color = cui_color_of(ui, o->color);
    else if (o->tone != CUI_TONE_NEUTRAL) l->color = cui__tone_color(ui, o->tone);
    else l->color = CUI_C(ui, BORDER_STRONG);
    return st->link_selected && st->sel_link == link_id;
}

bool cui_board_item_deleted(cui_ctx *ui, int *item_id) {
    cui__bd_build *b = cui__bd_cur(ui);
    if (!b || b->st->ev_del_read >= b->st->ev_del.n) return false;
    if (item_id) *item_id = b->st->ev_del.v[b->st->ev_del_read];
    b->st->ev_del_read++;
    return true;
}

bool cui_board_link_removed(cui_ctx *ui, int *link_id) {
    cui__bd_build *b = cui__bd_cur(ui);
    if (!b || b->st->ev_rm_read >= b->st->ev_rm.n) return false;
    if (link_id) *link_id = b->st->ev_rm.v[b->st->ev_rm_read];
    b->st->ev_rm_read++;
    return true;
}
