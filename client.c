#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <signal.h>
#include <unistd.h>

#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/un.h>

#include <fcntl.h>        
#include <termios.h>
#include <dirent.h>
#include <locale.h>
#include <errno.h> 
#include <notcurses/notcurses.h>

#include "parser.c"
#include "cronos.h"

typedef struct Pane {
    int id;
    int pty_master_fd;         
    pid_t shell_pid;           
    TerminalState state;       
    
    struct ncplane *nc_plane;  
    struct ncplane *border_plane; // NEW: Track the border!
    int width;
    int height;
    int y;
    int x;

    struct Pane *next;         
} Pane;

Pane *pane_list_head = NULL;


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
} Action;

typedef struct {
    uint32_t key;     
    int needs_ctrl;
    int needs_alt;
    Action action;
} KeyBinding;

typedef struct {
    uint8_t fg_r, fg_g, fg_b;
    uint8_t bg_r, bg_g, bg_b;
    uint8_t border_r, border_g, border_b;
} ClientConfig;

ClientConfig g_client_config;


#define MAX_KEYBINDINGS 64
KeyBinding keymap[MAX_KEYBINDINGS];
int keymap_count = 0;

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


void load_client_config(void) {
    g_client_config.fg_r = 220; g_client_config.fg_g = 220; g_client_config.fg_b = 220;
    g_client_config.bg_r = 0;   g_client_config.bg_g = 0;   g_client_config.bg_b = 0;
    g_client_config.border_r = 100; g_client_config.border_g = 100; g_client_config.border_b = 100;

    char path[512];
    const char *home = getenv("HOME");
    if (!home) return;
    snprintf(path, sizeof(path), "%s/.config/cronos/cronos.conf", home);

    FILE *f = fopen(path, "r");
    if (!f) return; // defaults stand

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == '#' || line[0] == '\0') continue;

        int r, g, b;
        char lhs[64], rhs[64];

        if (sscanf(line, "color_fg = %d,%d,%d", &r, &g, &b) == 3) {
            g_client_config.fg_r = r; g_client_config.fg_g = g; g_client_config.fg_b = b;
        }
        else if (sscanf(line, "color_bg = %d,%d,%d", &r, &g, &b) == 3) {
            g_client_config.bg_r = r; g_client_config.bg_g = g; g_client_config.bg_b = b;
        }
        else if (sscanf(line, "border_color = %d,%d,%d", &r, &g, &b) == 3) {
            g_client_config.border_r = r; g_client_config.border_g = g; g_client_config.border_b = b;
        }
        else if (sscanf(line, "bind %63[^=]= %63[^\n]", lhs, rhs) == 2) {
            // trim trailing space left over from "%63[^=]"
            char *end = lhs + strlen(lhs) - 1;
            while (end > lhs && *end == ' ') *end-- = '\0';

            int ctrl = 0, alt = 0;
            char *keypart = lhs;
            if (strncmp(lhs, "ctrl+", 5) == 0) { ctrl = 1; keypart = lhs + 5; }
            else if (strncmp(lhs, "alt+", 4) == 0) { alt = 1; keypart = lhs + 4; }

            uint32_t key;
            char *trimmed_action = rhs;
            while (*trimmed_action == ' ') trimmed_action++;

            Action act = parse_action_name(trimmed_action);
            if (act != ACTION_NONE && parse_key_name(keypart, &key)) {
                bind_key(key, ctrl, alt, act);
            }
        }
    }
    fclose(f);
}


struct notcurses_options opts = { 
    .loglevel = NCLOGLEVEL_SILENT,
    .flags = NCOPTION_SUPPRESS_BANNERS | NCOPTION_NO_QUIT_SIGHANDLERS
};

struct termios orig_t;
int raw_mode_active = 0; 

static struct notcurses *nc_global = NULL;

int count_active_sessions(){
    DIR *dir = opendir("/tmp"); 
    if(!dir) return 0; 

    struct dirent *entry;
    int count = 0;
    
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "cronos_", 7) == 0 && strstr(entry->d_name, ".sock")) {
            count++;
        }
    }
    closedir(dir);
    return count;
}

static void cleanup_handler(int sig) {
    (void)sig;
    if (nc_global) {
        notcurses_stop(nc_global);
        nc_global = NULL;
    }

    if (raw_mode_active) {
        tcsetattr(STDIN_FILENO, TCSANOW, &orig_t);
    }
    write(STDOUT_FILENO, "\033[0 q", 5);
    write(STDOUT_FILENO, "\033[?1049l", 8); // exit alt screen
    write(STDOUT_FILENO, "\033[?25h",   6); // restore cursor
    write(STDOUT_FILENO, "\033[0m",     4); // reset colors
    _exit(0);
}

