#include <notcurses/notcurses.h>
#include "headers/parser.h"

#define PENDING_BUF_SIZE 65536
#define MAX_KEYBINDINGS 64


void print_usage(char *prog){
    (void)prog;

    printf("Cronos Terminal Multiplexer CLI\n\n");
    printf("Usage:\n");
    printf("  %s new <name>      - Create and attach to a new background session\n", prog);
    printf("  %s attach <name>   - Attach to an existing active session\n", prog);
    printf("  %s list            - List all active background sessions\n", prog);
    printf("  %s kill <name>     - Kill an active background session\n", prog);
    printf("  %s killall         - Kill all background sessions\n", prog); 
    printf("  %s --help, -h      - Display this help menu\n", prog);
    printf("  %s --inst, -i      - Display shortcuts for tmux plane handling\n\n", prog);
    printf("Example:\n");
    printf("  cronos new dev_env\n");
}

void print_short(char *prog){
    (void)prog;

    printf("Cronos Terminal Multiplexer CLI\n\n");
    printf("Shortcut Keys: \n");
    
    printf("  Split Vertically       :  Ctrl + v\n");
    printf("  Split Horizontally     :  Ctrl + b\n");
    printf("  Move Focus Left        :  Ctrl + h\n");
    printf("  Move Focus Above       :  Ctrl + j\n"); 
    printf("  Move Focus Below       :  Ctrl + k\n\n");
    printf("  Move Focus Rihgt       :  Ctrl + l\n\n");

    printf("  Resize Pane            :  Alt + Arrow Keys\n");
    printf("  Scroll Pane History    :  Page Up / Page Down\n");
    printf("  Detach Session         :  Ctrl + q\n\n");

    printf("  Close Single Pnae      :  Ctrl + w\n");
    printf("  Close Terminal       :  Ctrl + q\n\n");
}



void render_scrollback_view(Pane *p) {
    if (!p->scroll_overlay) return;
    ncplane_erase(p->scroll_overlay);

    int total  = p->state.scrollback_count;
    int visible = p->height;

    int last_line = total - (p->state.scroll_offset - 1) * visible;
    int first_line = last_line - visible;
    if (first_line < 0) first_line = 0;

    for (int row = 0; row < visible; row++) {
        int idx = first_line + row;
        if (idx >= total) break;
        char *line = p->state.scrollback[
            (p->state.scrollback_head + idx) % SCROLLBACK_MAX_LINES];
        ncplane_putstr_yx(p->scroll_overlay, row, 0, line);
    }

    uint64_t ch = 0;
    ncchannels_set_fg_rgb8(&ch, 0,   0,   0);
    ncchannels_set_bg_rgb8(&ch, 100, 100, 200);
    int saved_y, saved_x;

    ncplane_cursor_yx(p->scroll_overlay, (unsigned*)&saved_y, (unsigned*)&saved_x);
    ncplane_set_channels(p->scroll_overlay, ch);
    ncplane_printf_yx(p->scroll_overlay, 0, 0,
        " SCROLL  offset=%d/%d  PgDn=forward  ESC=live ",
        p->state.scroll_offset, total / visible + 1);

    ncplane_set_fg_default(p->scroll_overlay);
    ncplane_set_bg_default(p->scroll_overlay);
}

void enter_scrollback(ClientContext *ctx, Pane *p) {
    if (p->scroll_overlay) return;  // already scrolling

    unsigned int rows, cols;
    ncplane_dim_yx(p->nc_plane, &rows, &cols);

    struct ncplane_options opts = {
        .y = p->y, .x = p->x,
        .rows = rows, .cols = cols,
    };
    p->scroll_overlay = ncplane_create(ctx->std, &opts);
    ncplane_move_top(p->scroll_overlay);
    ncplane_set_scrolling(p->scroll_overlay, false);

    uint64_t bg = 0;
    ncchannels_set_fg_rgb8(&bg, 220, 220, 220);
    ncchannels_set_bg_rgb8(&bg, 10,  10,  10);
    ncplane_set_base(p->scroll_overlay, " ", 0, bg);

    p->state.scroll_offset = 1;
    render_scrollback_view(p);
}

void exit_scrollback(Pane *p) {
    if (!p->scroll_overlay) return;
    ncplane_destroy(p->scroll_overlay);
    p->scroll_overlay = NULL;
    p->state.scroll_offset = 0;
}