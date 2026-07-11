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

#include "guide.c"
#include "pane.h"
#include "keymap.h"
#include "parser.c"


Pane *pane_list_head = NULL;
Pane *pane_at_yx(int y, int x) {
    Pane *p = pane_list_head;
    while (p) {
        if (x >= p->x && x < p->x + p->width && y >= p->y && y < p->y + p->height) return p;
        p = p->next;
    }
    return NULL;
}

ClientConfig g_client_config;




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


Pane *active_pane = NULL;



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


    ClientContext ctx; 
    memset(&ctx, 0, sizeof(ClientContext));
    ctx.std = std;
    ctx.footer = footer;
    ctx.client_sock = client_sock;
    ctx.pane_list_head = root_pane;
    ctx.active_pane = root_pane;
    ctx.session_count = session_count;
    strncpy(ctx.session_name, session_name, sizeof(ctx.session_name) - 1);


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
                    if (ni.evtype == NCTYPE_RELEASE) continue;
                    
                    FILE *idbg = fopen("/tmp/cronos_input.log", "a");
                    if (idbg) { fprintf(idbg, "id=0x%x alt=%d ctrl=%d shift=%d\n", id, ni.alt, ni.ctrl, ni.shift); fclose(idbg); }

                    if (!process_user_action(&ctx, id, &ni)) {
                        running = 0; 
                        break;
                    }
                }
            }
            else if (events[i].data.fd == client_sock ) {
                if(events[i].events & EPOLLIN){
                    CronosPacket pkt; 
                    ssize_t bytes = recv(client_sock, &pkt, sizeof(CronosPacket), MSG_WAITALL); 
                    FILE *dbg = fopen("/tmp/cronos_client.log", "a");
                    if (dbg) { fprintf(dbg, "RECV type=%d pane=%d len=%zu\n", pkt.type, pkt.pane_id, pkt.data_len); fclose(dbg); }


                    if (bytes == sizeof(CronosPacket)) {
                        if (pkt.type == PKT_TYPE_PTY_OUTPUT) {
                            handle_pty_output(&ctx,& pkt); 
                        }
                        else if (pkt.type == PKT_TYPE_COMMAND) {  //Control Commands
                            if (pkt.payload[0] == RES_SPLIT_HORZ_SUCC) { 
                                horz_split(&ctx,&pkt);                            
                            }
                            else if (pkt.payload[0] == RES_SPLIT_VERT_SUCC) {     
                                vert_split(&ctx,&pkt);                                  
                            }
                            else if (pkt.payload[0] == RES_PANE_CLOSED) {
                                res_pane_close(&ctx,&pkt); 
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