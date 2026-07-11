#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <notcurses/notcurses.h>
#include "headers/cronos.h"

// This file acts as a state machine that would read the ANSI characters from server 
// before sending them to the frontend

static void ansi256_to_rgb(int idx, uint8_t *r, uint8_t *g, uint8_t *b) {
    static const uint8_t basic[16][3] = {
        {0,0,0}, {170,0,0}, {0,170,0}, {170,85,0},
        {0,0,170}, {170,0,170}, {0,170,170}, {170,170,170},
        {85,85,85}, {255,85,85}, {85,255,85}, {255,255,85},
        {85,85,255}, {255,85,255}, {85,255,255}, {255,255,255}
    };
    if (idx < 16) {
        *r = basic[idx][0]; *g = basic[idx][1]; *b = basic[idx][2];
        return;
    }
    if (idx < 232) {
        static const uint8_t levels[6] = {0, 95, 135, 175, 215, 255};
        int i = idx - 16;
        *r = levels[(i / 36) % 6];
        *g = levels[(i / 6) % 6];
        *b = levels[i % 6];
        return;
    }
    uint8_t gray = (uint8_t)(8 + (idx - 232) * 10);
    *r = *g = *b = gray;
}

void init_terminal_state(TerminalState *state) {
    state->current_state = STATE_GROUND;
    state->param_count = 0;
    state->current_param_value = 0;
    for (int i = 0; i<MAX_ANSI_PARAMS; i++) {
        state->params[i] = 0;
    }
    state->saved_cursor_y = 0;   
    state->saved_cursor_x = 0;   

    state->scrollback_head = 0;
    state->scrollback_count = 0;
    state->scroll_offset = 0;
    memset(state->scrollback, 0, sizeof(state->scrollback));
}


