// slow_redraw.c -- an X11 client that behaves like the Battle.net launcher (a
// Wine/CEF window) under a fullscreen trip: it has a minimum and a preferred
// size, it repaints only some time after each resize, and it can insist on its
// own size. See README.md next to it.
//
//   cc -O2 -o slow_redraw tests/rig/xstate/slow_redraw.c $(pkg-config --cflags --libs xcb)
//   DISPLAY=:N ./slow_redraw [--lag MS] [--flash] [--insist] [--size WxH] [--min WxH] [--seconds S]
//     --lag MS     repaint the whole window (solid orange) only MS ms after the
//                  last ConfigureNotify that changed its size (default 150)
//     --flash      at once on a resize, paint the window WHITE (what CEF shows
//                  before its first frame at a new size), then orange after --lag
//     --insist     after a size change that is neither its preferred size nor a
//                  fullscreen one, ask (ConfigureRequest) for its preferred size
//                  again, --lag ms later -- as the launcher fights a tile
//     --echo       Wine's state sync: told a size other than the screen's while
//                  _NET_WM_STATE has FULLSCREEN, it asks the WM to REMOVE
//                  FULLSCREEN (it no longer covers the screen)
//     --size WxH   preferred (and initial) size, default 900x620
//     --min WxH    WM_NORMAL_HINTS minimum size, default 640x480
// It honours _NET_WM_STATE_FULLSCREEN as any client does: the WM sizes it. It
// prints one line per resize: "t=<ms> size WxH fs=<0|1>" and per repaint.
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

static void fill(xcb_window_t w, xcb_gcontext_t gc, uint32_t rgb, uint16_t W, uint16_t H) {
    xcb_change_gc(c, gc, XCB_GC_FOREGROUND, &rgb);
    xcb_rectangle_t r = {0, 0, W, H};
    xcb_poly_fill_rectangle(c, w, gc, 1, &r);
    xcb_flush(c);
}

