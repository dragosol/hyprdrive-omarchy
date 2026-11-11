// fake_wine -- an X11 client that looks like a Wine/Proton program to
// AppIdentity (tests/gates/identity.sh): it sets WM_CLASS, _NET_WM_PID and a
// 64x64 _NET_WM_ICON, and runs with argv[0] rewritten to a Windows path, as
// Wine does, so /proc/<pid>/cmdline says C:\...\X.exe. The environment
// (SteamAppId, WINEPREFIX, GAME_NAME, ...) is whatever it is exec'd with.
//
//   fake_wine CLASS 'C:\Games\Alpha\Alpha.exe' TITLE SECONDS [WxH] [RGB]
//
// Prints "xid=0x... pid=N" on stdout once mapped. Builds with
// `cc -O2 fake_wine.c $(pkg-config --cflags --libs xcb)`.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xcb/xcb.h>

static xcb_atom_t atom(xcb_connection_t* c, const char* n) {
    xcb_intern_atom_reply_t* r = xcb_intern_atom_reply(c, xcb_intern_atom(c, 0, (uint16_t)strlen(n), n), NULL);
    xcb_atom_t               a = r ? r->atom : 0;
    free(r);
    return a;
}

int main(int argc, char** argv) {
    if (argc < 5) {
        fprintf(stderr, "usage: fake_wine CLASS ARGV0 TITLE SECONDS [WxH] [RRGGBB]\n");
        return 2;
    }
    // Wine's argv[0]: re-exec once with it.
    if (strcmp(argv[0], argv[2]) != 0 && !getenv("FAKE_WINE_EXECED")) {
        setenv("FAKE_WINE_EXECED", "1", 1);
        argv[0] = argv[2];
        execv("/proc/self/exe", argv);
        perror("execv");
        return 1;
    }
    const char* CLS = argv[1];
    int         w = 640, h = 400;
    if (argc > 5)
        sscanf(argv[5], "%dx%d", &w, &h);
    const uint32_t RGB = argc > 6 ? (uint32_t)strtoul(argv[6], NULL, 16) : 0x3366cc;

    xcb_connection_t* c = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(c))
        return 1;
    xcb_screen_t* s   = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
    xcb_window_t  win = xcb_generate_id(c);
    uint32_t      vals[] = {s->white_pixel};
    xcb_create_window(c, XCB_COPY_FROM_PARENT, win, s->root, 0, 0, (uint16_t)w, (uint16_t)h, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, s->root_visual, XCB_CW_BACK_PIXEL, vals);

    // WM_CLASS "instance\0class\0": Wine uses the lowercase exe for both.
    char   wmclass[512];
    size_t n = (size_t)snprintf(wmclass, sizeof wmclass, "%s", CLS) + 1;
    n += (size_t)snprintf(wmclass + n, sizeof wmclass - n, "%s", CLS) + 1;
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, win, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 8, (uint32_t)n, wmclass);
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, win, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8, (uint32_t)strlen(argv[3]), argv[3]);
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, win, atom(c, "_NET_WM_NAME"), atom(c, "UTF8_STRING"), 8, (uint32_t)strlen(argv[3]), argv[3]);
    const uint32_t PID = (uint32_t)getpid();
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, win, atom(c, "_NET_WM_PID"), XCB_ATOM_CARDINAL, 32, 1, &PID);
    static uint32_t icon[2 + 64 * 64];
    icon[0] = icon[1] = 64;
    for (int i = 0; i < 64 * 64; ++i)
        icon[2 + i] = 0xff000000u | RGB;
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, win, atom(c, "_NET_WM_ICON"), XCB_ATOM_CARDINAL, 32, 2 + 64 * 64, icon);
    xcb_map_window(c, win);
    xcb_flush(c);
    printf("xid=0x%x pid=%u\n", win, PID);
    fflush(stdout);
    sleep((unsigned)atoi(argv[4]));
    xcb_disconnect(c);
    return 0;
}
