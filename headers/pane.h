#ifndef PANE_H
#define PANE_H
#include "cronos.h"

void handle_pty_output(ClientContext *ctx, CronosPacket *pkt);
void handle_pane_focus(ClientContext *ctx, Action act); 
void handle_pane_resize(ClientContext *ctx, Action act); 

void horz_split(ClientContext *ctx, CronosPacket *pkt);
void vert_split(ClientContext *ctx, CronosPacket *pkt);

void res_pane_close(ClientContext *ctx, CronosPacket *pkt);
void focus_neighbor(ClientContext *ctx, int direction);
void resize_active_pane(ClientContext *ctx, int direction);
void update_active_ui(ClientContext *ctx);

#endif