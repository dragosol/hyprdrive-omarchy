#pragma once

// HYPRDRIVE'S OWN ARTWORK (dock stacks C, docs/superpowers/specs/2026-09-24-dock-stacks-c-design.md).
// Original SVGs under assets/icons, compiled into the plugin with #embed so the
// .so stays one file. Names are what OverviewRender::appIcon is asked for; a
// "hyprdrive-" name never goes to the icon theme.

#include <string>
#include <string_view>

namespace ScrollOverview::BuiltinIcons {
    inline constexpr std::string_view PREFIX      = "hyprdrive-";
    inline constexpr std::string_view TRASH_EMPTY = "hyprdrive-trash-empty";
    inline constexpr std::string_view TRASH_FULL  = "hyprdrive-trash-full";
    inline constexpr std::string_view STACK_EMPTY = "hyprdrive-stack-empty";

    // The SVG source for `name`, with #ACCENT replaced by `accentHex`
    // ("#rrggbb"). Empty for an unknown name.
    std::string svg(std::string_view name, std::string_view accentHex);
}
