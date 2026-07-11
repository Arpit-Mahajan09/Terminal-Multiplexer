#ifndef CRONOS_H
#define CRONOS_H

#define MAX_EVENTS 5
#define BUFFER_SIZE 256
#define PENDING_BUF_SIZE 65536

#define MAX_KEYBINDINGS 64
#define MAX_ANSI_PARAMS 16
#define SCROLLBACK_MAX_LINES 2000
#define SCROLLBACK_LINE_WIDTH 256

#define PKT_TYPE_KEYSTROKE  0
#define PKT_TYPE_PTY_OUTPUT 1
#define PKT_TYPE_COMMAND    2

#define REQ_SPLIT_VERT       1
#define REQ_SPLIT_HORZ       2
#define RES_SPLIT_VERT_SUCC  3
#define RES_SPLIT_HORZ_SUCC  4
#define WINDOW_RESIZE        5
#define RES_PANE_CLOSED 6
#define REQ_CLOSE_PANE 7
#define REQ_NEW_WINDOW       8   
#define RES_NEW_WINDOW_SUCC  9   
#define REQ_RENAME_SESSION   10  


#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef enum {
    ACTION_NONE = 0,
    ACTION_DETACH,
    ACTION_SPLIT_VERT,
    ACTION_SPLIT_HORZ,
    ACTION_FOCUS_LEFT,
    ACTION_FOCUS_RIGHT,
    ACTION_FOCUS_UP,
    ACTION_FOCUS_DOWN,
    ACTION_CLOSE_PANE,
    ACTION_RESIZE_LEFT,
    ACTION_RESIZE_RIGHT,
    ACTION_RESIZE_UP,
    ACTION_RESIZE_DOWN,
    ACTION_SCROLL_UP,
    ACTION_SCROLL_DOWN,
    ACTION_SCROLL_RESET,
    ACTION_NEW_WINDOW,
    ACTION_NEXT_WINDOW,
    ACTION_PREV_WINDOW,
    ACTION_RENAME_WINDOW,
    ACTION_SESSION_MENU,
} Action;

typedef enum {
    STATE_GROUND,
    STATE_ESCAPE,
    STATE_CSI_ENTRY,
    STATE_CSI_PARAM, 
    STATE_OSC,         
    STATE_OSC_ESCAPE
} AnsiState;


typedef struct {
    uint8_t fg_r, fg_g, fg_b;
    uint8_t bg_r, bg_g, bg_b;
    uint8_t border_r, border_g, border_b;
} ClientConfig;


typedef struct {
    uint8_t type;      // 0 = Keystroke, 1 = PTY Output, 2 = Command (e.g., "Split")
    int pane_id;       
    size_t data_len;   
    char payload[BUFFER_SIZE]; 
} CronosPacket;



typedef struct {
    AnsiState current_state;
    int params[MAX_ANSI_PARAMS]; 
    int param_count;
    int current_param_value;     

    int saved_cursor_y;      
    int saved_cursor_x;

    char scrollback[SCROLLBACK_MAX_LINES][SCROLLBACK_LINE_WIDTH];
    int scrollback_head;    
    int scrollback_count;   
    int scroll_offset;      
} TerminalState;


typedef struct Pane {
    int id;
    int pty_master_fd;         
    pid_t shell_pid;           
    TerminalState state;       
    
    struct ncplane *nc_plane;  
    struct ncplane *border_plane; 
    struct ncplane *scroll_overlay;
    
    int width, height, y, x;
    int border_y, border_x;

    char pending_buf[PENDING_BUF_SIZE];
    size_t pending_len;


    struct Pane *next;         
} Pane;

typedef struct Window {
    int id;
    char name[32];
    Pane *pane_list_head;
    Pane *active_pane;
    struct Window *next;
} Window;


typedef struct {
    uint32_t key;     
    int needs_ctrl;
    int needs_alt;
    Action action;
} KeyBinding;

typedef struct {
    struct notcurses *nc;
    struct ncplane *std;
    struct ncplane *footer;

    Window *window_list_head;
    Window *active_window;
    int next_window_id;

    Pane *pane_list_head;
    Pane *active_pane;

    int client_sock;
    char session_name[128];
    int session_count;
} ClientContext;

void send_resize_packet(int client_sock, int pane_id, int rows, int cols);
Pane *find_pane_global(ClientContext *ctx, int pane_id);


#endif