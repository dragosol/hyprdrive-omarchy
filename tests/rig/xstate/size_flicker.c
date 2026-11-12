// size_flicker.c -- an X11 client whose size flickers: it maps a window and,
// at irregular intervals, resizes ITSELF smaller and back a few ms later (a
// ConfigureWindow on its own window each time, as a client-side resize does).
// The overview_rest gate's trigger for the "tiles shake at rest" reports: on
// a build whose grid reads the window's size as it is at that instant, the
// tile and its row neighbour twitch; on one that lays windows out with their
// settled size, nothing moves. Float it (its class is size_flicker): Hyprland
// only honours an X11 client's own configure on a floating window.
//
//   cc -O2 -o size_flicker tests/rig/xstate/size_flicker.c $(pkg-config --cflags --libs xcb)
//   DISPLAY=:N ./size_flicker [seconds] [shrink px] [hold ms] [min gap ms] [max gap ms] [first s] [last s]
//     defaults: 20 s, 15 px, 10 ms, 100..600 ms between flickers, the first
//     one 2 s after the window maps, none after `last` s (default: to the end;
//     the window stays mapped, still)
//   It prints one line per flicker (`flicker <n> t=<s>`) to stdout.
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <xcb/xcb.h>

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static void sleepMs(double ms) {
    struct timespec t = {(time_t)(ms / 1000), (long)((ms - (long)(ms / 1000) * 1000) * 1e6)};
    nanosleep(&t, NULL);
}

static void resize(xcb_connection_t* c, xcb_window_t w, uint32_t width, uint32_t height) {
    const uint32_t v[] = {width, height};
    xcb_configure_window(c, w, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, v);
    xcb_flush(c);
}

// Drain events; repaint the window on Expose so the tile has a picture.
static void pump(xcb_connection_t* c, xcb_window_t w, xcb_gcontext_t gc) {
    xcb_generic_event_t* e;
    while ((e = xcb_poll_for_event(c))) {
        if ((e->response_type & ~0x80) == XCB_EXPOSE) {
            const xcb_rectangle_t r = {0, 0, 4000, 4000};
            xcb_poly_fill_rectangle(c, w, gc, 1, &r);
            xcb_flush(c);
        }
        free(e);
    }
}

int main(int argc, char** argv) {
    const double   SECONDS = argc > 1 ? atof(argv[1]) : 20;
    const uint32_t SHRINK  = argc > 2 ? (uint32_t)atoi(argv[2]) : 15;
    const double   HOLD    = argc > 3 ? atof(argv[3]) : 10;
    const double   GAPMIN  = argc > 4 ? atof(argv[4]) : 100;
    const double   GAPMAX  = argc > 5 ? atof(argv[5]) : 600;
    const double   FIRST   = argc > 6 ? atof(argv[6]) : 2;
    const double   LAST    = argc > 7 ? atof(argv[7]) : SECONDS;
    const uint32_t W = 800, H = 600;

    xcb_connection_t* c = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(c)) {
        fprintf(stderr, "size_flicker: no X display\n");
        return 1;
    }
    xcb_screen_t*      s = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
    const xcb_window_t w = xcb_generate_id(c);
    const uint32_t     vals[] = {0x3a6ea5, XCB_EVENT_MASK_EXPOSURE | XCB_EVENT_MASK_STRUCTURE_NOTIFY};
    xcb_create_window(c, XCB_COPY_FROM_PARENT, w, s->root, 0, 0, W, H, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, s->root_visual, XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, vals);
    const char CLASS[] = "size_flicker\0size_flicker";
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, w, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8, sizeof CLASS, CLASS);
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, w, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8, 12, "size_flicker");
    const xcb_gcontext_t gc   = xcb_generate_id(c);
    const uint32_t       gv[] = {0xd08030};
    xcb_create_gc(c, gc, w, XCB_GC_FOREGROUND, gv);
    xcb_map_window(c, w);
    xcb_flush(c);
    printf("size_flicker 0x%x %ux%u shrink %u hold %.0f ms gaps %.0f..%.0f ms\n", w, W, H, SHRINK, HOLD, GAPMIN, GAPMAX);
    fflush(stdout);

    srand(7);
    const double START = now();
    // Let it map and be placed before the first flicker.
    while (now() - START < FIRST) {
        pump(c, w, gc);
        sleepMs(5);
    }
    int n = 0;
    while (now() - START < SECONDS) {
        if (now() - START >= LAST) {
            pump(c, w, gc);
            sleepMs(5);
            continue;
        }
        // The size it has NOW: the compositor may have placed it at another one.
        xcb_get_geometry_reply_t* g = xcb_get_geometry_reply(c, xcb_get_geometry(c, w), NULL);
        if (!g)
            break;
        const uint32_t CW = g->width, CH = g->height;
        free(g);
        resize(c, w, CW - SHRINK, CH - SHRINK);
        sleepMs(HOLD);
        resize(c, w, CW, CH);
        printf("flicker %d t=%.3f\n", ++n, now() - START);
        fflush(stdout);
        const double GAP  = GAPMIN + (GAPMAX - GAPMIN) * (rand() / (double)RAND_MAX);
        const double NEXT = now() + GAP / 1000.0;
        while (now() < NEXT) {
            pump(c, w, gc);
            sleepMs(5);
        }
    }
    xcb_disconnect(c);
    return 0;
}