void execute_csi_sequence(TerminalState *state, char final_byte, struct ncplane *pane) {
    if (final_byte == 'm') { 
        if (state->param_count == 0) {
            state->params[0] = 0;
            state->param_count = 1;
        }

        for (int i = 0; i < state->param_count; i++) {

            if (state->params[i]== 38 || state->params[i]== 48) {                       // NEW: extended color
                int is_fg = (state->params[i]== 38);
                if (i + 1 < state->param_count && state->params[i+1] == 5 && i + 2 < state->param_count) {
                    uint8_t r, g, b;
                    ansi256_to_rgb(state->params[i+2], &r, &g, &b);
                    if (is_fg) ncplane_set_fg_rgb8(pane, r, g, b);
                    else       ncplane_set_bg_rgb8(pane, r, g, b);
                    i += 2;
                } else if (i + 1 < state->param_count && state->params[i+1] == 2 && i + 4 < state->param_count) {
                    int r = state->params[i+2], g = state->params[i+3], b = state->params[i+4];
                    if (is_fg) ncplane_set_fg_rgb8(pane, r, g, b);
                    else       ncplane_set_bg_rgb8(pane, r, g, b);
                    i += 4;
                }
                continue;
            }

            switch (state->params[i]) {
                case 0:  // Reset all formatting
                    ncplane_set_fg_rgb8(pane, 220, 220, 220);
                    ncplane_set_bg_rgb8(pane, 0, 0, 0);
                    break;
                case 30: ncplane_set_fg_rgb8(pane, 0, 0, 0); break;       // Black
                case 31: ncplane_set_fg_rgb8(pane, 170, 0, 0); break;     // Red
                case 32: ncplane_set_fg_rgb8(pane, 0, 170, 0); break;     // Green
                case 33: ncplane_set_fg_rgb8(pane, 170, 85, 0); break;    // Yellow
                case 34: ncplane_set_fg_rgb8(pane, 0, 0, 170); break;     // Blue
                case 35: ncplane_set_fg_rgb8(pane, 170, 0, 170); break;   // Magenta
                case 36: ncplane_set_fg_rgb8(pane, 0, 170, 170); break;   // Cyan
                case 37: ncplane_set_fg_rgb8(pane, 170, 170, 170); break; // White

                // --- STANDARD BACKGROUND (40-47) ---
                case 40: ncplane_set_bg_rgb8(pane, 0, 0, 0); break;       // Black
                case 41: ncplane_set_bg_rgb8(pane, 170, 0, 0); break;     // Red
                case 42: ncplane_set_bg_rgb8(pane, 0, 170, 0); break;     // Green
                case 43: ncplane_set_bg_rgb8(pane, 170, 85, 0); break;    // Yellow
                case 44: ncplane_set_bg_rgb8(pane, 0, 0, 170); break;     // Blue
                case 45: ncplane_set_bg_rgb8(pane, 170, 0, 170); break;   // Magenta
                case 46: ncplane_set_bg_rgb8(pane, 0, 170, 170); break;   // Cyan
                case 47: ncplane_set_bg_rgb8(pane, 170, 170, 170); break; // White

                // --- BRIGHT FOREGROUND (90-97) ---
                case 90: ncplane_set_fg_rgb8(pane, 85, 85, 85); break;    // Bright Black (Gray)
                case 91: ncplane_set_fg_rgb8(pane, 255, 85, 85); break;   // Bright Red
                case 92: ncplane_set_fg_rgb8(pane, 85, 255, 85); break;   // Bright Green
                case 93: ncplane_set_fg_rgb8(pane, 255, 255, 85); break;  // Bright Yellow
                case 94: ncplane_set_fg_rgb8(pane, 85, 85, 255); break;   // Bright Blue
                case 95: ncplane_set_fg_rgb8(pane, 255, 85, 255); break;  // Bright Magenta
                case 96: ncplane_set_fg_rgb8(pane, 85, 255, 255); break;  // Bright Cyan
                case 97: ncplane_set_fg_rgb8(pane, 255, 255, 255); break; // Bright White

                // --- BRIGHT BACKGROUND (100-107) ---
                case 100: ncplane_set_bg_rgb8(pane, 85, 85, 85); break;   // Bright Black (Gray)
                case 101: ncplane_set_bg_rgb8(pane, 255, 85, 85); break;  // Bright Red
                case 102: ncplane_set_bg_rgb8(pane, 85, 255, 85); break;  // Bright Green
                case 103: ncplane_set_bg_rgb8(pane, 255, 255, 85); break; // Bright Yellow
                case 104: ncplane_set_bg_rgb8(pane, 85, 85, 255); break;  // Bright Blue
                case 105: ncplane_set_bg_rgb8(pane, 255, 85, 255); break; // Bright Magenta
                case 106: ncplane_set_bg_rgb8(pane, 85, 255, 255); break; // Bright Cyan
                case 107: ncplane_set_bg_rgb8(pane, 255, 255, 255); break;// Bright White
                
                // --- DEFAULT COLORS (39, 49) ---
                case 39: ncplane_set_fg_default(pane); break;             // Default Foreground
                case 49: ncplane_set_bg_default(pane); break;             // Default Background
            }
        }
    } else if (final_byte == 's') {   // Save cursor (CSI form)
        unsigned int y, x;
        ncplane_cursor_yx(pane, &y, &x);
        state->saved_cursor_y = (int)y;
        state->saved_cursor_x = (int)x;
    }
    else if (final_byte == 'u') {   // Restore cursor (CSI form)
        ncplane_cursor_move_yx(pane, state->saved_cursor_y, state->saved_cursor_x);
    }
    else if (final_byte == 'H') {
    // CUP (Cursor Position)
        int row = (state->param_count>0 && state->params[0]>0)? state->params[0]:1;
        int col = (state->param_count>1 && state->params[1]>0)? state->params[1]:1;
        ncplane_cursor_move_yx(pane, row - 1, col - 1);
    }
    else if (final_byte == 'J') {
        int param = (state->param_count > 0) ? state->params[0] : 0; 
        unsigned int curY, curX, dimY, dimX;
        ncplane_cursor_yx(pane, &curY, &curX); 
        ncplane_dim_yx(pane, &dimY, &dimX);

        if (param == 0) { 
            ncplane_erase_region(pane, curY, curX, dimY - curY, dimX - curX);
        }
        else if (param == 1) { 
            ncplane_erase_region(pane, 0, 0, curY, curX);
        }
        else if (param == 2 || param == 3) { 
            ncplane_erase(pane);
            ncplane_cursor_move_yx(pane, 0, 0);
        }
    }
    else if (final_byte=='K'){
        int param = (state->param_count>0)? state->params[0]: 0; 
        unsigned int curY, curX, dimY, dimX; 
        
        ncplane_cursor_yx(pane, &curY, &curX); 
        ncplane_dim_yx(pane, &dimY, &dimX); 

        if(param==0){
            for(unsigned int i=curX; i<=curX && i<dimX; i++){
                ncplane_putchar_yx(pane, curY, i, ' '); 
            }
        }
        else if(param==1){
            for(unsigned int i=0; i<dimX; i++){
                ncplane_putchar_yx(pane, curY, i, ' '); 
            }
        }
        else if(param==2){
            for(unsigned int i=0; i<dimX; i++){
                ncplane_putchar_yx(pane, curY, i, ' '); 
            }
        }
        ncplane_cursor_move_yx(pane, curY, curX); 
        }
    else if (final_byte == 'A') {  // Cursor Up
        int n = (state->param_count > 0 && state->params[0] > 0) ? state->params[0] : 1;
        unsigned int y, x;
        ncplane_cursor_yx(pane, &y, &x);
        ncplane_cursor_move_yx(pane, (y >= (unsigned)n) ? y - n : 0, x);
    }
    else if (final_byte == 'B') {  // Cursor Down
        int n = (state->param_count > 0 && state->params[0] > 0) ? state->params[0] : 1;
        unsigned int y, x, dimy, dimx;
        ncplane_cursor_yx(pane, &y, &x);
        ncplane_dim_yx(pane, &dimy, &dimx);
        ncplane_cursor_move_yx(pane, (y + n < dimy) ? y + n : dimy - 1, x);
    }
    else if (final_byte == 'C') {  // Cursor Right
        int n = (state->param_count > 0 && state->params[0] > 0) ? state->params[0] : 1;
        unsigned int y, x, dimy, dimx;
        ncplane_cursor_yx(pane, &y, &x);
        ncplane_dim_yx(pane, &dimy, &dimx);
        ncplane_cursor_move_yx(pane, y, (x + n < dimx) ? x + n : dimx - 1);
    }
    else if (final_byte == 'D') {  // Cursor Left
        int n = (state->param_count > 0 && state->params[0] > 0) ? state->params[0] : 1;
        unsigned int y, x;
        ncplane_cursor_yx(pane, &y, &x);
        ncplane_cursor_move_yx(pane, y, (x >= (unsigned)n) ? x - n : 0);
    }
    else if (final_byte == 'G') {  // Cursor Horizontal Absolute
        int col = (state->param_count > 0 && state->params[0] > 0) ? state->params[0] - 1 : 0;
        unsigned int y, x;
        ncplane_cursor_yx(pane, &y, &x);
        ncplane_cursor_move_yx(pane, y, col);
    }
}


