#pragma once
// TEXT SIZE FOLLOWS THE SYSTEM (#8, 2026-11-12/14: "Hyprdrive ignores Omarchy's
// [font] base-size ... the whole shell scales down; Hyprdrive's chrome doesn't").
//
// Omarchy's shell sizes every font from `[font] base-size` in
// ~/.config/omarchy/shell.toml (12 unless set; `omarchy display text size`
// writes it). Hyprdrive's sizes were drawn against that 12, so when
// general:text_size is "system" its text is scaled by base-size / 12 (no
// file, no [font] table or no base-size: 1). "default", as shipped, is
// Hyprdrive's own sizes; "custom" is the user's general:text_scale.
//
// Pure, for tests/unit/text_scale_test.cpp. Theme.cpp reads the file.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <string>

namespace Hyprdrive::TextScale {
    constexpr float OMARCHY_BASE = 12.F;
    constexpr float MIN_SCALE    = 0.5F;
    // GROWS TO 14, NO FURTHER (measured in the rig, 2026-11-14): the search
    // results card's rows and its preview pane are a fixed height, and past
    // 14/12 a group's heading meets its first row and the preview loses lines.
    // Until those rows grow with the text, a larger system size stops here.
    constexpr float MAX_SCALE    = 14.F / 12.F;

    // shell.toml's text -> the scale. Only `base-size = <number>` inside the
    // [font] table counts; a comment, another table's key or a value that is
    // not a positive number leaves 1.
    inline float fromShellToml(const std::string& toml) {
        std::istringstream in{toml};
        std::string        line;
        bool               inFont = false;
        while (std::getline(in, line)) {
            const auto B = line.find_first_not_of(" \t");
            if (B == std::string::npos || line[B] == '#')
                continue;
            if (line[B] == '[') {
                const auto E = line.find(']', B);
                auto       name = E == std::string::npos ? std::string{} : line.substr(B + 1, E - B - 1);
                name.erase(0, name.find_first_not_of(" \t"));
                name.erase(name.find_last_not_of(" \t") + 1);
                inFont = name == "font";
                continue;
            }
            if (!inFont)
                continue;
            const auto EQ = line.find('=', B);
            if (EQ == std::string::npos)
                continue;
            auto key = line.substr(B, EQ - B);
            key.erase(key.find_last_not_of(" \t") + 1);
            if (key != "base-size" && key != "\"base-size\"")
                continue;
            const char* const V   = line.c_str() + EQ + 1;
            char*             end = nullptr;
            const float       N   = std::strtof(V, &end);
            if (end == V || !std::isfinite(N) || N <= 0.F)
                return 1.F;
            return std::clamp(N / OMARCHY_BASE, MIN_SCALE, MAX_SCALE);
        }
        return 1.F;
    }

    // A font size in px under `scale`; never below 1.
    inline int scaled(int fontSize, float scale) {
        return std::max(1, static_cast<int>(std::lround(static_cast<float>(fontSize) * scale)));
    }
}