struct notcurses *nc = NULL;
TerminalState pane_state;   

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
    printf("  Create New Terminal    :  Ctrl + n\n");
    printf("  Split Vertically       :  Ctrl + v\n");
    printf("  Split Horizontally     :  Ctrl + b\n");
    printf("  Move Focus Right       :  Ctrl + h\n");
    printf("  Move Focus Above       :  Ctrl + j\n");
    printf("  Move Focus Below       :  Ctrl + k\n");
    
    printf("  Close Single Pnae      :  Ctrl + w\n");
    printf("  Close Terminal       :  Ctrl + q\n\n");
}


Pane *active_pane = NULL;

void update_active_ui(struct notcurses *nc, struct ncplane *footer, Pane *active_pane, const char *session_name, int session_cnt) {
    if (!footer || !active_pane) return;
    Pane *p = pane_list_head;
    while (p) {
        uint64_t channels = 0;
        ncchannels_set_fg_rgb8(&channels, 220, 220, 220);
        
        if (p == active_pane) {
            ncchannels_set_bg_rgb8(&channels, 30, 30, 50); 
        } else {
            ncchannels_set_bg_rgb8(&channels, 0, 0, 0); 
        }
        ncplane_set_base(p->nc_plane, " ", 0, channels);
        p = p->next;
    }

    ncplane_printf_yx(footer, 0, 1, " Cronos | Session: %s | Active Sessions: %d | [ Active Pane: %d ] ", 
                      session_name, session_cnt, active_pane->id+1);

    ncplane_move_top(footer);
    ncplane_move_top(footer);
    unsigned int curY, curX;
    ncplane_cursor_yx(active_pane->nc_plane, &curY, &curX);
    notcurses_cursor_enable(nc, active_pane->y + curY, active_pane->x + curX);
}



void send_resize_packet(int client_sock, int pane_id, int rows, int cols) {
    CronosPacket resize_pkt;
    memset(&resize_pkt, 0, sizeof(CronosPacket));
    resize_pkt.type = PKT_TYPE_COMMAND;
    resize_pkt.pane_id = pane_id;
    resize_pkt.data_len = 3;
    resize_pkt.payload[0] = WINDOW_RESIZE;
    
    resize_pkt.payload[1] = (unsigned char)(rows > 255 ? 255 : rows); 
    resize_pkt.payload[2] = (unsigned char)(cols > 255 ? 255 : cols); 
    
    write(client_sock, &resize_pkt, sizeof(CronosPacket));
}



