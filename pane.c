#include <notcurses/notcurses.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

#include "cronos.h"

void handle_pty_output(ClientContext *ctx, CronosPacket *pkt){
    struct ncplane *target_ncplane = ctx->std;      
    TerminalState *target_state = NULL; 
    Pane *current = ctx->pane_list_head; 

    while (current != NULL) {
        if (current->id == pkt->pane_id) {
            target_ncplane = current->nc_plane;
            target_state = &current->state;
            break;
        }
        current = current->next;
    }

    if (target_state->scroll_offset > 0) {
        if (current) {
            for (size_t j = 0; j < pkt->data_len && current->pending_len < PENDING_BUF_SIZE; j++) {
                current->pending_buf[current->pending_len++] = pkt->payload[j];
            }
        }
    } else {
        for (size_t j = 0; j < pkt->data_len; j++) {
            parse_ansi_byte(target_state, pkt->payload[j], target_ncplane);
        }
    }


    update_active_ui(ctx->nc, ctx->footer, ctx->active_pane, ctx->session_name, ctx->session_count);
    notcurses_render(ctx->nc);
}



// --- ROUTE 2: Control Commands --

    
void horz_split(ClientContext *ctx, CronosPacket *pkt){
    Pane *parent = ctx->pane_list_head; 
    
    while(parent){
        if(parent->id == pkt->pane_id) break; 
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
        
        struct ncplane *border_plane = ncplane_create(ctx->std, &bopts);
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
        
        struct ncplane *new_nc_plane = ncplane_create(ctx->std, &nopts);
        if (new_nc_plane == NULL) {
            FILE *dbg = fopen("/tmp/cronos_debug.log", "a");
            fprintf(dbg, "BUG: Failed to create HORZ plane. Rows: %d\n", lower_height);
            fclose(dbg);
            return; 
        }
        ncplane_set_scrolling(new_nc_plane, true);
        ncplane_set_fg_rgb8(new_nc_plane, 220, 220, 220);

        Pane *new_pane = malloc(sizeof(Pane));
        new_pane->id = pkt->payload[1]; 
        new_pane->nc_plane = new_nc_plane;
        new_pane->border_plane = border_plane;
        new_pane->width = parent->width;
        new_pane->height = lower_height;
        new_pane->y = low_start_y;
        new_pane->x = parent->x;
        
        init_terminal_state(&new_pane->state);
        
        new_pane->next = parent->next;
        parent->next = new_pane;
        ctx->active_pane = new_pane; 

        send_resize_packet(ctx->client_sock, parent->id, parent->height, parent->width);
        send_resize_packet(ctx->client_sock, new_pane->id, new_pane->height, new_pane->width);
        
        update_active_ui(ctx->nc, ctx->footer, ctx->active_pane, ctx->session_name, ctx->session_count);
        notcurses_render(ctx->nc);
    }
}

    
void vert_split(ClientContext *ctx, CronosPacket *pkt){
    Pane *parent = ctx->active_pane; 
    while(parent){
        if(parent->id == pkt->pane_id) break; 
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
        
        struct ncplane *border_plane = ncplane_create(ctx->std, &bopts);
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
        
        struct ncplane *new_nc_plane = ncplane_create(ctx->std, &nopts);
        if (new_nc_plane == NULL) {
            FILE *dbg = fopen("/tmp/cronos_debug.log", "a");
            fprintf(dbg, "BUG: Failed to create VERT plane. Cols: %d\n", right_width);
            fclose(dbg);
            return; 
        }
        ncplane_set_scrolling(new_nc_plane, true);
        ncplane_set_fg_rgb8(new_nc_plane, 220, 220, 220);

        Pane *new_pane = malloc(sizeof(Pane));
        new_pane->id = pkt->payload[1]; 
        new_pane->nc_plane = new_nc_plane;
        new_pane->border_plane = border_plane;
        new_pane->width = right_width;
        new_pane->height = pane_height;
        new_pane->y = parent->y;
        new_pane->x = right_start_x;
        
        init_terminal_state(&new_pane->state);
        
        new_pane->next = parent->next;
        parent->next = new_pane;
        ctx->active_pane = new_pane; 

        send_resize_packet(ctx->client_sock, parent->id, parent->height, parent->width);
        send_resize_packet(ctx->client_sock, new_pane->id, new_pane->height, new_pane->width);
        
        update_active_ui(ctx->nc, ctx->footer, ctx->active_pane, ctx->session_name, ctx->session_count);
        notcurses_render(ctx->nc);
    }
}

