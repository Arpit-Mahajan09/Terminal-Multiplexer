#ifndef WINDOW_H
#define WINDOW_H

#include "cronos.h"

void ctx_sync_window(ClientContext *ctx);

Window *window_create(ClientContext *ctx);
void    window_switch(ClientContext *ctx, Window *w);
void    window_next(ClientContext *ctx);
void    window_prev(ClientContext *ctx);
void    window_rename(ClientContext *ctx, const char *name);
void    window_render_bar(ClientContext *ctx);

void    window_finalize(ClientContext *ctx, int server_pane_id);

#endif