#ifndef KEYMAP_H
#define KEYMAP_H

#include <notcurses/notcurses.h>
#include "cronos.h"

void bind_key(uint32_t key, int ctrl, int alt, Action action);
void load_default_keymap(void);
int parse_key_name(const char *name, uint32_t *key_out);
Action parse_action_name(const char *name);
Action resolve_action(uint32_t id, ncinput *ni);
int process_user_action(ClientContext *ctx, uint32_t id, ncinput *ni);
void send_keystroke_to_server(ClientContext *ctx, uint32_t id);

#endif