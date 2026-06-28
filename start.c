#define _XOPEN_SOURCE 600 
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <fcntl.h>
#include <signal.h>

#include <sys/wait.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <termios.h>

#define MAX_EVENTS 5
#define BUFFER_SIZE 256

struct termios original_terminal_settings;

int main(){
    if (tcgetattr(STDIN_FILENO, &original_terminal_settings) == -1) {
        perror("tcgetattr original");
        return 1;
    }
    char* slave_name;
    int masterFd, slaveFd; 


    masterFd = posix_openpt(O_RDWR | O_NOCTTY);  // open an unused master pseudo-terminal device // creates slavePTY in locked state

    if(unlockpt(masterFd)==-1) return 1;  // unlocks the slave pTY 
    if(grantpt(masterFd)==-1) return 1; 
    
    slave_name = ptsname(masterFd); // Find filename coresponding to masterPTY in string
    if(slave_name==NULL) return 1; 

    slaveFd = open(slave_name, O_RDWR|O_NOCTTY);  // Executed by child process to establish connectioins to the slave terminal
    if(slaveFd ==-1) return 1; 

    printf("Successfully opened slave: %s\n", slave_name);

    pid_t pid;        // variable to hold proces id
    pid = fork(); 

    switch(pid){
        case -1: 
                perror("fork"); 
                exit(EXIT_FAILURE); 
        case 0:
                puts("Child executing"); 
                close(masterFd); 

                if(setsid()==-1){
                    perror("setsid"); 
                    exit(EXIT_FAILURE);    
                }

                if(ioctl(slaveFd, TIOCSCTTY,0)==-1){
                    perror("ioctl");
                    exit(EXIT_FAILURE); 
                }

                dup2(slaveFd, STDIN_FILENO);      // duplicate stdin,stdout, sterror to file
                dup2(slaveFd, STDOUT_FILENO);
                dup2(slaveFd, STDERR_FILENO);

                if(slaveFd>2) close(slaveFd); 
                char *args[]= {"/bin/sh", NULL};   
                execvp(args[0], args);

                perror("execvp failed");
                _exit(EXIT_FAILURE); 

        default:
            close(slaveFd);

            int epoll_fd = epoll_create1(0);    // epoll monitors file discriptors 
            if (epoll_fd == -1) {
                perror("Epoll creation failed");
                return 1;
            }

            struct epoll_event ev, events[MAX_EVENTS];
            ev.events = EPOLLIN; 
            ev.data.fd = masterFd; 

            if(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, masterFd, &ev)==-1){
                perror("epoll_ctl masterFd");
                return 1; 
            }

            ev.events = EPOLLIN;
            ev.data.fd = STDIN_FILENO;
            if(epoll_ctl(epoll_fd, EPOLL_CTL_ADD, STDIN_FILENO, &ev) == -1){
                perror("epoll_ctl stdin");
                return 1; 
            }

            printf("Parent: Waiting for command output via epoll...\n\n");

            struct termios raw = original_terminal_settings;
            tcgetattr(masterFd, &raw);

            raw.c_lflag &= ~ECHO; //Disables Echoing Passwords 
            raw.c_lflag &= ~(ISIG|IEXTEN); 
            raw.c_lflag &= ~ICANON; //Enables shortcut keys
            raw.c_lflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
            raw.c_cflag &= ~CSIZE;
            raw.c_lflag |= ~CS8;
            raw.c_lflag &= ~(OPOST);

            if(masterFd ==-1){
                perror("Error: Pseudo Terminal"); 
                return 1; 
            }

            int running = 1; 
            while(running){
                int num_events = epoll_wait(epoll_fd, events, MAX_EVENTS, -1); 
                if(num_events==-1){
                    perror("epoll_wait: failed"); 
                    break; 
                }
                for(int i=0; i<num_events; i++){
                    char buffer[BUFFER_SIZE]; 

                    if(events[i].data.fd ==STDIN_FILENO){
                        ssize_t byte_read = read(STDIN_FILENO,buffer, sizeof(buffer)); 
                        if(byte_read>0){
                            write(masterFd, buffer, byte_read); 
                        }
                    }
                    else if(events[i].data.fd==masterFd){
                        if(events[i].events & EPOLLIN){
                            ssize_t byte_read = read(masterFd, buffer, sizeof(buffer)); 
                            if(byte_read>0){
                                write(STDOUT_FILENO, buffer, byte_read); 
                            }
                            else if(byte_read<0){
                                running=0; 
                            }
                        }
                    }

                    if(events[i].events & (EPOLLHUP| EPOLLERR)){
                        running =0; 
                    }
                }
            }
        close(epoll_fd);

        int status;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status)) {
            tcsetattr(STDIN_FILENO, TCSANOW, &original_terminal_settings);
            printf("\nParent: Child process exited with status %d.\n", WEXITSTATUS(status));
        }

    }

    close(slaveFd); 
    close(masterFd);
}