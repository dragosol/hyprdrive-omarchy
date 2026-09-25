#pragma once

// HYPRDRIVE'S OWN ARTWORK (dock stacks C, docs/superpowers/specs/2026-09-24-dock-stacks-c-design.md).
// Original SVGs under assets/icons, compiled into the plugin with #embed so the
// .so stays one file. Names are what OverviewRender::appIcon is asked for; a
// "hyprdrive-" name never goes to the icon theme.

#include <string>
#include <string_view>

namespace ScrollOverview::BuiltinIcons {
    inline constexpr std::string_view PREFIX      = "hyprdrive-";
    // The Trash is one template in nine variants (2026-09-24):
    // "hyprdrive-trash-<empty|full>-<light|dark|clear>-<white|color>".
    inline constexpr std::string_view TRASH_PREFIX = "hyprdrive-trash-";
    inline constexpr std::string_view STACK_EMPTY = "hyprdrive-stack-empty";

    // The SVG source for `name`, with #ACCENT replaced by `accentHex`
    // ("#rrggbb"). Empty for an unknown name.
    std::string svg(std::string_view name, std::string_view accentHex);

    // The Trash's icon name for its state and the user's choices. `style`:
    // light | dark | clear (anything else = light); `papers`: white | color.
    std::string trashName(bool empty, std::string_view style, std::string_view papers);
}