void parse_ansi_byte(TerminalState *state, char ch, struct ncplane *pane) {
    switch (state->current_state) {
        case STATE_GROUND:
            if (ch == '\x1B') { 
                state->current_state = STATE_ESCAPE;
            } else if (ch == '\r') {    // cursor to column 0 of the current row
                unsigned int y, x;
                ncplane_cursor_yx(pane, &y, &x);
                ncplane_cursor_move_yx(pane, y, 0);
            } else if (ch == '\n') {    // Move cursor down one row
                    unsigned int y, x, dimy, dimx;
                    ncplane_cursor_yx(pane, &y, &x);
                    ncplane_dim_yx(pane, &dimy, &dimx);

                    if (y == dimy - 1) {
                        // This newline is about to trigger a scroll -- save row 0 first
                        char *dst = state->scrollback[(state->scrollback_head + state->scrollback_count) % SCROLLBACK_MAX_LINES];
                        unsigned int cols_to_copy = (dimx < SCROLLBACK_LINE_WIDTH - 1) ? dimx : SCROLLBACK_LINE_WIDTH - 1;
                        for (unsigned int col = 0; col < cols_to_copy; col++) {
                            uint32_t egc_cell = 0; // placeholder; use ncplane_at_yx below
                            (void)egc_cell;
                            char *egc = ncplane_at_yx(pane, 0, col, NULL, NULL);
                            dst[col] = (egc && egc[0]) ? egc[0] : ' ';
                            if (egc) free(egc);
                        }
                        dst[cols_to_copy] = '\0';

                        if (state->scrollback_count < SCROLLBACK_MAX_LINES) {
                            state->scrollback_count++;
                        } else {
                            state->scrollback_head = (state->scrollback_head + 1) % SCROLLBACK_MAX_LINES;
                        }
                    }
                    ncplane_putchar(pane, '\n');

            } 
            else if (ch == '\b') {    // Move cursor left one column
                unsigned int y, x;
                ncplane_cursor_yx(pane, &y, &x);
                if (x > 0) ncplane_cursor_move_yx(pane, y, x - 1);
            } 
            else if (ch == '\t') {
                unsigned int y, x, dimy, dimx;
                ncplane_cursor_yx(pane, &y, &x);
                ncplane_dim_yx(pane, &dimy, &dimx);
                unsigned int next_tab = (x + 8) & ~7;  // next tab stop (every 8 cols)
                if (next_tab < dimx) {
                    ncplane_cursor_move_yx(pane, y, next_tab);
                }
            }
            else {
                ncplane_putchar(pane, ch);
            }
            break;

        case STATE_ESCAPE:
            if (ch == '[') {
                // Control Sequence Introducer (CSI)
                state->current_state = STATE_CSI_ENTRY;
                state->param_count = 0;
                state->current_param_value = 0;
                for (int i = 0; i < MAX_ANSI_PARAMS; i++) state->params[i] = 0;
            } else if(ch == ']'){
                state->current_state = STATE_OSC; 
            } else if (ch == '7') {                         
                    unsigned int y, x;
                    ncplane_cursor_yx(pane, &y, &x);
                    state->saved_cursor_y = (int)y;
                    state->saved_cursor_x = (int)x;
                    state->current_state = STATE_GROUND;
            } else if (ch == '8') {                         
                    ncplane_cursor_move_yx(pane, state->saved_cursor_y, state->saved_cursor_x);
                    state->current_state = STATE_GROUND;
            } else{
                // CSI sequence (could be OSC, DCS, etc. which are more complex)
                // For a basic multiplexer, drop it and return to ground
                state->current_state = STATE_GROUND;
            }
            break;

        case STATE_CSI_ENTRY:
        case STATE_CSI_PARAM:
            if (ch >= '0' && ch <= '9') {
                state->current_state = STATE_CSI_PARAM;
                state->current_param_value = (state->current_param_value * 10) + (ch - '0');
            } 
            else if (ch == ';') {
                if (state->param_count < MAX_ANSI_PARAMS) {
                    state->params[state->param_count++] = state->current_param_value;
                }
                state->current_param_value = 0; 
                state->current_state = STATE_CSI_PARAM;
            } 
            else if (ch >= 0x40 && ch <= 0x7E) {
                if (state->param_count < MAX_ANSI_PARAMS) {
                    state->params[state->param_count++] = state->current_param_value;
                }
                
                execute_csi_sequence(state, ch, pane);
                state->current_state = STATE_GROUND;
            } 
            else if (ch == ':') {
                if (state->param_count < MAX_ANSI_PARAMS) {
                    state->params[state->param_count++] = state->current_param_value;
                }
                state->current_param_value = 0;
                state->current_state = STATE_CSI_PARAM;
            }
            else if (ch == '?' || ch == '>' || ch == '<' || ch == '=') {
                
            }
            else {
                state->current_state = STATE_GROUND;
            }
            break;
        case STATE_OSC:
            if (ch == '\007') {
                state->current_state = STATE_GROUND;
            } else if (ch == '\x1B') {
                state->current_state = STATE_OSC_ESCAPE;
            }
            break;
        case STATE_OSC_ESCAPE:
            state->current_state = (ch == '\\') ? STATE_GROUND : STATE_OSC;
            break;
        

    }
}