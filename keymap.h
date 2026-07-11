#ifndef KEYMAP_H
#define KEYMAP_H

#include <notcurses/notcurses.h>
#include "cronos.h"

void load_default_keymap(void);
int process_user_action(ClientContext *ctx, uint32_t id, ncinput *ni);
void send_keystroke_to_server(ClientContext *ctx, uint32_t id);

#endif