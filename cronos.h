#ifndef CRONOS_H
#define CRONOS_H

#define MAX_EVENTS 5
#define BUFFER_SIZE 256

#define REQ_SPLIT_VERT
#define RES_SPLIT_SUCCESS
#define WINDOW_RESIZE

typedef struct {
    int type; 
    int length;
    char data[BUFFER_SIZE];

} Pkt;

#endif