int main(int argc, char** argv) {
    double lag = 0.150, seconds = 120;
    int    flash = 0, insist = 0, echo = 0, pw = 900, ph = 620, mw = 640, mh = 480;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--lag") && i + 1 < argc)
            lag = atof(argv[++i]) / 1000.0;
        else if (!strcmp(argv[i], "--flash"))
            flash = 1;
        else if (!strcmp(argv[i], "--echo"))
            echo = 1;
        else if (!strcmp(argv[i], "--insist"))
            insist = 1;
        else if (!strcmp(argv[i], "--size") && i + 1 < argc)
            sscanf(argv[++i], "%dx%d", &pw, &ph);
        else if (!strcmp(argv[i], "--min") && i + 1 < argc)
            sscanf(argv[++i], "%dx%d", &mw, &mh);
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc)
            seconds = atof(argv[++i]);
    }
    c = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(c)) {
        fprintf(stderr, "slow_redraw: cannot open DISPLAY\n");
        return 2;
    }
    xcb_screen_t*    scr = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
    const xcb_atom_t A_STATE = atom("_NET_WM_STATE"), A_FS = atom("_NET_WM_STATE_FULLSCREEN"), A_NAME = atom("_NET_WM_NAME"), A_UTF8 = atom("UTF8_STRING");

    xcb_window_t w = xcb_generate_id(c);
    // No background: the server never clears it, so what shows between a
    // resize and the repaint is whatever the client last drew (or nothing).
    const uint32_t vals[] = {XCB_BACK_PIXMAP_NONE, XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_STRUCTURE_NOTIFY | XCB_EVENT_MASK_EXPOSURE};
    xcb_create_window(c, XCB_COPY_FROM_PARENT, w, scr->root, 0, 0, pw, ph, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, scr->root_visual,
                      XCB_CW_BACK_PIXMAP | XCB_CW_EVENT_MASK, vals);
    const char title[] = "slow redraw";
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, w, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8, strlen(title), title);
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, w, A_NAME, A_UTF8, 8, strlen(title), title);
    const char cls[] = "slow_redraw.exe\0slow_redraw.exe";
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, w, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8, sizeof cls, cls);
    // WM_NORMAL_HINTS: flags PSize(8)|PMinSize(16); 18 CARD32s
    uint32_t hints[18] = {0};
    hints[0] = 8 | 16;
    hints[3] = pw, hints[4] = ph, hints[5] = mw, hints[6] = mh;
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, w, XCB_ATOM_WM_NORMAL_HINTS, XCB_ATOM_WM_SIZE_HINTS, 32, 18, hints);
    xcb_gcontext_t gc = xcb_generate_id(c);
    const uint32_t fg = 0xff8800;
    xcb_create_gc(c, gc, w, XCB_GC_FOREGROUND, &fg);
    xcb_map_window(c, w);
    xcb_flush(c);
    printf("slow_redraw: window 0x%x lag=%.0fms flash=%d insist=%d size=%dx%d min=%dx%d\n", w, lag * 1000, flash, insist, pw, ph, mw, mh);
    fflush(stdout);

    const double  START = now();
    int           W = pw, H = ph, drawnW = -1, drawnH = -1, fs = 0, first = 1;
    double        paintAt = 0, insistAt = 0;
    struct pollfd pfd = {.fd = xcb_get_file_descriptor(c), .events = POLLIN};
    for (;;) {
        const double t = now();
        if (t - START >= seconds)
            break;
        if (paintAt > 0 && t >= paintAt) {
            paintAt = 0;
            fill(w, gc, 0xff8800, W, H);
            drawnW = W, drawnH = H;
            printf("t=%.0f painted %dx%d\n", (t - START) * 1000, W, H);
            fflush(stdout);
        }
        if (insistAt > 0 && t >= insistAt) {
            insistAt = 0;
            if (!fs && (W != pw || H != ph)) {
                const uint32_t sz[] = {(uint32_t)pw, (uint32_t)ph};
                xcb_configure_window(c, w, XCB_CONFIG_WINDOW_WIDTH | XCB_CONFIG_WINDOW_HEIGHT, sz);
                xcb_flush(c);
                printf("t=%.0f insists on %dx%d\n", (t - START) * 1000, pw, ph);
                fflush(stdout);
            }
        }
        double wake = 1.0;
        if (paintAt > 0 && paintAt - t < wake)
            wake = paintAt - t;
        if (insistAt > 0 && insistAt - t < wake)
            wake = insistAt - t;
        poll(&pfd, 1, (int)(wake * 1000) + 1);
        xcb_generic_event_t* ev;
        while ((ev = xcb_poll_for_event(c))) {
            const int type = ev->response_type & 0x7f;
            if (type == XCB_CONFIGURE_NOTIFY) {
                const xcb_configure_notify_event_t* e = (xcb_configure_notify_event_t*)ev;
                if (e->window == w && (e->width != W || e->height != H)) {
                    W = e->width, H = e->height;
                    printf("t=%.0f size %dx%d fs=%d\n", (now() - START) * 1000, W, H, fs);
                    fflush(stdout);
                    if (flash)
                        fill(w, gc, 0xffffff, W, H);
                    paintAt = now() + lag;
                    if (insist)
                        insistAt = now() + lag;
                    if (echo && fs && (W != scr->width_in_pixels || H != scr->height_in_pixels)) {
                        xcb_client_message_event_t m;
                        memset(&m, 0, sizeof m);
                        m.response_type  = XCB_CLIENT_MESSAGE;
                        m.format         = 32;
                        m.window         = w;
                        m.type           = A_STATE;
                        m.data.data32[0] = 0; // REMOVE
                        m.data.data32[1] = A_FS;
                        m.data.data32[3] = 1;
                        xcb_send_event(c, 0, scr->root, XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY, (const char*)&m);
                        xcb_flush(c);
                        printf("t=%.0f echo: not covering the screen, REMOVE FULLSCREEN\n", (now() - START) * 1000);
                        fflush(stdout);
                    }
                }
            } else if (type == XCB_EXPOSE) {
                // Exposed at the size it has drawn: repaint at once; else the
                // lagged repaint does it.
                if ((W == drawnW && H == drawnH) || first) {
                    first  = 0;
                    drawnW = W, drawnH = H;
                    fill(w, gc, 0xff8800, W, H);
                } else if (paintAt == 0)
                    paintAt = now() + lag;
            } else if (type == XCB_PROPERTY_NOTIFY && ((xcb_property_notify_event_t*)ev)->atom == A_STATE) {
                xcb_get_property_reply_t* r = xcb_get_property_reply(c, xcb_get_property(c, 0, w, A_STATE, XCB_ATOM_ATOM, 0, 32), NULL);
                int                       f = 0;
                if (r) {
                    const xcb_atom_t* v = xcb_get_property_value(r);
                    for (int i = 0; i < xcb_get_property_value_length(r) / 4; ++i)
                        f |= v[i] == A_FS;
                    free(r);
                }
                if (f != fs) {
                    fs = f;
                    printf("t=%.0f fullscreen=%d\n", (now() - START) * 1000, fs);
                    fflush(stdout);
                }
            }
            free(ev);
        }
        if (xcb_connection_has_error(c))
            break;
    }
    xcb_disconnect(c);
    return 0;
}
