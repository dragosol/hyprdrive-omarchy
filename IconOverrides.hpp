#pragma once

// PER-APP DOCK ICONS (2026-10-13, after a user's request: "change dock icons
// from the settings"). The user picks a replacement icon for an app in the
// settings panel's Icons section; the panel writes it here and the dock draws
// it in place of the app's own Icon=.
//
// The store is ~/.local/state/hyprdrive/icons, one override per line:
//
//     <desktop id> TAB <icon>
//
// <icon> is an absolute file path (SVG, PNG, JPEG or WebP -- anything the dock
// already loads by path) or an icon-theme name. The panel is its only writer
// and names every file it saves after the file's content, so a changed icon is
// always a new path: the dock's texture cache is keyed on the name and would
// keep an old picture under an unchanged one.
//
// parse() is pure (scripts/icon_overrides_test.cpp); the rest reads the file
// the way the favourites store does -- polled, re-read when its mtime moves.

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace ScrollOverview::IconOverrides {
    // Lines -> {desktop id -> icon}. Blank lines, `#` comments, a line with no
    // tab, an empty id or an empty icon are skipped; trailing whitespace and CR
    // are trimmed; the LAST line for an id wins.
    inline std::unordered_map<std::string, std::string> parse(std::string_view text) {
        std::unordered_map<std::string, std::string> out;
        size_t                                       pos = 0;
        while (pos <= text.size()) {
            const size_t     END  = text.find('\n', pos);
            std::string_view line = text.substr(pos, END == std::string_view::npos ? std::string_view::npos : END - pos);
            pos                   = END == std::string_view::npos ? text.size() + 1 : END + 1;
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
                line.remove_suffix(1);
            if (line.empty() || line.front() == '#')
                continue;
            const size_t TAB = line.find('\t');
            if (TAB == std::string_view::npos || TAB == 0 || TAB + 1 >= line.size())
                continue;
            out[std::string(line.substr(0, TAB))] = std::string(line.substr(TAB + 1));
        }
        return out;
    }

    // The override for a desktop id, or nullptr. Cheap to call per frame: the
    // file is stat'ed at most twice a second.
    const std::string* forId(const std::string& desktopId);
    // Moves whenever the set of overrides changes; part of the dock's item key.
    uint64_t generation();
}
