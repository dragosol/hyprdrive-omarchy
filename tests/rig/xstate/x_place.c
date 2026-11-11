// x_place.c -- an X11 client that maps one window of a chosen KIND, the way
// Wine / Proton / toolkits ask for floating, for the placement gates
// (tests/gates/placement.sh, beside.sh). It only maps the window and keeps it
// up; it never fights the compositor.
//
//   cc -O2 -o x_place tests/rig/xstate/x_place.c $(pkg-config --cflags --libs xcb)
//   DISPLAY=:N ./x_place KIND CLASS [seconds] [W H]
//     normal  a plain toplevel (tiles everywhere)
//     dialog  _NET_WM_WINDOW_TYPE_DIALOG + WM_TRANSIENT_FOR the root (a file chooser, a Wine dialog)
//     fixed   WM_NORMAL_HINTS min == max == W x H (Battle.net's login, a Wine dialog)
//     splash  _NET_WM_WINDOW_TYPE_SPLASH (a loading screen)
//     menu    override-redirect (a Wine menu, a tooltip): never managed
//     seconds exit after this long (default 30); W H default 480 360
//   X_PLACE_REMAP=<s>  unmap the window at s seconds and map it again 0.6 s
//                      later -- a Wine remap: the compositor sees a new window
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <xcb/xcb.h>

static xcb_connection_t* c;

static xcb_atom_t atom(const char* n) {
    xcb_intern_atom_reply_t* r = xcb_intern_atom_reply(c, xcb_intern_atom(c, 0, strlen(n), n), NULL);
    xcb_atom_t               a = r ? r->atom : XCB_ATOM_NONE;
    free(r);
    return a;
}

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: x_place normal|dialog|fixed|splash|menu CLASS [seconds] [W H]\n");
        return 2;
    }
    const char*  KIND    = argv[1];
    const char*  CLASS   = argv[2];
    const double SECONDS = argc > 3 ? atof(argv[3]) : 30;
    const int    W       = argc > 5 ? atoi(argv[4]) : 480;
    const int    H       = argc > 5 ? atoi(argv[5]) : 360;
    c                    = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(c)) {
        fprintf(stderr, "x_place: cannot open DISPLAY\n");
        return 2;
    }
    xcb_screen_t*  scr    = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
    const int      MENU   = strcmp(KIND, "menu") == 0;
    xcb_window_t   w      = xcb_generate_id(c);
    const uint32_t vals[] = {scr->white_pixel, MENU, XCB_EVENT_MASK_STRUCTURE_NOTIFY};
    xcb_create_window(c, XCB_COPY_FROM_PARENT, w, scr->root, 100, 100, W, H, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, scr->root_visual,
                      XCB_CW_BACK_PIXEL | XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK, vals);
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, w, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8, strlen(CLASS), CLASS);
    char cls[256];
    const int n = snprintf(cls, sizeof cls, "%s%c%s", CLASS, 0, CLASS) + 1; // instance\0class\0
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, w, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8, n, cls);
    const xcb_atom_t TYPE = atom("_NET_WM_WINDOW_TYPE");
    if (strcmp(KIND, "dialog") == 0) {
        const xcb_atom_t D = atom("_NET_WM_WINDOW_TYPE_DIALOG");
        xcb_change_property(c, XCB_PROP_MODE_REPLACE, w, TYPE, XCB_ATOM_ATOM, 32, 1, &D);
        xcb_change_property(c, XCB_PROP_MODE_REPLACE, w, XCB_ATOM_WM_TRANSIENT_FOR, XCB_ATOM_WINDOW, 32, 1, &scr->root);
    } else if (strcmp(KIND, "splash") == 0) {
        const xcb_atom_t S = atom("_NET_WM_WINDOW_TYPE_SPLASH");
        xcb_change_property(c, XCB_PROP_MODE_REPLACE, w, TYPE, XCB_ATOM_ATOM, 32, 1, &S);
    } else if (strcmp(KIND, "fixed") == 0) {
        // WM_SIZE_HINTS: flags, x, y, w, h, min w/h, max w/h, inc, aspect, base, gravity
        uint32_t hints[18] = {0};
        hints[0]           = 16 | 32; // PMinSize | PMaxSize
        hints[5] = hints[7] = W;
        hints[6] = hints[8] = H;
        xcb_change_property(c, XCB_PROP_MODE_REPLACE, w, XCB_ATOM_WM_NORMAL_HINTS, XCB_ATOM_WM_SIZE_HINTS, 32, 18, hints);
    }
    xcb_map_window(c, w);
    xcb_flush(c);
    printf("x_place: window 0x%x kind %s class %s\n", w, KIND, CLASS);
    fflush(stdout);

    const double  START     = now();
    const char*   REMAP_ENV = getenv("X_PLACE_REMAP");
    const double  REMAP     = REMAP_ENV ? START + atof(REMAP_ENV) : 0;
    int           remapped  = 0;
    struct pollfd pfd       = {.fd = xcb_get_file_descriptor(c), .events = POLLIN};
    for (;;) {
        const double t = now();
        if (t - START >= SECONDS)
            break;
        if (REMAP > 0 && remapped == 0 && t >= REMAP) {
            xcb_unmap_window(c, w);
            xcb_flush(c);
            remapped = 1;
            printf("x_place: unmapped\n");
            fflush(stdout);
        } else if (remapped == 1 && t >= REMAP + 0.6) {
            xcb_map_window(c, w);
            xcb_flush(c);
            remapped = 2;
            printf("x_place: mapped again\n");
            fflush(stdout);
        }
        poll(&pfd, 1, 50);
        xcb_generic_event_t* ev;
        while ((ev = xcb_poll_for_event(c)))
            free(ev);
        if (xcb_connection_has_error(c))
            break;
    }
    xcb_disconnect(c);
    return 0;
}
