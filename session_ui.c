#include "headers/session_ui.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <notcurses/notcurses.h>

#define MAX_SESSIONS 32

extern struct termios orig_t;
extern int raw_mode_active;
extern struct notcurses *nc_global;

// ── collect session names ─────────────────────────────────────

static int get_sessions(char names[][128]) {
    DIR *dir = opendir("/tmp");
    if (!dir) return 0;
    int count = 0;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL && count < MAX_SESSIONS) {
        if (strncmp(e->d_name, "cronos_", 7) == 0 && strstr(e->d_name, ".sock")) {
            // Extract name between cronos_ and .sock
            size_t len = strlen(e->d_name) - 12;  // 7 prefix + 5 suffix (.sock)
            if (len > 0 && len < 128) {
                strncpy(names[count], e->d_name + 7, len);
                names[count][len] = '\0';
                count++;
            }
        }
    }
    closedir(dir);
    return count;
}

// ── rename helper ─────────────────────────────────────────────

static void do_rename(ClientContext *ctx, const char *new_name) {
    char old_sock[256], new_sock[256];
    char old_pid[256],  new_pid[256];
    snprintf(old_sock, sizeof(old_sock), "/tmp/cronos_%s.sock", ctx->session_name);
    snprintf(new_sock, sizeof(new_sock), "/tmp/cronos_%s.sock", new_name);
    snprintf(old_pid,  sizeof(old_pid),  "/tmp/cronos_%s.pid",  ctx->session_name);
    snprintf(new_pid,  sizeof(new_pid),  "/tmp/cronos_%s.pid",  new_name);

    rename(old_sock, new_sock);
    rename(old_pid,  new_pid);

    // Tell the server to update its internal paths
    CronosPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.type     = PKT_TYPE_COMMAND;
    pkt.data_len = strlen(new_name) + 1;
    pkt.payload[0] = REQ_RENAME_SESSION;
    strncpy(&pkt.payload[1], new_name, BUFFER_SIZE - 2);
    write(ctx->client_sock, &pkt, sizeof(pkt));

    strncpy(ctx->session_name, new_name, sizeof(ctx->session_name) - 1);
}

// ── inline text input using notcurses ────────────────────────

static void read_line_nc(struct notcurses *nc, struct ncplane *plane,
                         int y, int x, char *buf, size_t bufsz) {
    size_t pos = 0;
    memset(buf, 0, bufsz);
    ncinput ni;
    uint32_t id;
    while ((id = notcurses_get_blocking(nc, &ni)) != (uint32_t)-1) {
        if (id == '\r' || id == NCKEY_ENTER) break;
        if ((id == NCKEY_BACKSPACE || id == 0x7F) && pos > 0) {
            buf[--pos] = '\0';
        } else if (id < 0x80 && id >= 0x20 && pos + 1 < bufsz) {
            buf[pos++] = (char)id;
        }
        // Redraw input field
        ncplane_printf_yx(plane, y, x, "%-30s", buf);
        notcurses_render(nc);
    }
}

// ── main overlay ──────────────────────────────────────────────

