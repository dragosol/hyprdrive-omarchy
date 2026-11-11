#!/usr/bin/env bash
# The tests that need NO compositor: pure functions, compiled and run on the
# spot. `make check` runs this, which is the whole point -- a host test nobody
# runs is a comment with a build cost.
#
# Three of them today:
#   drop_table_test   Hyprdrive::Drop::resolve(), all 65536 fact
#                     combinations against the pre-split branch ladder.
#   geometry_test     Hyprdrive::Geometry -- the overview's transform,
#                     clamps and box predicates.
#   search_letters_test  SearchLetters -- Serpantinum Input.qml bounce eases,
#                     UTF-8 split, prefix/suffix diff, animator state.
#   icon_size_test    IconSize.hpp: icon texture sizes (the drawn size,
#                     never upscaled) and a downscale that filters.
#   thumbs_test       Thumbs.cpp against a stand-in thumbnailer: a file still
#                     being written waits until it settles and keeps the
#                     thumbnail it had meanwhile; a future mtime gets one.
#   search_providers_nobus_test  the GNOME search worker with no session bus
#                     (the std::terminate that took the compositor down).
#   trash_dirs_test   TrashDirs.hpp: /proc/mounts decoding (a backslash in a
#                     drive's label threw on the render thread), and Empty
#                     Trash's walk, which never follows a disk's symlinks.
#   store_file_test   StoreFile.hpp: the stacks store survives a full disk
#                     (a short write, /dev/full) byte for byte.
#   search_providers_deadbus_test  the search worker when a private
#                     dbus-daemon dies under it: no 100% CPU spin, and it
#                     reconnects (skipped without dbus-daemon).
#   file_uri_test     FileUri.hpp: file names with ' \ # % ? and spaces as
#                     URIs, and Show in Files reaching a stand-in FileManager1.
#   launcher_states_test  LauncherStates.hpp: badge and progress state is its
#                     sender's, and goes when the sender leaves the bus.
#   debug_paths_test  DebugPaths.hpp: debug logs under $XDG_RUNTIME_DIR, /tmp
#                     only without one.
#   desktop_plan_test  DesktopPlan.hpp: a renumber plan is refused whole when
#                     it would put two desktops on one number or one below 1
#                     (shift(0, -1)); heal pulls apart desktops sharing one.
#   fullscreen_book_test  FullscreenBook.hpp: a fullscreen trip's places are
#                     kept by number (never place -1), the payback decision,
#                     and random fullscreen sequences that must give back the
#                     desktop numbers they started with.
#   motion_math_test  MotionMath.hpp: each shared motion function against
#                     verbatim copies of the definitions it replaced, bit for bit.
#
# Anything that needs a running Hyprland is a GATE (tests/gates/), not a host
# test.
set -u
cd "$(dirname "$0")/.."

