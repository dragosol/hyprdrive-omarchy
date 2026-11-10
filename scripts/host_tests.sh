#!/usr/bin/env bash
# The tests that need NO compositor: pure functions, compiled and run on the
# spot. `make check` runs this, which is the whole point -- a host test nobody
# runs is a comment with a build cost.
#
# Three of them today:
#   drop_table_test   ScrollOverview::Drop::resolve(), all 65536 fact
#                     combinations against the pre-split branch ladder.
#   geometry_test     ScrollOverview::Geometry -- the overview's transform,
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
#
# Anything that needs a running Hyprland is a GATE (scripts/gates/), not a host
# test, and lives in scripts/suite.sh instead.
set -u
cd "$(dirname "$0")/.."

CXX=${CXX:-g++}
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
fail=0

run() {
    local name=$1; shift
    if ! "$CXX" -std=c++2b -O2 -Wall -o "$OUT/$name" "$@" 2>"$OUT/$name.build"; then
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

run drop_table_test scripts/drop_table_test.cpp
# shellcheck disable=SC2046
run geometry_test scripts/geometry_test.cpp src/OverviewGeometry.cpp $(pkg-config --cflags --libs hyprutils)
run strip_scrolling_test scripts/strip_scrolling_test.cpp $(pkg-config --cflags --libs hyprutils)
run search_letters_test scripts/search_letters_test.cpp
run workspace_plan_test scripts/workspace_plan_test.cpp
run strip_layout_test scripts/strip_layout_test.cpp
run tool_search_test scripts/tool_search_test.cpp
run debug_paths_test scripts/debug_paths_test.cpp
run stack_model_test scripts/stack_model_test.cpp src/StackModel.cpp
run submap_borrow_test scripts/submap_borrow_test.cpp
run mount_sort_test scripts/mount_sort_test.cpp
run gvfs_mount_test scripts/gvfs_mount_test.cpp
run mosaic_pack_test scripts/mosaic_pack_test.cpp
run show_desktop_test scripts/show_desktop_test.cpp
run preview_grid_test scripts/preview_grid_test.cpp
run motion_spacing_test scripts/motion_spacing_test.cpp
run icon_overrides_test scripts/icon_overrides_test.cpp
run dock_drop_test scripts/dock_drop_test.cpp
run finger_swipe_test scripts/finger_swipe_test.cpp
run hot_corner_test scripts/hot_corner_test.cpp
run desktop_step_test scripts/desktop_step_test.cpp
run desktop_row_test scripts/desktop_row_test.cpp
run desktop_strip_test scripts/desktop_strip_test.cpp
# shellcheck disable=SC2046
run builtin_icons_test scripts/builtin_icons_test.cpp src/BuiltinIcons.cpp --embed-dir=. $(pkg-config --cflags --libs librsvg-2.0)
# shellcheck disable=SC2046
run stack_reader_test scripts/stack_reader_test.cpp src/StackReader.cpp src/StackModel.cpp $(pkg-config --cflags --libs gio-2.0)
run omarchy_search_test scripts/omarchy_search_test.cpp src/OmarchySearch.cpp $(pkg-config --cflags --libs hyprutils libsystemd glib-2.0)
# shellcheck disable=SC2046
run icon_size_test scripts/icon_size_test.cpp $(pkg-config --cflags --libs cairo)
# shellcheck disable=SC2046
run thumbs_test scripts/thumbs_test.cpp src/Thumbs.cpp -DTHUMBS_SETTLE_MS=250 -pthread $(pkg-config --cflags --libs glib-2.0)
# shellcheck disable=SC2046
run search_providers_nobus_test scripts/search_providers_nobus_test.cpp src/SearchProviders.cpp $(pkg-config --cflags --libs libsystemd)
run trash_dirs_test scripts/trash_dirs_test.cpp
run store_file_test scripts/store_file_test.cpp
# shellcheck disable=SC2046
run search_providers_deadbus_test scripts/search_providers_deadbus_test.cpp src/SearchProviders.cpp $(pkg-config --cflags --libs libsystemd)
# shellcheck disable=SC2046
run file_uri_test scripts/file_uri_test.cpp $(pkg-config --cflags --libs glib-2.0 libsystemd)
# shellcheck disable=SC2046
run launcher_states_test scripts/launcher_states_test.cpp $(pkg-config --cflags libsystemd)

exit $fail
