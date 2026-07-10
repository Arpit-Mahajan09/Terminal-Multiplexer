#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <notcurses/notcurses.h>


#define MAX_ANSI_PARAMS 16
// This file acts as a state machine that would read the ANSI characters from server 
// before sending them to the frontend

typedef enum {
    STATE_GROUND,
    STATE_ESCAPE,
    STATE_CSI_ENTRY,
    STATE_CSI_PARAM, 
    STATE_OSC,         
    STATE_OSC_ESCAPE
} AnsiState;

typedef struct {
    AnsiState current_state;
    int params[MAX_ANSI_PARAMS]; 
    int param_count;
    int current_param_value;     
} TerminalState;


void init_terminal_state(TerminalState *state) {
    state->current_state = STATE_GROUND;
    state->param_count = 0;
    state->current_param_value = 0;
    for (int i = 0; i<MAX_ANSI_PARAMS; i++) {
        state->params[i] = 0;
    }
}


void execute_csi_sequence(TerminalState *state, char final_byte, struct ncplane *pane) {
    if (final_byte == 'm') { 
        if (state->param_count == 0) {
            state->params[0] = 0;
            state->param_count = 1;
        }

        for (int i = 0; i < state->param_count; i++) {
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
    } else if (final_byte == 'H') {
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
            for(unsigned int i=curX; i<dimX; i++){
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
                ncplane_putchar(pane, '\n');
            } else if (ch == '\b') {    // Move cursor left one column
                unsigned int y, x;
                ncplane_cursor_yx(pane, &y, &x);
                if (x > 0) ncplane_cursor_move_yx(pane, y, x - 1);
            } else {
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
            }
            else{
                // CSI sequence (could be OSC, DCS, etc. which are more complex)
                // For a basic multiplexer, drop it and return to ground
                state->current_state = STATE_GROUND;
            }
            break;

        case STATE_CSI_ENTRY:
        case STATE_CSI_PARAM:
            if (ch >= '0' && ch <= '9') {
                // Build the numeric parameter
                state->current_state = STATE_CSI_PARAM;
                state->current_param_value = (state->current_param_value * 10) + (ch - '0');
            } 
            else if (ch == ';') {
                // Parameter separator, save the current param and move to the next
                if (state->param_count < MAX_ANSI_PARAMS) {
                    state->params[state->param_count++] = state->current_param_value;
                }
                state->current_param_value = 0; // Reset for the next parameter
                state->current_state = STATE_CSI_PARAM;
            } 
            else if (ch >= 0x40 && ch <= 0x7E) {
                // Final character (a-z, A-Z). This terminates the sequence.
                // Save the final parameter first
                if (state->param_count < MAX_ANSI_PARAMS) {
                    state->params[state->param_count++] = state->current_param_value;
                }
                
                // Execute the accumulated sequence
                execute_csi_sequence(state, ch, pane);
                
                // Return to normal text processing
                state->current_state = STATE_GROUND;
            } 
            else if (ch == ':') {
                // Kitty protocol sub-parameter separator (e.g. "1:3" in "5:3u")
                // Save current value, discard the sub-value that follows
                if (state->param_count < MAX_ANSI_PARAMS) {
                    state->params[state->param_count++] = state->current_param_value;
                }
                state->current_param_value = 0;
                state->current_state = STATE_CSI_PARAM;
            }
            else if (ch == '?' || ch == '>' || ch == '<' || ch == '=') {
                //stay in param state
            }
            else {
                // Unexpected character (e.g., C0 control character in the middle of a sequence)
                // Robust parsers handle this differently, but returning to ground is a safe fallback
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