int run_session(const char *socket_path, const char *session_name){

    int client_sock = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    if (tcgetattr(STDIN_FILENO, &orig_t) == 0) {
        struct termios raw = orig_t;
        cfmakeraw(&raw);
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        raw_mode_active = 1;
    }
    
    int connected =0; 
    for (int i = 0; i < 20; i++) {
    if (connect(client_sock, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
        connected = 1;
        break;
    }
    usleep(100000);
    }
    if (!connected) {
        perror("Failed to connect to server"); // stderr still normal here
        close(client_sock);
        return 1;
    }

    setlocale(LC_ALL, "");
    nc = notcurses_init(&opts, NULL);
    if (nc == NULL) {
        fprintf(stderr, "\nFatal: Failed to initialize Notcurses.\n");
        if (raw_mode_active) tcsetattr(STDIN_FILENO, TCSANOW, &orig_t);
        return 1;
    }
    nc_global = nc;
    write(STDOUT_FILENO, "\033[1 q", 5);

    signal(SIGINT,  cleanup_handler);
    signal(SIGTERM, cleanup_handler);
    init_terminal_state(&pane_state);

    load_default_keymap();          
    load_client_config();

    int epoll_fd = epoll_create1(0);

    struct ncplane *std = notcurses_stdplane(nc); 

    ncplane_set_scrolling(std, true);
    struct epoll_event ev, events[MAX_EVENTS];

    int nc_fd = notcurses_inputready_fd(nc); 
    ev.events = EPOLLIN; ev.data.fd = nc_fd; 
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, nc_fd, &ev);
    
    ev.events = EPOLLIN; ev.data.fd = client_sock; 
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_sock, &ev);
    
    unsigned int dimy, dimx;
    ncplane_dim_yx(std, &dimy, &dimx);

    struct ncplane_options fopts = {
        .y = dimy - 1,
        .x = 0,
        .rows = 1,
        .cols = dimx,
        .flags = 0,
    };
    struct ncplane *footer = ncplane_create(std, &fopts);
    
    uint64_t footer_channels = 0;
    ncchannels_set_fg_rgb8(&footer_channels, 255, 255, 255);
    ncchannels_set_bg_rgb8(&footer_channels, 40, 40, 200);
    ncplane_set_base(footer, " ", 0, footer_channels); 

    int session_count = count_active_sessions();
    ncplane_printf_yx(footer, 0, 1, " Cronos Multiplexer | Session: %s | Active Sessions: %d ", session_name, session_count);

    ncplane_resize(std, 0, 0, dimy - 1, dimx, 0, 0, dimy - 1, dimx);
    ncplane_set_scrolling(std, true);

    Pane *root_pane = malloc(sizeof(Pane));
    root_pane->border_plane = NULL;
    root_pane->id = 0;
    root_pane->nc_plane = std;
    root_pane->height = dimy - 1; 
    root_pane->width = dimx;
    root_pane->y = 0;
    root_pane->x = 0;
    root_pane->next = NULL;
    root_pane->state = pane_state;

    active_pane = root_pane;
    pane_list_head = root_pane; 

    ncplane_set_fg_rgb8(std, 220, 220, 220);   
    send_resize_packet(client_sock, root_pane->id, root_pane->height, root_pane->width);
    int running = 1; 

    while (running) {
        int num_events = epoll_wait(epoll_fd, events, MAX_EVENTS, -1); 

        for (int i = 0; i < num_events; i++) {
            char buffer[BUFFER_SIZE]; 
            ncinput ni;
            uint32_t id;

            if (events[i].data.fd == nc_fd && (events[i].events & EPOLLIN)) {  
                
                while((id = notcurses_get_nblock(nc, &ni))!=0){
                    if(id == (uint32_t)-1) break; 
                    if (ni.evtype == NCTYPE_RELEASE) {
                        continue;
                    }
                    FILE *idbg = fopen("/tmp/cronos_input.log", "a");
                    if (idbg) { fprintf(idbg, "id=0x%x alt=%d ctrl=%d shift=%d\n", id, ni.alt, ni.ctrl, ni.shift); fclose(idbg); }

                     
                    Action act = resolve_action(id, &ni);
                    if (act != ACTION_NONE) {
                        switch (act) {
                            case ACTION_DETACH:
                                printf("\r\n[Client Detached]\r\n");
                                running = 0;
                                break;

                            case ACTION_SPLIT_VERT: {
                                CronosPacket pkt;
                                memset(&pkt, 0, sizeof(CronosPacket));
                                pkt.type = PKT_TYPE_COMMAND;
                                pkt.pane_id = active_pane ? active_pane->id : 0;
                                pkt.data_len = 1;
                                pkt.payload[0] = REQ_SPLIT_VERT;
                                write(client_sock, &pkt, sizeof(CronosPacket));
                                break;
                            }

                            case ACTION_SPLIT_HORZ: {
                                CronosPacket pkt;
                                memset(&pkt, 0, sizeof(CronosPacket));
                                pkt.type = PKT_TYPE_COMMAND;
                                pkt.pane_id = active_pane ? active_pane->id : 0;
                                pkt.data_len = 1;
                                pkt.payload[0] = REQ_SPLIT_HORZ;
                                write(client_sock, &pkt, sizeof(CronosPacket));
                                break;
                            }

                            case ACTION_CLOSE_PANE:
                                if (active_pane && active_pane != pane_list_head) {
                                    CronosPacket pkt;
                                    memset(&pkt, 0, sizeof(CronosPacket));
                                    pkt.type = PKT_TYPE_COMMAND;
                                    pkt.pane_id = active_pane->id;
                                    pkt.data_len = 1;
                                    pkt.payload[0] = REQ_CLOSE_PANE;
                                    write(client_sock, &pkt, sizeof(CronosPacket));
                                }
                                break;

                            case ACTION_FOCUS_LEFT: {
                                Pane *p = pane_list_head;
                                while (p) {
                                    if (p->x + p->width + 1 == active_pane->x &&
                                        active_pane->y >= p->y && active_pane->y < p->y + p->height) {
                                        active_pane = p;
                                        break;
                                    }
                                    p = p->next;
                                }
                                update_active_ui(nc, footer, active_pane, session_name, session_count);
                                notcurses_render(nc);
                                break;
                            }

                            case ACTION_FOCUS_RIGHT: {
                                Pane *p = pane_list_head;
                                while (p) {
                                    if (active_pane->x + active_pane->width + 1 == p->x &&
                                        active_pane->y >= p->y && active_pane->y < p->y + p->height) {
                                        active_pane = p;
                                        break;
                                    }
                                    p = p->next;
                                }
                                update_active_ui(nc, footer, active_pane, session_name, session_count);
                                notcurses_render(nc);
                                break;
                            }

                            case ACTION_FOCUS_DOWN: {
                                Pane *p = pane_list_head;
                                while (p) {
                                    if (p->y + p->height + 1 == active_pane->y &&
                                        active_pane->x >= p->x && active_pane->x < p->x + p->width) {
                                        active_pane = p;
                                        break;
                                    }
                                    p = p->next;
                                }
                                update_active_ui(nc, footer, active_pane, session_name, session_count);
                                notcurses_render(nc);
                                break;
                            }

                            case ACTION_FOCUS_UP: {
                                Pane *p = pane_list_head;
                                while (p) {
                                    if (active_pane->y + active_pane->height + 1 == p->y &&
                                        active_pane->x >= p->x && active_pane->x < p->x + p->width) {
                                        active_pane = p;
                                        break;
                                    }
                                    p = p->next;
                                }
                                update_active_ui(nc, footer, active_pane, session_name, session_count);
                                notcurses_render(nc);
                                break;
                            }

                            case ACTION_RESIZE_RIGHT: {
                                Pane *neighbor = pane_list_head;
                                while (neighbor) {
                                    if (neighbor->x == active_pane->x + active_pane->width + 1 && neighbor->y == active_pane->y) {
                                        if (neighbor->width > 4) {
                                            neighbor->x += 1;
                                            neighbor->width -= 1;
                                            ncplane_move_yx(neighbor->nc_plane, neighbor->y, neighbor->x);
                                            ncplane_resize(neighbor->nc_plane, 0, 0, neighbor->height, neighbor->width, 0, 0, neighbor->height, neighbor->width);
                                            send_resize_packet(client_sock, neighbor->id, neighbor->height, neighbor->width);

                                            active_pane->width += 1;
                                            ncplane_resize(active_pane->nc_plane, 0, 0, active_pane->height, active_pane->width, 0, 0, active_pane->height, active_pane->width);
                                            send_resize_packet(client_sock, active_pane->id, active_pane->height, active_pane->width);

                                            if (neighbor->border_plane) {
                                                ncplane_move_yx(neighbor->border_plane, neighbor->y, active_pane->x + active_pane->width);
                                            }
                                        }
                                        break;
                                    }
                                    neighbor = neighbor->next;
                                }
                                notcurses_render(nc);
                                break;
                            }

                            case ACTION_RESIZE_LEFT: {
                                Pane *neighbor = pane_list_head;
                                while (neighbor) {
                                    if (neighbor->x + neighbor->width + 1 == active_pane->x && neighbor->y == active_pane->y) {
                                        if (active_pane->width > 4) {
                                            neighbor->width += 1;
                                            ncplane_resize(neighbor->nc_plane, 0, 0, neighbor->height, neighbor->width, 0, 0, neighbor->height, neighbor->width);
                                            send_resize_packet(client_sock, neighbor->id, neighbor->height, neighbor->width);

                                            active_pane->x += 1;
                                            active_pane->width -= 1;
                                            ncplane_move_yx(active_pane->nc_plane, active_pane->y, active_pane->x);
                                            ncplane_resize(active_pane->nc_plane, 0, 0, active_pane->height, active_pane->width, 0, 0, active_pane->height, active_pane->width);
                                            send_resize_packet(client_sock, active_pane->id, active_pane->height, active_pane->width);

                                            if (active_pane->border_plane) {
                                                ncplane_move_yx(active_pane->border_plane, active_pane->y, active_pane->x - 1);
                                            }
                                        }
                                        break;
                                    }
                                    neighbor = neighbor->next;
                                }
                                notcurses_render(nc);
                                break;
                            }

                            case ACTION_RESIZE_DOWN: {
                                Pane *neighbor = pane_list_head;
                                while (neighbor) {
                                    if (neighbor->y == active_pane->y + active_pane->height + 1 && neighbor->x == active_pane->x) {
                                        if (neighbor->height > 4) {
                                            neighbor->y += 1;
                                            neighbor->height -= 1;
                                            ncplane_move_yx(neighbor->nc_plane, neighbor->y, neighbor->x);
                                            ncplane_resize(neighbor->nc_plane, 0, 0, neighbor->height, neighbor->width, 0, 0, neighbor->height, neighbor->width);
                                            send_resize_packet(client_sock, neighbor->id, neighbor->height, neighbor->width);

                                            active_pane->height += 1;
                                            ncplane_resize(active_pane->nc_plane, 0, 0, active_pane->height, active_pane->width, 0, 0, active_pane->height, active_pane->width);
                                            send_resize_packet(client_sock, active_pane->id, active_pane->height, active_pane->width);

                                            if (neighbor->border_plane) {
                                                ncplane_move_yx(neighbor->border_plane, active_pane->y + active_pane->height, active_pane->x);
                                            }
                                        }
                                        break;
                                    }
                                    neighbor = neighbor->next;
                                }
                                notcurses_render(nc);
                                break;
                            }

                            case ACTION_RESIZE_UP: {
                                Pane *neighbor = pane_list_head;
                                while (neighbor) {
                                    if (neighbor->y + neighbor->height + 1 == active_pane->y && neighbor->x == active_pane->x) {
                                        if (active_pane->height > 4) {
                                            neighbor->height += 1;
                                            ncplane_resize(neighbor->nc_plane, 0, 0, neighbor->height, neighbor->width, 0, 0, neighbor->height, neighbor->width);
                                            send_resize_packet(client_sock, neighbor->id, neighbor->height, neighbor->width);

                                            active_pane->y += 1;
                                            active_pane->height -= 1;
                                            ncplane_move_yx(active_pane->nc_plane, active_pane->y, active_pane->x);
                                            ncplane_resize(active_pane->nc_plane, 0, 0, active_pane->height, active_pane->width, 0, 0, active_pane->height, active_pane->width);
                                            send_resize_packet(client_sock, active_pane->id, active_pane->height, active_pane->width);

                                            if (active_pane->border_plane) {
                                                ncplane_move_yx(active_pane->border_plane, active_pane->y - 1, active_pane->x);
                                            }
                                        }
                                        break;
                                    }
                                    neighbor = neighbor->next;
                                }
                                notcurses_render(nc);
                                break;
                            }

                            default:
                            break;
                        }
                    continue;
                    }
                }
            }
            else if (events[i].data.fd == client_sock ) {

                // reads data in server 
                if(events[i].events & EPOLLIN){
                    CronosPacket pkt; 
                    ssize_t bytes = recv(client_sock, &pkt, sizeof(CronosPacket), MSG_WAITALL); 
                    FILE *dbg = fopen("/tmp/cronos_client.log", "a");
                    if (dbg) { fprintf(dbg, "RECV type=%d pane=%d len=%zu\n", pkt.type, pkt.pane_id, pkt.data_len); fclose(dbg); }


                    if (bytes == sizeof(CronosPacket)) {
                        if (pkt.type == PKT_TYPE_PTY_OUTPUT) {
                            
                            struct ncplane *target_ncplane = std;      
                            TerminalState *target_state = &pane_state; 
                            
                            Pane *current = pane_list_head; 
                            while (current != NULL) {
                                if (current->id == pkt.pane_id) {
                                    target_ncplane = current->nc_plane;
                                    target_state = &current->state;
                                    break;
                                }
                                current = current->next;
                            }

                            for(size_t j = 0; j < pkt.data_len; j++) {
                                FILE *dbg = fopen("/tmp/cronos_client.log", "a");
                                fprintf(dbg, "pane %d got %zu bytes: %.*s\n", pkt.pane_id, pkt.data_len, (int)pkt.data_len, pkt.payload);
                                fclose(dbg);

                                parse_ansi_byte(target_state, pkt.payload[j], target_ncplane);
                            }

                            update_active_ui(nc, footer, active_pane, session_name, session_count);
                            notcurses_render(nc);
                        }
                        
                        // --- ROUTE 2: Control Commands ---
                        else if (pkt.type == PKT_TYPE_COMMAND) {
                            
                            if (pkt.payload[0] == RES_SPLIT_HORZ_SUCC) {                                
                                Pane *parent = pane_list_head; 
                                
                                while(parent){
                                    if(parent->id == pkt.pane_id) break; 
                                    parent= parent->next; 
                                }

                                if (parent && parent->height > 4) {
                                    int upper_len = parent->height / 2;
                                    int border_y = parent->y + upper_len;
                                    int low_start_y = border_y + 1;
                                    int lower_height = parent->height - upper_len - 1;
                                    
                                    
                                    ncplane_resize(parent->nc_plane, 0, 0,                            
                                                   upper_len, parent->width, 0, 0,  
                                                   upper_len, parent->width);
                                               
                                    parent->height = upper_len;

                                    struct ncplane_options bopts = {
                                        .y = border_y,
                                        .x = parent->x,
                                        .rows = 1,
                                        .cols = parent->width,
                                        .flags = 0,
                                    };
                                    
                                    struct ncplane *border_plane = ncplane_create(std, &bopts);
                                    uint64_t border_channels = 0;
                                    ncchannels_set_fg_rgb8(&border_channels, 100, 100, 100); // Gray
                                    ncplane_set_base(border_plane, "─", 0, border_channels);

                                    struct ncplane_options nopts = {
                                        .y = low_start_y,
                                        .x = parent->x,
                                        .rows = lower_height,
                                        .cols = parent->width,
                                        .flags = 0,
                                    };
                                    
                                    struct ncplane *new_nc_plane = ncplane_create(std, &nopts);
                                    if (new_nc_plane == NULL) {
                                        FILE *dbg = fopen("/tmp/cronos_debug.log", "a");
                                        fprintf(dbg, "BUG: Failed to create HORZ plane. Rows: %d\n", lower_height);
                                        fclose(dbg);
                                        continue; 
                                    }
                                    ncplane_set_scrolling(new_nc_plane, true);
                                    ncplane_set_fg_rgb8(new_nc_plane, 220, 220, 220);

                                    Pane *new_pane = malloc(sizeof(Pane));
                                    new_pane->id = pkt.payload[1]; 
                                    new_pane->nc_plane = new_nc_plane;
                                    new_pane->border_plane = border_plane;
                                    new_pane->width = parent->width;
                                    new_pane->height = lower_height;
                                    new_pane->y = low_start_y;
                                    new_pane->x = parent->x;
                                    
                                    init_terminal_state(&new_pane->state);
                                    
                                    new_pane->next = parent->next;
                                    parent->next = new_pane;
                                    active_pane = new_pane; 

                                    send_resize_packet(client_sock, parent->id, parent->height, parent->width);
                                    send_resize_packet(client_sock, new_pane->id, new_pane->height, new_pane->width);
                                    
                                    update_active_ui(nc, footer, active_pane, session_name, session_count);
                                    notcurses_render(nc);
                                }
                            }
                            else if (pkt.payload[0] == RES_SPLIT_VERT_SUCC) {                                
                                Pane *parent = active_pane; 
                                while(parent){
                                    if(parent->id == pkt.pane_id) break; 
                                    parent = parent->next; 
                                }
                                if (parent && parent->width >4) {
                                    int left_width = parent->width / 2;
                                    int border_x = parent->x + left_width;
                                    int right_start_x = border_x + 1;
                                    int right_width = parent->width - left_width - 1;
                                    
                                    int pane_height = parent->height;  // Respect footer

                                    
                                    ncplane_resize(parent->nc_plane, 0, 0,                            
                                                   pane_height, left_width, 0, 0,  
                                                   pane_height, left_width);
                                               
                                    parent->width = left_width;
                                    parent->height = pane_height;

                                    struct ncplane_options bopts = {
                                        .y = parent->y,
                                        .x = border_x,
                                        .rows = pane_height,
                                        .cols = 1,
                                        .flags = 0,
                                    };
                                    
                                    struct ncplane *border_plane = ncplane_create(std, &bopts);
                                    uint64_t border_channels = 0;
                                    ncchannels_set_fg_rgb8(&border_channels, 100, 100, 100); // Gray
                                    ncplane_set_base(border_plane, "│", 0, border_channels);

                                    struct ncplane_options nopts = {
                                        .y = parent->y,
                                        .x = right_start_x,
                                        .rows = pane_height,
                                        .cols = right_width,
                                        .flags = 0,
                                    };
                                    
                                    struct ncplane *new_nc_plane = ncplane_create(std, &nopts);
                                    if (new_nc_plane == NULL) {
                                        FILE *dbg = fopen("/tmp/cronos_debug.log", "a");
                                        fprintf(dbg, "BUG: Failed to create VERT plane. Cols: %d\n", right_width);
                                        fclose(dbg);
                                        continue; 
                                    }
                                    ncplane_set_scrolling(new_nc_plane, true);
                                    ncplane_set_fg_rgb8(new_nc_plane, 220, 220, 220);

                                    Pane *new_pane = malloc(sizeof(Pane));
                                    new_pane->id = pkt.payload[1]; 
                                    new_pane->nc_plane = new_nc_plane;
                                    new_pane->border_plane = border_plane;
                                    new_pane->width = right_width;
                                    new_pane->height = pane_height;
                                    new_pane->y = parent->y;
                                    new_pane->x = right_start_x;
                                    
                                    init_terminal_state(&new_pane->state);
                                    
                                    new_pane->next = parent->next;
                                    parent->next = new_pane;
                                    active_pane = new_pane; 

                                    send_resize_packet(client_sock, parent->id, parent->height, parent->width);
                                    send_resize_packet(client_sock, new_pane->id, new_pane->height, new_pane->width);
                                    
                                    update_active_ui(nc, footer, active_pane, session_name, session_count);
                                    notcurses_render(nc);
                                }
                            }
                            else if (pkt.payload[0] == RES_PANE_CLOSED) {
                                Pane *prev = NULL;
                                Pane *curr = pane_list_head;

                                while (curr != NULL) {
                                    if (curr->id == pkt.pane_id) {
                                        Pane *absorbed_by = NULL;
                                        Pane *neighbor = pane_list_head;

                                        while (neighbor) {
                                            // Neighbor to the LEFT -- curr owns the shared border, dies with curr below
                                            if (neighbor != curr && neighbor->x + neighbor->width + 1 == curr->x && neighbor->y == curr->y) {
                                                neighbor->width += curr->width + 1;
                                                ncplane_resize(neighbor->nc_plane, 0, 0, neighbor->height, neighbor->width, 0, 0, neighbor->height, neighbor->width);
                                                send_resize_packet(client_sock, neighbor->id, neighbor->height, neighbor->width);
                                                absorbed_by = neighbor;
                                                break;
                                            }
                                            // Neighbor ABOVE -- curr owns the shared border, dies with curr below
                                            if (neighbor != curr && neighbor->y + neighbor->height + 1 == curr->y && neighbor->x == curr->x) {
                                                neighbor->height += curr->height + 1;
                                                ncplane_resize(neighbor->nc_plane, 0, 0, neighbor->height, neighbor->width, 0, 0, neighbor->height, neighbor->width);
                                                send_resize_packet(client_sock, neighbor->id, neighbor->height, neighbor->width);
                                                absorbed_by = neighbor;
                                                break;
                                            }
                                            // Neighbor to the RIGHT -- neighbor owns the shared border; it must
                                            // move left and inherit whatever border curr had on its far side
                                            if (neighbor != curr && neighbor->x == curr->x + curr->width + 1 && neighbor->y == curr->y) {
                                                neighbor->x = curr->x;
                                                neighbor->width += curr->width + 1;
                                                ncplane_move_yx(neighbor->nc_plane, neighbor->y, neighbor->x);
                                                ncplane_resize(neighbor->nc_plane, 0, 0, neighbor->height, neighbor->width, 0, 0, neighbor->height, neighbor->width);
                                                send_resize_packet(client_sock, neighbor->id, neighbor->height, neighbor->width);

                                                if (neighbor->border_plane) ncplane_destroy(neighbor->border_plane);
                                                neighbor->border_plane = curr->border_plane; // may be NULL, that's fine
                                                curr->border_plane = NULL; // ownership transferred -- don't double-free
                                                absorbed_by = neighbor;
                                                break;
                                            }
                                            // Neighbor BELOW -- same transfer logic, vertically
                                            if (neighbor != curr && neighbor->y == curr->y + curr->height + 1 && neighbor->x == curr->x) {
                                                neighbor->y = curr->y;
                                                neighbor->height += curr->height + 1;
                                                ncplane_move_yx(neighbor->nc_plane, neighbor->y, neighbor->x);
                                                ncplane_resize(neighbor->nc_plane, 0, 0, neighbor->height, neighbor->width, 0, 0, neighbor->height, neighbor->width);
                                                send_resize_packet(client_sock, neighbor->id, neighbor->height, neighbor->width);

                                                if (neighbor->border_plane) ncplane_destroy(neighbor->border_plane);
                                                neighbor->border_plane = curr->border_plane;
                                                curr->border_plane = NULL;
                                                absorbed_by = neighbor;
                                                break;
                                            }
                                            neighbor = neighbor->next;
                                        }

                                        if (prev) prev->next = curr->next;
                                        else pane_list_head = curr->next;

                                        ncplane_destroy(curr->nc_plane);
                                        if (curr->border_plane) ncplane_destroy(curr->border_plane);

                                        if (active_pane == curr) {
                                            active_pane = absorbed_by ? absorbed_by : pane_list_head;
                                        }

                                        free(curr);
                                        break;
                                    }
                                    prev = curr;
                                    curr = curr->next;
                                }
                                update_active_ui(nc, footer, active_pane, session_name, session_count);
                                notcurses_render(nc);
                            }
                        }
                    }
                    else if (bytes == 0) {
                        running = 0; 
                    }
                }
                if (events[i].events & (EPOLLHUP | EPOLLERR)) {
                    running = 0; 
                }
            }
        }
    }

    notcurses_stop(nc);
    if (raw_mode_active) {
        tcsetattr(STDIN_FILENO, TCSANOW, &orig_t);
        raw_mode_active = 0;
    }
    close(client_sock);
    printf("\nDisconnected from Cronos server.\n");
    return 0;
}

