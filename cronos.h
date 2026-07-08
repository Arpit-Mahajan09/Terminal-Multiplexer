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

typedef struct {
    uint8_t type;      // 0 = Keystroke, 1 = PTY Output, 2 = Command (e.g., "Split")
    int pane_id;       // Which pane is this for?
    size_t data_len;   // How many bytes of payload?
    char payload[256]; // The actual text or ANSI codes
} CronosPacket;

#endif