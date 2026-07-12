#define _XOPEN_SOURCE 600 
#define HISTORY_SIZE (1024 * 64)    
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include <sys/wait.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <errno.h> 
#include "headers/cronos.h"
#include <signal.h>

#define MAX_PANES 10
#define MAX_ENV_VARS 32

typedef struct {
    char shell_path[256];
    char env_keys[MAX_ENV_VARS][64];
    char env_vals[MAX_ENV_VARS][192];
    int env_count;
} ServerConfig;

ServerConfig g_config;

void load_server_config(void) {
    strcpy(g_config.shell_path, "/bin/sh");
    g_config.env_count = 0;

    char path[512];
    const char *home = getenv("HOME");
    if (!home) return;
    snprintf(path, sizeof(path), "%s/.config/cronos/cronos.conf", home);

    FILE *f = fopen(path, "r");
    if (!f) return; // defaults stand

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        // strip trailing newline
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == '#' || line[0] == '\0') continue;

        char key[128], val[192];
        if (strncmp(line, "shell", 5) == 0 && sscanf(line, "shell = %191[^\n]", val) == 1) {
            strncpy(g_config.shell_path, val, sizeof(g_config.shell_path) - 1);
        }
        else if (strncmp(line, "env", 3) == 0 && sscanf(line, "env %127[^= ] = %191[^\n]", key, val) == 2) {
            if (g_config.env_count < MAX_ENV_VARS) {
                strncpy(g_config.env_keys[g_config.env_count], key, sizeof(g_config.env_keys[0]) - 1);
                strncpy(g_config.env_vals[g_config.env_count], val, sizeof(g_config.env_vals[0]) - 1);
                g_config.env_count++;
            }
        }
    }
    fclose(f);
}

typedef struct {
    int id;
    int master_fd;
    pid_t shell_pid;
    int is_active;
} ServerPane;

ServerPane panes[MAX_PANES];
char history_buf[HISTORY_SIZE];
CronosPacket split_history[MAX_PANES];
int split_count = 0;

int next_pane_id = 0;
size_t hist_head = 0;
size_t hist_size = 0;

int spawn_new_pty(int *pane_id){
    if(next_pane_id>= MAX_PANES) return -1;

    //FILE *dbg = fopen("/tmp/cronos_spawn.log", "a");

    int masterFd = posix_openpt(O_RDWR | O_NOCTTY);
    if (grantpt(masterFd) == -1) { 
       // if(dbg) fprintf(dbg, "grantpt failed: %d\n", errno); if(dbg) fclose(dbg); 
        return -1; }
    if (masterFd == -1) {
        // if(dbg) fprintf(dbg, "posix_openpt failed: %d\n", errno); if(dbg) fclose(dbg); 
         return -1; }
    if (unlockpt(masterFd) == -1) { 
//        if(dbg) fprintf(dbg, "unlockpt failed: %d\n", errno); if(dbg) fclose(dbg); 
        return -1; }

    struct winsize ws = { .ws_row = 24, .ws_col = 80 };
    ioctl(masterFd, TIOCSWINSZ, &ws);
    
    char* slave_name = ptsname(masterFd);
    // if (dbg) fprintf(dbg, "slave_name = %s\n", slave_name ? slave_name : "NULL");

    int slaveFd = open(slave_name, O_RDWR|O_NOCTTY);
    if (slaveFd == -1) {
        // if (dbg) { fprintf(dbg, "open(slave) FAILED: errno=%d (%s)\n", errno, strerror(errno)); fclose(dbg); }
        close(masterFd);
        return -1;
    }
    // if (dbg) fprintf(dbg, "slaveFd opened OK: %d\n", slaveFd);
    
    pid_t pid = fork(); 

    if (pid == 0) { 
        close(masterFd); 
        setsid();
        ioctl(slaveFd, TIOCSCTTY, 0);

        dup2(slaveFd, STDIN_FILENO);
        dup2(slaveFd, STDOUT_FILENO);
        dup2(slaveFd, STDERR_FILENO);
        if(slaveFd > 2) close(slaveFd); 

        setenv("TERM", "xterm-256color", 1); // fallback default, may be overridden below
        for (int i = 0; i < g_config.env_count; i++) {
            setenv(g_config.env_keys[i], g_config.env_vals[i], 1);
        }
        
        char *args[] = { g_config.shell_path, "-i", NULL };
        execvp(args[0], args);
        fprintf(stderr, "execvp failed: %s\n", strerror(errno));
        _exit(EXIT_FAILURE); 
    }
    else if (pid > 0) {
        close(slaveFd);
        // if (dbg) { fprintf(dbg, "forked child pid=%d for pane %d\n", pid, next_pane_id); fclose(dbg); }
        
        *pane_id = next_pane_id;
        panes[*pane_id].id = *pane_id;
        panes[*pane_id].master_fd = masterFd;
        panes[*pane_id].shell_pid = pid;
        panes[*pane_id].is_active = 1;
        
        next_pane_id++;
        return masterFd;
    }

    // if (dbg) { fprintf(dbg, "fork() FAILED: errno=%d\n", errno); fclose(dbg); }
    close(slaveFd);
    close(masterFd);
    return -1; 
}



