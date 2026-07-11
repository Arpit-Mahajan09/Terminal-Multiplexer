#ifndef GUIDE_H
#define GUIDE_H

#include <notcurses/notcurses.h>
#include "cronos.h"

void enter_scrollback(ClientContext *ctx, Pane *p);
void exit_scrollback(Pane *p);
void print_usage(char *prog); 
void print_short(char *prog);
void render_scrollback_view(Pane *p); 

#endif