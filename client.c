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
#include <notcurses/notcurses.h>

#include "parser.c"
#include "cronos.h"



struct notcurses_options opts = { 
    .loglevel = NCLOGLEVEL_SILENT,
    .flags = NCOPTION_SUPPRESS_BANNERS | NCOPTION_NO_QUIT_SIGHANDLERS
};

struct termios orig_t;
int raw_mode_active = 0; 

static struct notcurses *nc_global = NULL;

static void cleanup_handler(int sig) {
    (void)sig;
    if (nc_global) {
        notcurses_stop(nc_global);
        nc_global = NULL;
    }

    if (raw_mode_active) {
        tcsetattr(STDIN_FILENO, TCSANOW, &orig_t);
    }
    // Force-restore terminal in case notcurses_stop missed anything
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
    printf("  %s --help, -h      - Display this help menu\n\n", prog);
    printf("Example:\n");
    printf("  cronos new dev_env\n");
}

typedef struct Pane {
    int id;
    int pty_master_fd;         // The FD for this specific shell
    pid_t shell_pid;           // The PID of this specific shell
    TerminalState state;       // The ANSI parser state for this pane
    
    // Notcurses UI elements
    struct ncplane *nc_plane;  // The specific window for this pane
    int width;
    int height;
    int y;
    int x;
    
    // Linked list or Tree pointers
    struct Pane *next;         
} Pane;

Pane *active_pane = NULL;


Pane* split_pane_vertically(struct notcurses *nc, Pane *parent_pane) {
    // 1. Calculate new dimensions (split the width in half)
    int new_width = parent_pane->width / 2;
    int right_start_x = parent_pane->x + new_width;
    
    ncplane_resize(parent_pane->nc_plane, 0, 0,                            
                   parent_pane->height, new_width, 0, 0,  
                   parent_pane->height, new_width);
               
    parent_pane->width = new_width;

    // 3. Create the new right pane
    struct ncplane_options nopts = {
        .y = parent_pane->y,
        .x = right_start_x,
        .rows = parent_pane->height,
        .cols = parent_pane->width, // (The remaining half)
        .flags = 0,
    };
    
    struct ncplane *new_nc_plane = ncplane_create(parent_pane->nc_plane, &nopts);
    ncplane_set_scrolling(new_nc_plane, true);

    // 4. Allocate memory for the new Pane struct
    Pane *new_pane = malloc(sizeof(Pane));
    new_pane->nc_plane = new_nc_plane;
    new_pane->width = parent_pane->width;
    new_pane->height = parent_pane->height;
    new_pane->y = parent_pane->y;
    new_pane->x = right_start_x;
    
    // 5. Spawn a brand new PTY and Shell for this pane!
    new_pane->pty_master_fd = spawn_new_pty(&new_pane->shell_pid);
    init_terminal_state(&new_pane->state);
    
    return new_pane;
}

int run_session(const char *socket_path){

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
    init_terminal_state(&pane_state);

    int epoll_fd = epoll_create1(0);

    struct ncplane *std = notcurses_stdplane(nc); 
    ncplane_set_scrolling(std, true);
    struct epoll_event ev, events[MAX_EVENTS];

    int nc_fd = notcurses_inputready_fd(nc); 
    ev.events = EPOLLIN; ev.data.fd = nc_fd; 
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, nc_fd, &ev);
    
    ev.events = EPOLLIN; ev.data.fd = client_sock; 
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_sock, &ev);

    int running = 1; 


    while (running) {
        int num_events = epoll_wait(epoll_fd, events, MAX_EVENTS, -1); 

        for (int i = 0; i < num_events; i++) {
            char buffer[BUFFER_SIZE]; 
            ncinput ni;
            uint32_t id;

            if (events[i].data.fd == nc_fd && (events[i].events & EPOLLIN)) {  
                // reads changes in user terminal 
                while((id = notcurses_get_nblock(nc, &ni))!=0){
                    if(id == (uint32_t)-1) break; 
                    if (ni.evtype == NCTYPE_RELEASE) {
                        continue;
                    }
                     
                    if((id=='Q'|| id=='q') && ni.ctrl){
                        printf("\r\n[Client Detached]\r\n"); 
                        running =0; 
                        break; 
                    }
                    if((id=='B'|| id=='b') && ni.ctrl){
                        split_pane_vertically(); 
                    }
                    if (id == NCKEY_BACKSPACE || id == 0x7F || id == '\b') {
                        char c = 0x7F;           
                        write(client_sock, &c, 1);
                    }
                    else if (id == NCKEY_ENTER || id == NCKEY_RETURN) {
                        char c = '\r';                       // Enter
                        write(client_sock, &c, 1);
                    }
                    else if(id< 0x80){
                        char c = (char)id; 
                        write(client_sock, &c, 1);
                    }

                    notcurses_render(nc); 
                }
            }
            else if (events[i].data.fd == client_sock ) {
                // reads data in server 
                if(events[i].events & EPOLLIN){
                    ssize_t bytes = read(client_sock, buffer, sizeof(buffer)); 
                    if (bytes > 0) {
                        for(ssize_t j = 0; j < bytes; j++) {
                            parse_ansi_byte(&pane_state, buffer[j], std);
                        }
                        notcurses_render(nc);
                    }
                    else if (bytes == 0) running = 0;
                }
                if(events[i].events & (EPOLLHUP|EPOLLERR)){
                    running =0; 
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
        // Look for our specific tracking files
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
            
            // Clean up files after signaling process
            unlink(pid_path);
            // Derive and remove the socket file
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
        } else if (pid > 0) {
            // Give the server daemon a brief window to complete initialization
            usleep(200000); 
            waitpid(pid, NULL, WNOHANG); // Clean up the immediate fork zombie
            return run_session(socket_path);
        } else {
            perror("Fork failed");
            return 1;
        }
    } 
    else if (strcmp(cmd, "attach") == 0) {
        if (access(socket_path, F_OK) != 0) {
            fprintf(stderr, "Error: Session '%s' does not exist.\n", session_name);
            return 1;
        }
        return run_session(socket_path);
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