void list_sessions(){
    DIR *dir = opendir("/tmp"); 
    if(!dir){
        perror("Error: Failed to Look For Sessions");
        return; 
    }
    struct dirent *entry; 
    int found=0; 
    
    while((entry=readdir(dir))!= NULL){
        if (strncmp(entry->d_name, "cronos_", 7) == 0 && strstr(entry->d_name, ".sock")) {
            if(found==0){
                printf("Active Cronos Sessions\n");
            }
            char session_name[128];
            strncpy(session_name, entry->d_name + 7, strlen(entry->d_name) - 12);
            session_name[strlen(entry->d_name) - 12] = '\0';
            printf("  - %s\n", session_name);
            found++;
        }

    }
    if (found==0) printf("No active sessions found.\n");
    closedir(dir);

}

void kill_all_sessions() {
    DIR *dir = opendir("/tmp");
    if (!dir) {
        perror("Error: Could not open /tmp");
        return;
    }

    struct dirent *entry;
    printf("Terminating all Cronos sessions...\n");

    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "cronos_", 7) == 0 && strstr(entry->d_name, ".pid")) {
            char pid_path[512];
            snprintf(pid_path, sizeof(pid_path), "/tmp/%s", entry->d_name);

            FILE *f = fopen(pid_path, "r");
            if (f) {
                pid_t target_pid;
                if (fscanf(f, "%d", &target_pid) == 1) {
                    printf("  - Killing process %d (File: %s)\n", target_pid, entry->d_name);
                    kill(target_pid, SIGTERM);
                }
                fclose(f);
            }
            
            unlink(pid_path);
            char socket_path[512];
            snprintf(socket_path, sizeof(socket_path), "/tmp/%s", entry->d_name);
            strcpy(socket_path + strlen(socket_path) - 4, ".sock");
            unlink(socket_path);
        }
    }
    closedir(dir);
    printf("All sessions terminated.\n");
}