void show_session_ui(ClientContext *ctx) {
    char session_names[MAX_SESSIONS][128];
    int count = get_sessions(session_names);

    unsigned int dimy, dimx;
    ncplane_dim_yx(ctx->std, &dimy, &dimx);

    // Overlay panel dimensions
    int panel_h = count + 8;
    int panel_w = 42;
    int panel_y = ((int)dimy - panel_h) / 2;
    int panel_x = ((int)dimx - panel_w) / 2;
    if (panel_y < 0) panel_y = 0;
    if (panel_x < 0) panel_x = 0;

    struct ncplane_options opts = {
        .y = panel_y, .x = panel_x,
        .rows = (unsigned)panel_h, .cols = (unsigned)panel_w,
    };
    struct ncplane *panel = ncplane_create(ctx->std, &opts);
    ncplane_move_top(panel);

    // Style
    uint64_t bg_ch = 0;
    ncchannels_set_fg_rgb8(&bg_ch, 220, 220, 220);
    ncchannels_set_bg_rgb8(&bg_ch, 30,  30,  80);
    ncplane_set_base(panel, " ", 0, bg_ch);

    int selected = 0;
    // Pre-select current session
    for (int i = 0; i < count; i++) {
        if (strcmp(session_names[i], ctx->session_name) == 0) { selected = i; break; }
    }

    int redraw = 1;
    ncinput ni;
    uint32_t id;

    while (1) {
        if (redraw) {
            ncplane_erase(panel);
            // Border
            ncplane_printf_yx(panel, 0, 0, "╔════════════════════════════════════════╗");
            ncplane_printf_yx(panel, 1, 0, "║         Session Manager                ║");
            ncplane_printf_yx(panel, 2, 0, "╠════════════════════════════════════════╣");

            for (int i = 0; i < count; i++) {
                bool is_current = strcmp(session_names[i], ctx->session_name) == 0;
                bool is_sel = (i == selected);
                ncplane_printf_yx(panel, 3 + i, 0, "║ %s %s%-32s%s ║",
                    is_sel     ? "▶" : " ",
                    is_current ? "*" : " ",
                    session_names[i],
                    "");
            }

            int bot = 3 + count;
            ncplane_printf_yx(panel, bot,   0, "╠════════════════════════════════════════╣");
            ncplane_printf_yx(panel, bot+1, 0, "║ [Enter]Switch [N]ew [R]ename [ESC]Back║");
            ncplane_printf_yx(panel, bot+2, 0, "╚════════════════════════════════════════╝");

            ncplane_move_top(panel);
            notcurses_render(ctx->nc);
            redraw = 0;
        }

        id = notcurses_get_blocking(ctx->nc, &ni);
        if (id == (uint32_t)-1) break;

        if (id == NCKEY_ESC || (id == 'q' && !ni.ctrl)) {
            break;

        } else if (id == NCKEY_UP) {
            if (selected > 0) selected--;
            redraw = 1;

        } else if (id == NCKEY_DOWN) {
            if (selected < count - 1) selected++;
            redraw = 1;

        } else if (id == '\r' || id == NCKEY_ENTER) {
            // Switch to selected session
            if (strcmp(session_names[selected], ctx->session_name) == 0) {
                break;  // already current, just close
            }
            // Detach from current, exec into new session
            ncplane_destroy(panel);
            notcurses_stop(ctx->nc);
            nc_global = NULL;
            if (raw_mode_active) {
                tcsetattr(STDIN_FILENO, TCSANOW, &orig_t);
                raw_mode_active = 0;
            }
            execlp("cronos", "cronos", "attach", session_names[selected], NULL);
            // If exec fails, fall back
            _exit(1);

        } else if (id == 'n' || id == 'N') {
            // Create new session — prompt for name
            int bot = 3 + count;
            ncplane_printf_yx(panel, bot+1, 0,
                "║ New session name: %-21s ║", "");
            notcurses_render(ctx->nc);

            char new_name[64] = {0};
            read_line_nc(ctx->nc, panel, bot+1, 19, new_name, sizeof(new_name));

            if (strlen(new_name) > 0) {
                ncplane_destroy(panel);
                notcurses_stop(ctx->nc);
                nc_global = NULL;
                if (raw_mode_active) {
                    tcsetattr(STDIN_FILENO, TCSANOW, &orig_t);
                    raw_mode_active = 0;
                }
                // Fork a child to start the server daemon, then exec attach
                pid_t pid = fork();
                if (pid == 0) {
                    int devnull = open("/dev/null", O_RDWR);
                    if (devnull >= 0) {
                        dup2(devnull, 0); dup2(devnull, 1); dup2(devnull, 2);
                        close(devnull);
                    }
                    setsid();
                    execlp("cronos-server", "cronos-server", new_name, NULL);
                    _exit(1);
                }
                usleep(300000);  // wait for server socket
                execlp("cronos", "cronos", "attach", new_name, NULL);
                _exit(1);
            }
            redraw = 1;

        } else if (id == 'r' || id == 'R') {
            // Rename current session
            int bot = 3 + count;
            ncplane_printf_yx(panel, bot+1, 0,
                "║ Rename to: %-27s ║", "");
            notcurses_render(ctx->nc);

            char new_name[64] = {0};
            read_line_nc(ctx->nc, panel, bot+1, 12, new_name, sizeof(new_name));

            if (strlen(new_name) > 0) {
                do_rename(ctx, new_name);
                // Refresh session list
                count = get_sessions(session_names);
                selected = 0;
                for (int i = 0; i < count; i++) {
                    if (strcmp(session_names[i], ctx->session_name) == 0) {
                        selected = i; break;
                    }
                }
            }
            redraw = 1;
        }
    }

    ncplane_destroy(panel);
    notcurses_render(ctx->nc);
}