CXX=${CXX:-g++}
# Every src/<area>/ folder is on the include path, as in the Makefile: the tests
# and the sources name headers bare ("StackModel.hpp").
INC=()
for d in src/*/; do INC+=("-I${d%/}"); done
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
fail=0

run() {
    local name=$1; shift
    if ! "$CXX" -std=c++2b -O2 -Wall "${INC[@]}" -o "$OUT/$name" "$@" 2>"$OUT/$name.build"; then
        echo "host_tests: $name FAILED TO BUILD" >&2
        cat "$OUT/$name.build" >&2
        fail=1
        return
    fi
    if ! "$OUT/$name" > "$OUT/$name.out" 2>&1; then
        echo "host_tests: $name FAILED" >&2
        cat "$OUT/$name.out" >&2
        fail=1
        return
    fi
    echo "host_tests: $name OK -- $(tail -1 "$OUT/$name.out")"
}

run drop_table_test tests/unit/drop_table_test.cpp
# shellcheck disable=SC2046
run geometry_test tests/unit/geometry_test.cpp src/overview/OverviewGeometry.cpp $(pkg-config --cflags --libs hyprutils)
run strip_scrolling_test tests/unit/strip_scrolling_test.cpp $(pkg-config --cflags --libs hyprutils)
run search_letters_test tests/unit/search_letters_test.cpp
run workspace_plan_test tests/unit/workspace_plan_test.cpp
run strip_layout_test tests/unit/strip_layout_test.cpp
run tool_search_test tests/unit/tool_search_test.cpp
run debug_paths_test tests/unit/debug_paths_test.cpp
run stack_model_test tests/unit/stack_model_test.cpp src/stacks/StackModel.cpp
run submap_borrow_test tests/unit/submap_borrow_test.cpp
run mount_sort_test tests/unit/mount_sort_test.cpp
run gvfs_mount_test tests/unit/gvfs_mount_test.cpp
run mosaic_pack_test tests/unit/mosaic_pack_test.cpp
run show_desktop_test tests/unit/show_desktop_test.cpp
run x_state_request_test tests/unit/x_state_request_test.cpp
run preview_grid_test tests/unit/preview_grid_test.cpp
run motion_spacing_test tests/unit/motion_spacing_test.cpp
# shellcheck disable=SC2046
run motion_math_test tests/unit/motion_math_test.cpp $(pkg-config --cflags --libs hyprutils)
run icon_overrides_test tests/unit/icon_overrides_test.cpp
run dock_drop_test tests/unit/dock_drop_test.cpp
run finger_swipe_test tests/unit/finger_swipe_test.cpp
run hot_corner_test tests/unit/hot_corner_test.cpp
run desktop_step_test tests/unit/desktop_step_test.cpp
run desktop_plan_test tests/unit/desktop_plan_test.cpp
run fullscreen_book_test tests/unit/fullscreen_book_test.cpp
run desktop_row_test tests/unit/desktop_row_test.cpp
run desktop_strip_test tests/unit/desktop_strip_test.cpp
# shellcheck disable=SC2046
run builtin_icons_test tests/unit/builtin_icons_test.cpp src/apps/BuiltinIcons.cpp --embed-dir=. $(pkg-config --cflags --libs librsvg-2.0)
# shellcheck disable=SC2046
run stack_reader_test tests/unit/stack_reader_test.cpp src/stacks/StackReader.cpp src/stacks/StackModel.cpp $(pkg-config --cflags --libs gio-2.0)
run search_backend_test tests/unit/search_backend_test.cpp src/search/SearchBackend.cpp $(pkg-config --cflags --libs hyprutils libsystemd glib-2.0)
# shellcheck disable=SC2046
run icon_size_test tests/unit/icon_size_test.cpp $(pkg-config --cflags --libs cairo)
# shellcheck disable=SC2046
run thumbs_test tests/unit/thumbs_test.cpp src/stacks/Thumbs.cpp -DTHUMBS_SETTLE_MS=250 -pthread $(pkg-config --cflags --libs glib-2.0)
# shellcheck disable=SC2046
run search_providers_nobus_test tests/unit/search_providers_nobus_test.cpp src/search/SearchProviders.cpp $(pkg-config --cflags --libs libsystemd)
run trash_dirs_test tests/unit/trash_dirs_test.cpp
run store_file_test tests/unit/store_file_test.cpp
# shellcheck disable=SC2046
run search_providers_deadbus_test tests/unit/search_providers_deadbus_test.cpp src/search/SearchProviders.cpp $(pkg-config --cflags --libs libsystemd)
# shellcheck disable=SC2046
run file_uri_test tests/unit/file_uri_test.cpp $(pkg-config --cflags --libs glib-2.0 libsystemd)
# shellcheck disable=SC2046
run launcher_states_test tests/unit/launcher_states_test.cpp $(pkg-config --cflags libsystemd)

# The animation clocks are measured through MotionClock.hpp (msBetween /
# secondsBetween against a `now` the site already holds) or Config's
# realElapsedMs / animElapsedMs -- never a raw std::chrono::duration read, so
# a site's clock kind is visible at the call. The files that animate; their
# raw reads went in the A2 merge, and a new one fails here.
MOTION_CLOCK_FILES=(src/motion/BorderFade.cpp src/motion/FlightBlur.cpp src/motion/FlightController.cpp src/motion/Motion.cpp
    src/overview/TileChrome.cpp src/overview/OverviewStrip.cpp src/overview/OverviewCards.cpp src/overview/Ripple.cpp
    src/layout/MosaicGestures.cpp src/layout/MosaicParts.hpp src/layout/FullscreenDesktop.cpp src/layout/ShowDesktop.cpp
    src/desktops/DesktopStrip.cpp src/dock/SpringLoad.cpp src/dock/ScratchpadExit.cpp src/dock/DockPreview.cpp
    src/dock/DockSpringLoad.cpp src/stacks/StackFan.cpp src/search/SearchSession.hpp src/search/SearchLetters.hpp
    src/search/KeyboardCat.cpp)
if raw=$(grep -n 'std::chrono::duration<' "${MOTION_CLOCK_FILES[@]}"); then
    echo "host_tests: motion_clock_reads FAILED -- a raw duration read in motion code (use MotionClock.hpp):" >&2
    echo "$raw" >&2
    fail=1
else
    echo "host_tests: motion_clock_reads OK -- ${#MOTION_CLOCK_FILES[@]} files, no raw duration read"
fi

exit $fail