int main(int argc, char *argv[]) {

    if(argc>1 && (strcmp(argv[1], "--help")==0||strcmp(argv[1], "-h")==0)){
        print_usage(argv[0]);
        return 0;
    }
    if(argc>1 && (strcmp(argv[1], "--inst")==0||strcmp(argv[1], "-i")==0)){
        print_short(argv[0]);
        return 0;
    }
    if(argc<2){
        print_usage(argv[0]);
        return 1;  
    }
    
    char *cmd=argv[1]; 
    if(strcmp(cmd, "list")==0){
        list_sessions(); 
        return 0; 
    }
    if (strcmp(cmd, "killall") == 0) {
        kill_all_sessions();
        return 0;
    }
    if(argc<3){
        fprintf(stderr, "Error: missing session name"); 
        print_usage(argv[0]); 
        return 1; 
    }

    char *session_name=argv[2]; 
    char socket_path[256]; 
    char pid_path[256];     
    snprintf(socket_path, sizeof(socket_path), "/tmp/cronos_%s.sock", session_name); 
    snprintf(pid_path, sizeof(pid_path), "/tmp/cronos_%s.pid", session_name); 


    if (strcmp(cmd, "new") == 0) {
        // Prevent launching duplicate sessions overlapping the same socket
        if (access(socket_path, F_OK) == 0) {
            fprintf(stderr, "Error: Session '%s' already exists. Use 'attach' instead.\n", session_name);
            return 1;
        }

        printf("Launching background session daemon: %s...\n", session_name);
        pid_t pid = fork();
        if (pid == 0) {
            // Child wrapper process: Exec the background daemon server binary

            int devnull = open("/dev/null", O_RDWR);
            if (devnull >= 0) {
                dup2(devnull, STDIN_FILENO);
                dup2(devnull, STDOUT_FILENO);
                dup2(devnull, STDERR_FILENO);
                close(devnull);
            }
            setsid(); 
            char *args[] = {"./cronos-server", session_name, NULL};
            execvp(args[0], args);
            perror("execvp failed to launch server binary");
            exit(1);
        } 
        else if (pid > 0) {
            usleep(200000); 
            waitpid(pid, NULL, WNOHANG); 
            return run_session(socket_path, session_name);
        } 
        else {
            perror("Fork failed");
            return 1;
        }
    } 
    else if (strcmp(cmd, "attach") == 0) {
        if (access(socket_path, F_OK) != 0) {
            fprintf(stderr, "Error: Session '%s' does not exist.\n", session_name);
            return 1;
        }
        return run_session(socket_path, session_name);
    } 
    else if (strcmp(cmd, "kill") == 0) {
        FILE *f = fopen(pid_path, "r");
        if (!f) {
            fprintf(stderr, "Error: Could not find tracking metadata for session '%s'.\n", session_name);
            return 1;
        }
        pid_t target_pid;
        if (fscanf(f, "%d", &target_pid) == 1) {
            printf("Stopping server process %d...\n", target_pid);
            kill(target_pid, SIGTERM);
        }
        fclose(f);
        unlink(socket_path);
        unlink(pid_path);
        return 0;
    } 
    else {
        fprintf(stderr, "Unknown routing directive: %s\n", cmd);
        print_usage(argv[0]);
        return 1;
    }
}