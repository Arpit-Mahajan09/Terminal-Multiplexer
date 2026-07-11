#define _XOPEN_SOURCE 600
#include <notcurses/notcurses.h>

struct notcurses_options opts = { 
    .loglevel = NCLOGLEVEL_PANIC,
    .flags = NCOPTION_SUPPRESS_BANNERS 
};


int main(int argc, char **argv) {
    // 1. Initialize Notcurses
    struct notcurses_options opts = { .loglevel = NCLOGLEVEL_PANIC };
    struct notcurses *nc = notcurses_init(&opts, stdout);
    if (!nc) return 1;

    // 2. Get the standard plane
    struct ncplane *std = notcurses_stdplane(nc);

    // 3. Print text at a specific row/col
    ncplane_putstr_yx(std, 5, 10, "Hello, Notcurses!");

    // 4. Render the UI
    notcurses_render(nc);

    // 5. Wait for user input
    notcurses_get(nc, NULL, NULL);

    // 6. Teardown
    notcurses_stop(nc);
    return 0;
}
