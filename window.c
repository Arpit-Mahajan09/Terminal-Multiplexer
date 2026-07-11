#include "headers/window.h"
#include "headers/pane.h"
#include "headers/parser.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <notcurses/notcurses.h>

// helpers______________________________


void ctx_sync_window(ClientContext *ctx) {
    if (!ctx->active_window) return;
    ctx->pane_list_head = ctx->active_window->pane_list_head;
    ctx->active_pane    = ctx->active_window->active_pane;
}

static void hide_window(ClientContext *ctx, Window *w) {
    unsigned int dimy, dimx;
    ncplane_dim_yx(ctx->std, &dimy, &dimx);
    int off = (int)dimy + 200;

    Pane *p = w->pane_list_head;
    while (p) {
        ncplane_move_yx(p->nc_plane, off, 0);
        if (p->border_plane)  ncplane_move_yx(p->border_plane,  off, 0);
        if (p->scroll_overlay) ncplane_move_yx(p->scroll_overlay, off, 0);
        p = p->next;
    }
}

static void show_window(Window *w) {
    Pane *p = w->pane_list_head;
    while (p) {
        ncplane_move_yx(p->nc_plane, p->y, p->x);
        if (p->border_plane)
            ncplane_move_yx(p->border_plane, p->border_y, p->border_x);
        if (p->scroll_overlay)
            ncplane_move_yx(p->scroll_overlay, p->y, p->x);
        p = p->next;
    }
}

// __ Main _____________________________________________

void window_prompt_rename(ClientContext *ctx) {
    if (!ctx->footer || !ctx->active_window) return;

    char buf[32] = {0};
    size_t pos = 0;
    ncinput ni;
    uint32_t id;

    ncplane_erase(ctx->footer);
    ncplane_printf_yx(ctx->footer, 0, 1, "Rename window to: ");
    notcurses_render(ctx->nc);

    while ((id = notcurses_get_blocking(ctx->nc, &ni)) != (uint32_t)-1) {
        if (id == '\r' || id == NCKEY_ENTER) break;
        if (id == NCKEY_ESC) { pos = 0; break; }              // cancel
        if ((id == NCKEY_BACKSPACE || id == 0x7F) && pos > 0) {
            buf[--pos] = '\0';
        } else if (id < 0x80 && id >= 0x20 && pos + 1 < sizeof(buf)) {
            buf[pos++] = (char)id;
        }
        ncplane_erase(ctx->footer);
        ncplane_printf_yx(ctx->footer, 0, 1, "Rename window to: %s", buf);
        notcurses_render(ctx->nc);
    }

    if (pos > 0) {
        window_rename(ctx, buf);       // redraws the bar itself
    } else {
        window_render_bar(ctx);        // restore normal footer on cancel/empty
        notcurses_render(ctx->nc);
    }
}

Window *window_create(ClientContext *ctx) {
    CronosPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.type       = PKT_TYPE_COMMAND;
    pkt.data_len   = 1;
    pkt.payload[0] = REQ_NEW_WINDOW;
    write(ctx->client_sock, &pkt, sizeof(pkt));
    return NULL;  
}

void window_finalize(ClientContext *ctx, int server_pane_id) {
    if (ctx->active_window) hide_window(ctx, ctx->active_window);

    unsigned int dimy, dimx;
    ncplane_dim_yx(ctx->std, &dimy, &dimx);

    struct ncplane_options opts = {
        .y = 0, .x = 0,
        .rows = dimy - 1, .cols = dimx,
    };
    struct ncplane *plane = ncplane_create(ctx->std, &opts);
    ncplane_set_scrolling(plane, true);
    ncplane_set_fg_rgb8(plane, 220, 220, 220);

    // Build pane
    Pane *pane = calloc(1, sizeof(Pane));
    pane->id     = server_pane_id;
    pane->nc_plane = plane;
    pane->height = (int)dimy - 1;
    pane->width  = (int)dimx;
    pane->y = 0; pane->x = 0;
    init_terminal_state(&pane->state);

    Window *w = calloc(1, sizeof(Window));
    w->id = ctx->next_window_id++;
    snprintf(w->name, sizeof(w->name), "win%d", w->id + 1);
    w->pane_list_head = pane;
    w->active_pane    = pane;

    Window **tail = &ctx->window_list_head;
    while (*tail) tail = &(*tail)->next;
    *tail = w;

    ctx->active_window = w;
    ctx_sync_window(ctx);

    send_resize_packet(ctx->client_sock, pane->id, pane->height, pane->width);
    window_render_bar(ctx);
    notcurses_render(ctx->nc);
}

void window_switch(ClientContext *ctx, Window *w) {
    if (w == ctx->active_window) return;

    
    if (ctx->active_window)
        ctx->active_window->active_pane = ctx->active_pane;

    hide_window(ctx, ctx->active_window);
    show_window(w);

    ctx->active_window = w;
    ctx_sync_window(ctx);

    window_render_bar(ctx);
    notcurses_render(ctx->nc);
}

void window_next(ClientContext *ctx) {
    if (!ctx->active_window || !ctx->active_window->next) {
        window_switch(ctx, ctx->window_list_head);
    } else {
        window_switch(ctx, ctx->active_window->next);
    }
}

void window_prev(ClientContext *ctx) {
    if (!ctx->active_window) return;
    Window *prev = NULL, *cur = ctx->window_list_head;
    while (cur && cur != ctx->active_window) { prev = cur; cur = cur->next; }
    if (!prev) {
        Window *last = ctx->window_list_head;
        while (last->next) last = last->next;
        prev = last;
    }
    window_switch(ctx, prev);
}

void window_rename(ClientContext *ctx, const char *name) {
    if (!ctx->active_window) return;
    strncpy(ctx->active_window->name, name, sizeof(ctx->active_window->name) - 1);
    window_render_bar(ctx);
    notcurses_render(ctx->nc);
}

// Renders footer showing window tabs + session info
void window_render_bar(ClientContext *ctx) {
    if (!ctx->footer) return;
    ncplane_erase(ctx->footer);

    // Build window tab string
    char tabstr[512] = {0};
    char tmp[64];
    Window *w = ctx->window_list_head;
    while (w) {
        bool active = (w == ctx->active_window);
        snprintf(tmp, sizeof(tmp), active ? " [%s*] " : " [%s] ", w->name);
        strncat(tabstr, tmp, sizeof(tabstr) - strlen(tabstr) - 1);
        w = w->next;
    }

    ncplane_printf_yx(ctx->footer, 0, 1,
        " Cronos | %s| Session: %s | Sessions: %d | Pane: %d ",
        tabstr,
        ctx->session_name,
        ctx->session_count,
        ctx->active_pane ? ctx->active_pane->id + 1 : 0);

    ncplane_move_top(ctx->footer);
}