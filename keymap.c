#include "cronos.h"
#include "pane.h"
#include "guide.h"     
#include "session_ui.h"
#include "window.h"
#include <stdint.h>
#include <stdio.h> 
#include <notcurses/notcurses.h>


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
    bind_key(NCKEY_PGUP, 0, 0, ACTION_SCROLL_UP);
    bind_key(NCKEY_PGDOWN, 0, 0, ACTION_SCROLL_DOWN);

    bind_key('n', 1, 0, ACTION_NEW_WINDOW);   bind_key('N', 1, 0, ACTION_NEW_WINDOW);
    bind_key('r', 1, 0, ACTION_RENAME_WINDOW); bind_key('R', 1, 0, ACTION_RENAME_WINDOW);
    bind_key('a', 1, 0, ACTION_SESSION_MENU); bind_key('A', 1, 0, ACTION_SESSION_MENU);
    bind_key(']', 1, 0, ACTION_NEXT_WINDOW);
    bind_key('[', 1, 0, ACTION_PREV_WINDOW);
}


int parse_key_name(const char *name, uint32_t *key_out) {
    if (strcmp(name, "left") == 0)  { *key_out = NCKEY_LEFT;  return 1; }
    if (strcmp(name, "right") == 0) { *key_out = NCKEY_RIGHT; return 1; }
    if (strcmp(name, "up") == 0)    { *key_out = NCKEY_UP;    return 1; }
    if (strcmp(name, "down") == 0)  { *key_out = NCKEY_DOWN;  return 1; }
    if (strlen(name) == 1) { *key_out = (uint32_t)name[0]; return 1; }
    return 0;
}






void send_keystroke_to_server(ClientContext *ctx, uint32_t id) {
    CronosPacket pkt;
    memset(&pkt, 0, sizeof(CronosPacket));
    pkt.type = PKT_TYPE_KEYSTROKE;
    pkt.pane_id = ctx->active_pane ? ctx->active_pane->id : 0;
    pkt.data_len = 1;

    int send_packet = 0;

    if (id == NCKEY_BACKSPACE || id == 0x7F || id == '\b') {
        pkt.payload[0] = 0x7F;
        send_packet = 1;
    }
    else if (id == NCKEY_ENTER || id == NCKEY_RETURN) {
        pkt.payload[0] = '\r';
        send_packet = 1;
    }
    else if (id == NCKEY_UP) {
        memcpy(pkt.payload, "\x1B[A", 3);
        pkt.data_len = 3;
        send_packet = 1;
    }
    else if (id == NCKEY_DOWN) {
        memcpy(pkt.payload, "\x1B[B", 3);
        pkt.data_len = 3;
        send_packet = 1;
    }
    else if (id == NCKEY_RIGHT) {
        memcpy(pkt.payload, "\x1B[C", 3);
        pkt.data_len = 3;
        send_packet = 1;
    }
    else if (id == NCKEY_LEFT) {
        memcpy(pkt.payload, "\x1B[D", 3);
        pkt.data_len = 3;
        send_packet = 1;
    }
    else if (id < 0x80) {
        pkt.payload[0] = (char)id;
        send_packet = 1;
    }

    if (send_packet) {
        write(ctx->client_sock, &pkt, sizeof(CronosPacket));
    }
}




#include "guide.h"     
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
int process_user_action(ClientContext *ctx, uint32_t id, ncinput *ni) {
    Action act = resolve_action(id, ni);
    
    if (act != ACTION_NONE) {
        switch (act) {
            case ACTION_DETACH:
                return 0; // Signal to break the main loop

            case ACTION_SPLIT_VERT:
            case ACTION_SPLIT_HORZ:
            case ACTION_CLOSE_PANE: {
                CronosPacket pkt;
                memset(&pkt, 0, sizeof(CronosPacket));
                pkt.type = PKT_TYPE_COMMAND;
                pkt.pane_id = ctx->active_pane ? ctx->active_pane->id : 0;
                pkt.data_len = 1;
                
                if (act == ACTION_SPLIT_VERT) pkt.payload[0] = REQ_SPLIT_VERT;
                else if (act == ACTION_SPLIT_HORZ) pkt.payload[0] = REQ_SPLIT_HORZ;
                else if (act == ACTION_CLOSE_PANE) pkt.payload[0] = REQ_CLOSE_PANE;
                
                write(ctx->client_sock, &pkt, sizeof(CronosPacket));
                break;
            }

            case ACTION_FOCUS_LEFT:
            case ACTION_FOCUS_RIGHT:
            case ACTION_FOCUS_DOWN:
            case ACTION_FOCUS_UP:
                handle_pane_focus(ctx, act);
                break;

            case ACTION_RESIZE_RIGHT:
            case ACTION_RESIZE_LEFT:
            case ACTION_RESIZE_DOWN:
            case ACTION_RESIZE_UP:
                handle_pane_resize(ctx, act);
                break;

            case ACTION_NEW_WINDOW:
                window_create(ctx);  
                break;

            case ACTION_NEXT_WINDOW:
                if (ctx->active_window)
                    ctx->active_window->active_pane = ctx->active_pane;
                window_next(ctx);
                ctx_sync_window(ctx);
                break;

            case ACTION_PREV_WINDOW:
                if (ctx->active_window)
                    ctx->active_window->active_pane = ctx->active_pane;
                window_prev(ctx);
                ctx_sync_window(ctx);
                break;

            case ACTION_RENAME_WINDOW: {
                window_rename(ctx, "renamed");  
                break;
            }

            case ACTION_SESSION_MENU:
                show_session_ui(ctx);
                window_render_bar(ctx);
                notcurses_render(ctx->nc);
                break;

            case ACTION_SCROLL_UP:
                if (!ctx->active_pane) break;
                if (!ctx->active_pane->scroll_overlay)
                    enter_scrollback(ctx, ctx->active_pane);
                else {
                    ctx->active_pane->state.scroll_offset++;
                    render_scrollback_view(ctx->active_pane);
                }
                notcurses_render(ctx->nc);
                break;

            case ACTION_SCROLL_DOWN:
                if (!ctx->active_pane || !ctx->active_pane->scroll_overlay) break;
                if (ctx->active_pane->state.scroll_offset > 1) {
                    ctx->active_pane->state.scroll_offset--;
                    render_scrollback_view(ctx->active_pane);
                } else {
                    exit_scrollback(ctx->active_pane);
                }
                notcurses_render(ctx->nc);
                break;
                
            default:
                break;
        }
    } else {
        send_keystroke_to_server(ctx, id);
    }
    return 1;
}