void add_to_history(const char* buff, size_t len){
    for(size_t i=0; i<len; i++){
        history_buf[(hist_head + hist_size)% HISTORY_SIZE] = buff[i]; 
        if(hist_size< HISTORY_SIZE){
            hist_size++; 
        }
        else{
            hist_head = (hist_head+1)% HISTORY_SIZE; 
        }
    }
}

void dump_history_to_client(int client_fd) {
    if (hist_size == 0) return;
    size_t bytes_sent = 0;
    while (bytes_sent < hist_size) {
        size_t index = (hist_head + bytes_sent) % HISTORY_SIZE;
        size_t chunk_size = HISTORY_SIZE - index;
        if (chunk_size > BUFFER_SIZE) chunk_size = BUFFER_SIZE;
        if (bytes_sent + chunk_size > hist_size) chunk_size = hist_size - bytes_sent;

        CronosPacket pkt;
        memset(&pkt, 0, sizeof(CronosPacket));
        pkt.type = PKT_TYPE_PTY_OUTPUT;
        pkt.pane_id = 0; // History currently tracks the main/first pane
        pkt.data_len = chunk_size;
        memcpy(pkt.payload, history_buf + index, chunk_size);
        
        write(client_fd, &pkt, sizeof(CronosPacket));
        bytes_sent += chunk_size;
    }
}

char global_socket_path[256];
char global_pid_path[256];

void handle_sigterm(int sig) {
    for (int p = 0; p < next_pane_id; p++) {
        if (panes[p].is_active) {
            kill(-panes[p].shell_pid, SIGTERM);  
        }
    }

    unlink(global_socket_path);
    unlink(global_pid_path);
    _exit(0);
}


