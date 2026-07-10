#ifndef CRONOS_H
#define CRONOS_H

#define MAX_EVENTS 5
#define BUFFER_SIZE 256

#define PKT_TYPE_KEYSTROKE  0
#define PKT_TYPE_PTY_OUTPUT 1
#define PKT_TYPE_COMMAND    2

#define REQ_SPLIT_VERT       1
#define REQ_SPLIT_HORZ       2
#define RES_SPLIT_VERT_SUCC  3
#define RES_SPLIT_HORZ_SUCC  4
#define WINDOW_RESIZE        5
#define RES_PANE_CLOSED 6

typedef struct {
    uint8_t type;      // 0 = Keystroke, 1 = PTY Output, 2 = Command (e.g., "Split")
    int pane_id;       // Which pane is this for?
    size_t data_len;   // How many bytes of payload?
    char payload[BUFFER_SIZE]; // The actual text or ANSI codes
} CronosPacket;

#endif