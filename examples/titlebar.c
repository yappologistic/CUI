/* Title bar: a window that draws its own title bar with cui_title_bar. A
 * sidebar reaches up into the title bar with a segment of its own, and the
 * main segment beside it holds navigation, a search field, actions and the
 * window buttons. The panel button hides the sidebar, and its segment with it.
 * --system-frame keeps the system's title bar (the same bars, without window
 * buttons), --fixed-size makes the window not resizable and --software draws
 * with the software renderer. */
#include <cui/cui.h>
#include <cui/cui_app.h>

#include <stdio.h>
#include <string.h>

typedef struct {
    char query[96];
    int page;
    bool sidebar;
    bool own_frame;   /* the app's bars move the window: Windows with custom_title_bar */
} app_state;

static const char *const pages[] = { "Home", "Inbox", "Starred" };
static const char *const page_icons[] = { CUI_ICON_HOUSE, CUI_ICON_INBOX, CUI_ICON_STAR };

/* Shows or hides the sidebar: in the sidebar's segment while it is open, in the main one while it is not. */
static void sidebar_button(cui_ctx *ui, app_state *d) {
    if (cui_icon_button(ui, CUI_ICON_PANEL_LEFT, .size = CUI_SIZE_SM, .tooltip = d->sidebar ? "Hide sidebar" : "Show sidebar"))
        d->sidebar = !d->sidebar;
}

static void sidebar(cui_ctx *ui, app_state *d) {
    cui_column(ui, .width = CUI_PX(220), .height = CUI_FILL, .bg = CUI_COLOR_SURFACE_MUTED) {
        cui_title_bar(ui, "side", .icon = CUI_ICON_SQUARE_TERMINAL, .title = "Workspace", .no_window_buttons = true, .no_border = true) {
            cui_spacer(ui);
            sidebar_button(ui, d);
        }
        cui_column(ui, .pad = 8, .gap = 2) {
            for (int i = 0; i < 3; i++)
                if (cui_nav_item(ui, pages[i], .icon = page_icons[i], .selected = d->page == i)) d->page = i;
        }
    }
    cui_separator(ui);
}

static void frame(cui_ctx *ui, void *user) {
    app_state *d = user;
    cui_row(ui, .width = CUI_FILL, .height = CUI_FILL) {
        if (d->sidebar) sidebar(ui, d);
        cui_column(ui, .width = CUI_FILL, .height = CUI_FILL) {
            /* the icon and the title move into the sidebar's segment while it is open */
            cui_title_bar(ui, "title", .icon = d->sidebar ? NULL : CUI_ICON_SQUARE_TERMINAL, .title = d->sidebar ? NULL : "Workspace") {
                if (!d->sidebar) sidebar_button(ui, d);
                if (cui_icon_button(ui, CUI_ICON_ARROW_LEFT, .size = CUI_SIZE_SM, .tooltip = "Back", .disabled = d->page == 0))
                    d->page--;
                if (cui_icon_button(ui, CUI_ICON_ARROW_RIGHT, .size = CUI_SIZE_SM, .tooltip = "Forward", .disabled = d->page == 2))
                    d->page++;
                cui_spacer(ui);
                cui_search(ui, "find", d->query, sizeof d->query, .placeholder = "Search", .size = CUI_SIZE_SM, .width = CUI_PX(280));
                cui_spacer(ui);
                bool dark = cui_get_theme_mode(ui) == CUI_THEME_DARK;
                if (cui_icon_button(ui, dark ? CUI_ICON_SUN : CUI_ICON_MOON, .size = CUI_SIZE_SM, .tooltip = "Switch theme"))
                    cui_set_theme(ui, dark ? CUI_THEME_LIGHT : CUI_THEME_DARK);
            }
            cui_column(ui, .pad = 24, .gap = 8) {
                cui_text(ui, pages[d->page], .size = 22, .font = CUI_FONT_MEDIUM);
                const char *hint = d->own_frame ? "Drag the empty part of either title bar segment to move the window."
                                                : "The system draws the title bar here, and these bars show without window buttons.";
                cui_text(ui, d->query[0] ? d->query : hint, .color = CUI_COLOR_TEXT_MUTED);
            }
        }
    }
}

int main(int argc, char **argv) {
    cui_app_desc desc = { .title = "Title bar", .width = 960, .height = 520, .min_width = 640, .custom_title_bar = true };
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--system-frame") == 0) desc.custom_title_bar = false;
        if (strcmp(argv[i], "--fixed-size") == 0) desc.fixed_size = true;
        if (strcmp(argv[i], "--software") == 0) desc.renderer = CUI_RENDERER_SOFTWARE;
    }
    app_state d;
    memset(&d, 0, sizeof d);
    d.sidebar = true;
    d.own_frame = desc.custom_title_bar && strcmp(cui_app_backend(NULL), "win32") == 0;
    return cui_app_run(&desc, frame, &d);
}
