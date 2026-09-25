// See BuiltinIcons.hpp. No Hyprland headers: scripts/builtin_icons_test.cpp
// links this file on the host.
#include "BuiltinIcons.hpp"

namespace {
    // Embedded with a trailing NUL so each is a C string.
    constexpr char TRASH_EMPTY_SVG[] = {
#embed "assets/icons/trash-empty.svg"
        , 0};
    constexpr char TRASH_FULL_SVG[] = {
#embed "assets/icons/trash-full.svg"
        , 0};
    constexpr char STACK_EMPTY_SVG[] = {
#embed "assets/icons/stack-empty.svg"
        , 0};
}

namespace ScrollOverview::BuiltinIcons {
    std::string svg(std::string_view name, std::string_view accentHex) {
        std::string out = name == TRASH_EMPTY ? TRASH_EMPTY_SVG : name == TRASH_FULL ? TRASH_FULL_SVG : name == STACK_EMPTY ? STACK_EMPTY_SVG : "";
        constexpr std::string_view TOKEN = "#ACCENT";
        for (size_t at = out.find(TOKEN); at != std::string::npos; at = out.find(TOKEN, at + accentHex.size()))
            out.replace(at, TOKEN.size(), accentHex);
        return out;
    }
}