void res_pane_close(ClientContext *ctx, CronosPacket *pkt){
    Pane *prev = NULL;
    Pane *curr = ctx->pane_list_head;

    while (curr != NULL) {
        if (curr->id == pkt->pane_id) {
            Pane *absorbed_by = NULL;
            Pane *neighbor = ctx->pane_list_head;

            while (neighbor) {
                // Neighbor to the LEFT -- curr owns the shared border, dies with curr below
                if (neighbor != curr && neighbor->x + neighbor->width + 1 == curr->x && neighbor->y == curr->y) {
                    neighbor->width += curr->width + 1;
                    ncplane_resize(neighbor->nc_plane, 0, 0, neighbor->height, neighbor->width, 0, 0, neighbor->height, neighbor->width);
                    send_resize_packet(ctx->client_sock, neighbor->id, neighbor->height, neighbor->width);
                    absorbed_by = neighbor;
                    break;
                }
                // Neighbor ABOVE -- curr owns the shared border, dies with curr below
                if (neighbor != curr && neighbor->y + neighbor->height + 1 == curr->y && neighbor->x == curr->x) {
                    neighbor->height += curr->height + 1;
                    ncplane_resize(neighbor->nc_plane, 0, 0, neighbor->height, neighbor->width, 0, 0, neighbor->height, neighbor->width);
                    send_resize_packet(ctx->client_sock, neighbor->id, neighbor->height, neighbor->width);
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
                    send_resize_packet(ctx->client_sock, neighbor->id, neighbor->height, neighbor->width);

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
                    send_resize_packet(ctx->client_sock, neighbor->id, neighbor->height, neighbor->width);

                    if (neighbor->border_plane) ncplane_destroy(neighbor->border_plane);
                    neighbor->border_plane = curr->border_plane;
                    curr->border_plane = NULL;
                    absorbed_by = neighbor;
                    break;
                }
                neighbor = neighbor->next;
            }

            if (prev) prev->next = curr->next;
            else ctx->pane_list_head = curr->next;

            ncplane_destroy(curr->nc_plane);
            if (curr->border_plane) ncplane_destroy(curr->border_plane);

            if (ctx->active_pane == curr) {
                ctx->active_pane = absorbed_by ? absorbed_by : ctx->pane_list_head;
            }

            free(curr);
            break;
        }
        prev = curr;
        curr = curr->next;
    }
    update_active_ui(ctx->nc, ctx->footer, ctx->active_pane, ctx->session_name, ctx->session_count);
    notcurses_render(ctx->nc);
}

void handle_pane_focus(ClientContext *ctx, Action act) {
    Pane *p = ctx->pane_list_head;
    while (p) {
        int match = 0;
        if (act == ACTION_FOCUS_LEFT  && p->x + p->width + 1 == ctx->active_pane->x && ctx->active_pane->y >= p->y && ctx->active_pane->y < p->y + p->height) match = 1;
        if (act == ACTION_FOCUS_RIGHT && ctx->active_pane->x + ctx->active_pane->width + 1 == p->x && ctx->active_pane->y >= p->y && ctx->active_pane->y < p->y + p->height) match = 1;
        if (act == ACTION_FOCUS_DOWN  && p->y + p->height + 1 == ctx->active_pane->y && ctx->active_pane->x >= p->x && ctx->active_pane->x < p->x + p->width) match = 1;
        if (act == ACTION_FOCUS_UP    && ctx->active_pane->y + ctx->active_pane->height + 1 == p->y && ctx->active_pane->x >= p->x && ctx->active_pane->x < p->x + p->width) match = 1;
        
        if (match) {
            ctx->active_pane = p;
            break;
        }
        p = p->next;
    }
    update_active_ui(ctx->nc, ctx->footer, ctx->active_pane, ctx->session_name, ctx->session_count);
    notcurses_render(ctx->nc);
}

void handle_pane_resize(ClientContext *ctx, Action act) {
    Pane *neighbor = ctx->pane_list_head;
    while (neighbor) {
        int match = 0;
        
        if (act == ACTION_RESIZE_RIGHT && neighbor->x == ctx->active_pane->x + ctx->active_pane->width + 1 && neighbor->y == ctx->active_pane->y) {
            if (neighbor->width > 4) {
                neighbor->x += 1;
                neighbor->width -= 1;
                ncplane_move_yx(neighbor->nc_plane, neighbor->y, neighbor->x);
                ncplane_resize(neighbor->nc_plane, 0, 0, neighbor->height, neighbor->width, 0, 0, neighbor->height, neighbor->width);
                send_resize_packet(ctx->client_sock, neighbor->id, neighbor->height, neighbor->width);

                ctx->active_pane->width += 1;
                ncplane_resize(ctx->active_pane->nc_plane, 0, 0, ctx->active_pane->height, ctx->active_pane->width, 0, 0, ctx->active_pane->height, ctx->active_pane->width);
                send_resize_packet(ctx->client_sock, ctx->active_pane->id, ctx->active_pane->height, ctx->active_pane->width);

                if (neighbor->border_plane) {
                    ncplane_move_yx(neighbor->border_plane, neighbor->y, ctx->active_pane->x + ctx->active_pane->width);
                }
            }
            match = 1;
        }

        if (match) break;
        neighbor = neighbor->next;
    }
    notcurses_render(ctx->nc);
}