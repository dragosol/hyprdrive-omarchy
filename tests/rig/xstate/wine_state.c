// wine_state.c -- an X11 client that insists on a _NET_WM_STATE the way Wine
// does: it maps a window, and every time _NET_WM_STATE changes (PropertyNotify)
// or the window is configured (ConfigureNotify), if the property lacks the
// state it wants it sends _NET_WM_STATE ADD for it to the root window. Each
// second it prints how many requests it sent and how often the property it
// read back changed. See README.md next to it.
//
//   cc -O2 -o wine_state tests/rig/xstate/wine_state.c $(pkg-config --cflags --libs xcb)
//   DISPLAY=:N ./wine_state [max|fs] [seconds]
//     max (default): wants MAXIMIZED_VERT + MAXIMIZED_HORZ (one message, both atoms)
//     fs:            wants FULLSCREEN
//     flap:          the storm-guard case: asks for the OPPOSITE of what it
//                    reads (FULLSCREEN there -> REMOVE it, absent -> ADD it)
//     seconds:       exit after this long (default 20)
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <xcb/xcb.h>

static xcb_connection_t* c;
static xcb_atom_t        A_STATE, A_FS, A_MV, A_MH, A_NAME, A_UTF8;

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

// Bit 0 FULLSCREEN, 1 MAXIMIZED_VERT, 2 MAXIMIZED_HORZ.
static int readState(xcb_window_t w) {
    xcb_get_property_reply_t* r = xcb_get_property_reply(c, xcb_get_property(c, 0, w, A_STATE, XCB_ATOM_ATOM, 0, 32), NULL);
    int                       s = 0;
    if (r) {
        const xcb_atom_t* v = xcb_get_property_value(r);
        int               n = xcb_get_property_value_length(r) / 4;
        for (int i = 0; i < n; ++i)
            s |= v[i] == A_FS ? 1 : v[i] == A_MV ? 2 : v[i] == A_MH ? 4 : 0;
        free(r);
    }
    return s;
}

static void request(xcb_window_t root, xcb_window_t w, int fs, uint32_t action) {
    xcb_client_message_event_t e;
    memset(&e, 0, sizeof e);
    e.response_type  = XCB_CLIENT_MESSAGE;
    e.format         = 32;
    e.window         = w;
    e.type           = A_STATE;
    e.data.data32[0] = action; // 0 REMOVE, 1 ADD
    e.data.data32[1] = fs ? A_FS : A_MV;
    e.data.data32[2] = fs ? XCB_ATOM_NONE : A_MH;
    e.data.data32[3] = 1; // source: application
    xcb_send_event(c, 0, root, XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY, (const char*)&e);
    xcb_flush(c);
}

int main(int argc, char** argv) {
    const int    FLAP    = argc > 1 && strcmp(argv[1], "flap") == 0;
    const int    FS      = FLAP || (argc > 1 && strcmp(argv[1], "fs") == 0);
    const double SECONDS = argc > 2 ? atof(argv[2]) : 20;
    c                    = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(c)) {
        fprintf(stderr, "wine_state: cannot open DISPLAY\n");
        return 2;
    }
    xcb_screen_t* scr = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
    A_STATE = atom("_NET_WM_STATE"), A_FS = atom("_NET_WM_STATE_FULLSCREEN"), A_MV = atom("_NET_WM_STATE_MAXIMIZED_VERT"),
    A_MH = atom("_NET_WM_STATE_MAXIMIZED_HORZ"), A_NAME = atom("_NET_WM_NAME"), A_UTF8 = atom("UTF8_STRING");

    xcb_window_t   w      = xcb_generate_id(c);
    const uint32_t vals[] = {scr->black_pixel, XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_STRUCTURE_NOTIFY};
    xcb_create_window(c, XCB_COPY_FROM_PARENT, w, scr->root, 0, 0, 800, 600, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, scr->root_visual,
                      XCB_CW_BACK_PIXEL | XCB_CW_EVENT_MASK, vals);
    const char* title = FLAP ? "wine-state flap" : FS ? "wine-state fs" : "wine-state max";
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, w, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8, strlen(title), title);
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, w, A_NAME, A_UTF8, 8, strlen(title), title);
    const char cls[] = "wine_state.exe\0wine_state.exe"; // instance\0class: an .exe class, as Wine gives a game
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, w, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8, sizeof cls, cls);
    xcb_map_window(c, w);
    xcb_flush(c);

    const int    WANT  = FS ? 1 : 2; // the bit whose absence triggers a request
    const double START = now();
    double       tick  = START + 1;
    int          sent = 0, changes = 0, totalSent = 0, totalChanges = 0, last = -1, mapped = 0;
    struct pollfd pfd = {.fd = xcb_get_file_descriptor(c), .events = POLLIN};
    printf("wine_state: window 0x%x wants %s\n", w, FLAP ? "the opposite of FULLSCREEN, every time" : FS ? "FULLSCREEN" : "MAXIMIZED_VERT+HORZ");
    fflush(stdout);
    for (;;) {
        double t = now();
        if (t - START >= SECONDS)
            break;
        if (t >= tick) {
            printf("t=%2.0fs sent=%d changes=%d state=%s%s%s\n", tick - START, sent, changes, last >= 0 && (last & 1) ? "FS " : "",
                   last >= 0 && (last & 2) ? "MAXV " : "", last >= 0 && (last & 4) ? "MAXH" : "");
            fflush(stdout);
            sent = changes = 0;
            tick += 1;
            continue;
        }
        poll(&pfd, 1, (int)((tick - t) * 1000) + 1);
        xcb_generic_event_t* ev;
        while ((ev = xcb_poll_for_event(c))) {
            const int type = ev->response_type & 0x7f;
            int       look = 0;
            if (type == XCB_MAP_NOTIFY)
                mapped = look = 1;
            else if (type == XCB_CONFIGURE_NOTIFY)
                look = 1;
            else if (type == XCB_PROPERTY_NOTIFY && ((xcb_property_notify_event_t*)ev)->atom == A_STATE)
                look = 1;
            free(ev);
            if (!look || !mapped)
                continue;
            const int s = readState(w);
            if (s != last) {
                ++changes, ++totalChanges;
                last = s;
            }
            if (FLAP) {
                request(scr->root, w, 1, (s & 1) ? 0 : 1);
                ++sent, ++totalSent;
            } else if (!(s & WANT)) {
                request(scr->root, w, FS, 1);
                ++sent, ++totalSent;
            }
        }
        if (xcb_connection_has_error(c))
            break;
    }
    printf("wine_state: total sent=%d changes=%d over %.0fs\n", totalSent, totalChanges, SECONDS);
    xcb_disconnect(c);
    return 0;
}