int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <session_name>\n", argv[0]);
        return 1;
    }

    char socket_path[256];
    char pid_path[256];
    snprintf(socket_path, sizeof(socket_path), "/tmp/cronos_%s.sock", argv[1]);
    snprintf(pid_path, sizeof(pid_path), "/tmp/cronos_%s.pid", argv[1]);

    strncpy(global_socket_path, socket_path, sizeof(global_socket_path));
    strncpy(global_pid_path, pid_path, sizeof(global_pid_path));

    signal(SIGTERM, handle_sigterm);
    signal(SIGPIPE, SIG_IGN); 

    if (daemon(0, 0)==-1) {
        perror("daemon failed");
        return 1;
    }

    memset(panes, 0, sizeof(panes));
    load_server_config();
    int primary_pane_id;
    int masterFd = spawn_new_pty(&primary_pane_id);
    if (masterFd == -1) return 1;
    
    unlink(socket_path);
    int server_sock = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;
    addr.sun_family = AF_UNIX;

    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1); 
    bind(server_sock, (struct sockaddr*)&addr, sizeof(addr));
    listen(server_sock, 1);

    int epoll_fd = epoll_create1(0);
    struct epoll_event ev, events[MAX_EVENTS];
    
    ev.events = EPOLLIN; ev.data.fd = masterFd; 
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, masterFd, &ev);
    
    ev.events = EPOLLIN; ev.data.fd = server_sock; 
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_sock, &ev);

    int running=1; 
    int client_sock=-1; 

    FILE *pid_file = fopen(pid_path, "w"); 
    if (pid_file) {
        fprintf(pid_file, "%d\n", getpid());
        fclose(pid_file);
    } else {
        perror("Failed to create pid file");
    }


    while (running) {
        int num_events = epoll_wait(epoll_fd, events, MAX_EVENTS, -1); 

        // FILE *edbg = fopen("/tmp/cronos_epoll.log", "a");
        // if (edbg) fcntl(fileno(edbg), F_SETFD, FD_CLOEXEC);

        for (int i = 0; i < num_events; i++) {
            int active_fd = events[i].data.fd;

            // if (edbg) {
            //     fprintf(edbg, "  fd=%d flags=%s%s%s%s  (masterFd=%d server_sock=%d client_sock=%d)\n",
            //     active_fd,
            //     (events[i].events & EPOLLIN)  ? "IN "  : "",
            //     (events[i].events & EPOLLOUT) ? "OUT " : "",
            //     (events[i].events & EPOLLHUP) ? "HUP " : "",
            //     (events[i].events & EPOLLERR) ? "ERR " : "",
            //     masterFd, server_sock, client_sock);
            // }

            if (active_fd == server_sock) {
                if (events[i].events & EPOLLIN) {
                    int new_client = accept(server_sock, NULL, NULL);
                    if (new_client != -1) {
                        struct epoll_event ev_client;
                        ev_client.events = EPOLLIN | EPOLLHUP | EPOLLERR; 
                        ev_client.data.fd = new_client;
                        epoll_ctl(epoll_fd, EPOLL_CTL_ADD, new_client, &ev_client);

                        client_sock = new_client;   
                        for (int s = 0; s < split_count; s++) {
                            write(client_sock, &split_history[s], sizeof(CronosPacket));
                        }
                        dump_history_to_client(client_sock);
                    }
                }
            }
            else if (active_fd == client_sock) {
                if (events[i].events & EPOLLIN) {
                    CronosPacket pkt; 
                    ssize_t bytes = recv(client_sock, &pkt, sizeof(CronosPacket), MSG_WAITALL);

                    if (bytes == sizeof(CronosPacket)) {
                        if (pkt.type == PKT_TYPE_KEYSTROKE) {
                            int target_id = pkt.pane_id;
                            ssize_t w = -1;

                            if (target_id >= 0 && target_id < next_pane_id && panes[target_id].is_active) {
                                w = write(panes[target_id].master_fd, pkt.payload, pkt.data_len);
                            }

                        //     FILE *dbg = fopen("/tmp/cronos_server.log", "a");
                        //     if (dbg) { fprintf(dbg, "WROTE %zd bytes to pane %d master_fd (errno=%d)\n", w, target_id, errno); fclose(dbg); }
                        }
                        else if (pkt.type == PKT_TYPE_COMMAND) {
                            if (pkt.payload[0] == WINDOW_RESIZE) {
                                int target_id = pkt.pane_id;
                                if (target_id >= 0 && target_id < next_pane_id && panes[target_id].is_active) {
                                    struct winsize ws;
                                    ws.ws_row = (unsigned char)pkt.payload[1];
                                    ws.ws_col = (unsigned char)pkt.payload[2];
                                    ws.ws_xpixel = 0; 
                                    ws.ws_ypixel = 0; 
                                    ioctl(panes[target_id].master_fd, TIOCSWINSZ, &ws);
                                }
                            }
                            else if (pkt.payload[0] == REQ_SPLIT_VERT || pkt.payload[0] == REQ_SPLIT_HORZ) {
                                int new_pane_id;
                                int new_master_fd = spawn_new_pty(&new_pane_id);
                                
                                if (new_master_fd != -1) {
                                    struct epoll_event ev_pty;
                                    ev_pty.events = EPOLLIN;
                                    ev_pty.data.fd = new_master_fd;
                                    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, new_master_fd, &ev_pty);

                                    CronosPacket res_pkt;
                                    memset(&res_pkt, 0, sizeof(CronosPacket));
                                    res_pkt.type = PKT_TYPE_COMMAND;
                                    res_pkt.data_len = 2; 
                                    res_pkt.pane_id = pkt.pane_id; 
                                    
                                    if (pkt.payload[0] == REQ_SPLIT_VERT) {
                                        res_pkt.payload[0] = RES_SPLIT_VERT_SUCC;
                                    } else {
                                        res_pkt.payload[0] = RES_SPLIT_HORZ_SUCC;
                                    }
                                    
                                    res_pkt.payload[1] = new_pane_id; 
                                    if (split_count < MAX_PANES) {
                                        split_history[split_count++] = res_pkt;
                                    }
                                    write(client_sock, &res_pkt, sizeof(CronosPacket));
                                }
                            }
                            else if (pkt.payload[0] == REQ_CLOSE_PANE) {
                                int target_id = pkt.pane_id;
                                if (target_id > 0 && target_id < next_pane_id && panes[target_id].is_active) {
                                    kill(-panes[target_id].shell_pid, SIGTERM);
                                }
                            }
                            else if (pkt.payload[0] == REQ_NEW_WINDOW) {

                                int new_pane_id;
                                int new_fd = spawn_new_pty(&new_pane_id);

                                if (new_fd != -1) {
                                    struct epoll_event ev_pty;
                                    ev_pty.events   = EPOLLIN;
                                    ev_pty.data.fd  = new_fd;
                                    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, new_fd, &ev_pty);

                                    CronosPacket res;
                                    memset(&res, 0, sizeof(res));
                                    res.type       = PKT_TYPE_COMMAND;
                                    res.data_len   = 2;
                                    res.payload[0] = RES_NEW_WINDOW_SUCC;
                                    res.payload[1] = (unsigned char)new_pane_id;
                                    write(client_sock, &res, sizeof(res));
                                }
                            }
                            else if (pkt.payload[0] == REQ_RENAME_SESSION) {
                                char new_name[128];

                                strncpy(new_name, &pkt.payload[1], sizeof(new_name) - 1);
                                snprintf(global_socket_path, sizeof(global_socket_path),
                                        "/tmp/cronos_%s.sock", new_name);
                                snprintf(global_pid_path, sizeof(global_pid_path),
                                        "/tmp/cronos_%s.pid", new_name);
                            }
                        }
                    } 
                    else if (bytes <= 0) { 
                        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_sock, NULL);
                        close(client_sock);
                        client_sock = -1; 
                    }
                }
                if (events[i].events & (EPOLLHUP | EPOLLERR)) {
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_sock, NULL);
                    close(client_sock);
                    client_sock = -1;
                }
            }
            // : PTY Output 
            else { 
                if (events[i].events & EPOLLIN) {
                    int found_pane_id = -1;
                    for (int p = 0; p < next_pane_id; p++) {
                        if (panes[p].is_active && panes[p].master_fd == active_fd) {
                            found_pane_id = p;
                            break;
                        }
                    }

                    if (found_pane_id != -1) {
                        CronosPacket pkt;
                        memset(&pkt, 0, sizeof(CronosPacket));

                        ssize_t bytes = read(active_fd, pkt.payload, sizeof(pkt.payload));
                        // FILE *dbg = fopen("/tmp/cronos_server.log", "a");
                        // if (dbg) {
                            // fprintf(dbg, "read(fd=%d, pane=%d) -> %zd  errno=%d (%s)\n",
                                // active_fd, found_pane_id, bytes, errno, strerror(errno));
                            // fclose(dbg);
                        // }

                        if (bytes > 0) {
                            if (found_pane_id == 0) add_to_history((char*)pkt.payload, bytes);
                            if (client_sock != -1) {
                                pkt.type = PKT_TYPE_PTY_OUTPUT;
                                pkt.pane_id = found_pane_id;
                                pkt.data_len = bytes;
                                write(client_sock, &pkt, sizeof(CronosPacket));
                            }
                        }
                        else if (bytes <= 0) {
                            panes[found_pane_id].is_active = 0;
                            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, active_fd, NULL);
                            close(active_fd);

                            if (found_pane_id == 0) {
                                running = 0;
                            } else if (client_sock != -1) {
                                CronosPacket kill_pkt;
                                memset(&kill_pkt, 0, sizeof(CronosPacket));
                                kill_pkt.type = PKT_TYPE_COMMAND;
                                kill_pkt.pane_id = found_pane_id;
                                kill_pkt.data_len = 1;
                                kill_pkt.payload[0] = RES_PANE_CLOSED;
                                write(client_sock, &kill_pkt, sizeof(CronosPacket));
                            }
                        }
                    }
                }
            } 
        } 
        //if (edbg) fclose(edbg);
    }
    if (client_sock!=-1) close(client_sock);
    close(server_sock);
    unlink(socket_path);
    unlink(pid_path);
    return 0;
}

// ./server work
// ./client work

// ps aux | grep server
// lsof +U | grep "SOCKET_PATH"

// kill <PID>
// pkill server 
// kill $(cat /var/run/cronos.pid)