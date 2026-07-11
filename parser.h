#ifndef PARSER_H
#define PARSER_H
#include "cronos.h"
void init_terminal_state(TerminalState *state);
void execute_csi_sequence(TerminalState *state, char final_byte, struct ncplane *pane);
void parse_ansi_byte(TerminalState *state, char ch, struct ncplane *pane);
#endif