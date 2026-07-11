#include <notcurses/notcurses.h>
#include "parser.c"

#define PENDING_BUF_SIZE 65536
#define MAX_KEYBINDINGS 64


KeyBinding keymap[MAX_KEYBINDINGS];
int keymap_count = 0;


Action resolve_action(uint32_t id, ncinput *ni) {
    for (int i = 0; i < keymap_count; i++) {
        if (keymap[i].key == id &&
            keymap[i].needs_ctrl == ni->ctrl &&
            keymap[i].needs_alt == ni->alt) {
            return keymap[i].action;
        }
    }
    return ACTION_NONE;
}


void print_usage(char *prog){
    printf("Cronos Terminal Multiplexer CLI\n\n");
    printf("Usage:\n");
    printf("  %s new <name>      - Create and attach to a new background session\n", prog);
    printf("  %s attach <name>   - Attach to an existing active session\n", prog);
    printf("  %s list            - List all active background sessions\n", prog);
    printf("  %s kill <name>     - Kill an active background session\n", prog);
    printf("  %s killall         - Kill all background sessions\n", prog); 
    printf("  %s --help, -h      - Display this help menu\n", prog);
    printf("  %s --inst, -i      - Display shortcuts for tmux plane handling\n\n", prog);
    printf("Example:\n");
    printf("  cronos new dev_env\n");
}

void print_short(char *prog){
    printf("Cronos Terminal Multiplexer CLI\n\n");
    printf("Shortcut Keys: \n");
    
    printf("  Split Vertically       :  Ctrl + v\n");
    printf("  Split Horizontally     :  Ctrl + b\n");
    printf("  Move Focus Right       :  Ctrl + h\n");
    printf("  Move Focus Above       :  Ctrl + j\n"); 
    printf("  Move Focus Below       :  Ctrl + k\n\n");

    printf("  Resize Pane            :  Alt + Arrow Keys\n");
    printf("  Scroll Pane History    :  Page Up / Page Down\n");
    printf("  Detach Session         :  Ctrl + q\n\n");

    printf("  Close Single Pnae      :  Ctrl + w\n");
    printf("  Close Terminal       :  Ctrl + q\n\n");
}

void render_scrollback_view(Pane *p) {
    TerminalState *st = &p->state;
    ncplane_erase(p->nc_plane);

    int total_lines = st->scrollback_count;
    int visible = p->height;
    int start = total_lines - visible - st->scroll_offset;
    if (start < 0) start = 0;

    for (int row = 0; row < visible; row++) {
        int line_idx = start + row;
        if (line_idx >= total_lines) break;
        char *line = st->scrollback[(st->scrollback_head + line_idx) % SCROLLBACK_MAX_LINES];
        ncplane_putstr_yx(p->nc_plane, row, 0, line);
    }
}

void exit_scrollback(Pane *p) {
    p->state.scroll_offset = 0;
    ncplane_erase(p->nc_plane);
    ncplane_cursor_move_yx(p->nc_plane, 0, 0);

    for (size_t j = 0; j < p->pending_len; j++) {
        parse_ansi_byte(&p->state, p->pending_buf[j], p->nc_plane);
    }
    p->pending_len = 0;
}


void bind_key(uint32_t key, int ctrl, int alt, Action action) {
    if (keymap_count >= MAX_KEYBINDINGS) return;
    keymap[keymap_count].key = key;
    keymap[keymap_count].needs_ctrl = ctrl;
    keymap[keymap_count].needs_alt = alt;
    keymap[keymap_count].action = action;
    keymap_count++;
}

void load_default_keymap(void) {
    bind_key('q', 1, 0, ACTION_DETACH);      bind_key('Q', 1, 0, ACTION_DETACH);
    bind_key('v', 1, 0, ACTION_SPLIT_VERT);  bind_key('V', 1, 0, ACTION_SPLIT_VERT);
    bind_key('b', 1, 0, ACTION_SPLIT_HORZ);  bind_key('B', 1, 0, ACTION_SPLIT_HORZ);
    bind_key('h', 1, 0, ACTION_FOCUS_LEFT);  bind_key('H', 1, 0, ACTION_FOCUS_LEFT);
    bind_key('l', 1, 0, ACTION_FOCUS_RIGHT); bind_key('L', 1, 0, ACTION_FOCUS_RIGHT);
    bind_key('j', 1, 0, ACTION_FOCUS_DOWN);  bind_key('J', 1, 0, ACTION_FOCUS_DOWN);
    bind_key('k', 1, 0, ACTION_FOCUS_UP);    bind_key('K', 1, 0, ACTION_FOCUS_UP);
    bind_key('w', 1, 0, ACTION_CLOSE_PANE);  bind_key('W', 1, 0, ACTION_CLOSE_PANE);
    bind_key(NCKEY_LEFT,  0, 1, ACTION_RESIZE_LEFT);
    bind_key(NCKEY_RIGHT, 0, 1, ACTION_RESIZE_RIGHT);
    bind_key(NCKEY_UP,    0, 1, ACTION_RESIZE_UP);
    bind_key(NCKEY_DOWN,  0, 1, ACTION_RESIZE_DOWN);
    bind_key(NCKEY_PGUP, 0, 0, ACTION_SCROLL_UP);
    bind_key(NCKEY_PGDOWN, 0, 0, ACTION_SCROLL_DOWN);

}
int parse_key_name(const char *name, uint32_t *key_out) {
    if (strcmp(name, "left") == 0)  { *key_out = NCKEY_LEFT;  return 1; }
    if (strcmp(name, "right") == 0) { *key_out = NCKEY_RIGHT; return 1; }
    if (strcmp(name, "up") == 0)    { *key_out = NCKEY_UP;    return 1; }
    if (strcmp(name, "down") == 0)  { *key_out = NCKEY_DOWN;  return 1; }
    if (strlen(name) == 1) { *key_out = (uint32_t)name[0]; return 1; }
    return 0;
}

Action parse_action_name(const char *name) {
    if (strcmp(name, "detach") == 0)       return ACTION_DETACH;
    if (strcmp(name, "split_vert") == 0)   return ACTION_SPLIT_VERT;
    if (strcmp(name, "split_horz") == 0)   return ACTION_SPLIT_HORZ;
    if (strcmp(name, "focus_left") == 0)   return ACTION_FOCUS_LEFT;
    if (strcmp(name, "focus_right") == 0)  return ACTION_FOCUS_RIGHT;
    if (strcmp(name, "focus_up") == 0)     return ACTION_FOCUS_UP;
    if (strcmp(name, "focus_down") == 0)   return ACTION_FOCUS_DOWN;
    if (strcmp(name, "close_pane") == 0)   return ACTION_CLOSE_PANE;
    if (strcmp(name, "resize_left") == 0)  return ACTION_RESIZE_LEFT;
    if (strcmp(name, "resize_right") == 0) return ACTION_RESIZE_RIGHT;
    if (strcmp(name, "resize_up") == 0)    return ACTION_RESIZE_UP;
    if (strcmp(name, "resize_down") == 0)  return ACTION_RESIZE_DOWN;
    return ACTION_NONE;
}