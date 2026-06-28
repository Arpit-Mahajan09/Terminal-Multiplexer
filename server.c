#define _GNU_SOURCE
#define _XOPEN_SOURCE 600 
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

#include "cronos.h"
#include <signal.h>

char global_socket_path[256];
char global_pid_path[256];

void handle_sigterm(int sig) {
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
    signal(SIGTERM, handle_sigterm);
    signal(SIGPIPE, SIG_IGN); 

    int masterFd = posix_openpt(O_RDWR | O_NOCTTY);
    if (masterFd==-1 || grantpt(masterFd)==-1|| unlockpt(masterFd)==-1 ) return 1;
    
    char* slave_name = ptsname(masterFd);
    int slaveFd = open(slave_name, O_RDWR|O_NOCTTY);
    
    pid_t pid = fork(); 

    if (pid == 0) { 
        close(masterFd); 
        setsid();
        ioctl(slaveFd, TIOCSCTTY, 0);

        dup2(slaveFd, STDIN_FILENO);
        dup2(slaveFd, STDOUT_FILENO);
        dup2(slaveFd, STDERR_FILENO);
        if(slaveFd > 2) close(slaveFd); 
        
        char *args[] = {"/bin/sh", NULL};   
        execvp(args[0], args);
        _exit(EXIT_FAILURE); 
    }

    if (daemon(0, 0)==-1) {
        perror("daemon failed");
        return 1;
    }

    close(slaveFd);
    
    unlink(socket_path);
    int server_sock = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;
    addr.sun_family = AF_UNIX;

    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1); 
    bind(server_sock, (struct sockaddr*)&addr, sizeof(addr));
    listen(server_sock, 1);

    printf("Server daemon running. Waiting for client...\n");
    int client_sock = accept(server_sock, NULL, NULL);
    printf("Client connected!\n");

    
    int epoll_fd = epoll_create1(0);
    struct epoll_event ev, events[MAX_EVENTS];
    
    ev.events = EPOLLIN; ev.data.fd = masterFd; 
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, masterFd, &ev);
    
    ev.events = EPOLLIN; ev.data.fd = client_sock; 
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_sock, &ev);

    int running=1; 
    FILE *pid_file = fopen(pid_path, "w"); 
    if (pid_file) {
        fprintf(pid_file, "%d\n", getpid());
        fclose(pid_file);
    } else {
        perror("Failed to create pid file");
    }


    ev.events = EPOLLIN; 
    ev.data.fd = server_sock; 
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_sock, &ev);

    while (running) {
        int num_events = epoll_wait(epoll_fd, events, MAX_EVENTS, -1); 
        for (int i = 0; i < num_events; i++) {
            char buffer[BUFFER_SIZE]; 

            if (events[i].data.fd == server_sock && (events[i].events & EPOLLIN)) {
                int new_client = accept(server_sock, NULL, NULL);
                if (new_client != -1) {
                    struct epoll_event ev_client;
                    ev_client.events = EPOLLIN | EPOLLHUP | EPOLLERR; 
                    ev_client.data.fd = new_client;
                    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, new_client, &ev_client);

                    client_sock = new_client; 
                }
            }
            
            if (events[i].data.fd == client_sock && (events[i].events & EPOLLIN)) {
                ssize_t bytes = read(client_sock, buffer, sizeof(buffer)); 
                if (bytes > 0) write(masterFd, buffer, bytes);
                else {
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_sock, NULL);
                    close(client_sock);
                    client_sock = -1; 
                } 
            }
            
            else if (events[i].data.fd == masterFd && (events[i].events & EPOLLIN)) {
                ssize_t bytes = read(masterFd, buffer, sizeof(buffer)); 
                if (bytes>0 && client_sock!=-1) write(client_sock, buffer, bytes);
                else if (bytes <= 0) running = 0; 
            }
        }
    }
    close(client_sock);
    close(server_sock);
    close(masterFd);
    return 0;
}

// ./server work
// ./client work

// ps aux | grep server
// lsof +U | grep "SOCKET_PATH"

// kill <PID>
// pkill server 
// kill $(cat /var/run/cronos.pid)