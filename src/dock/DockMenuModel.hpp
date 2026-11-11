#pragma once

// The dock menu's pure parts (user, 2026-11-11: "add a maximize and fullscreen
// option and always open in fullscreen in the context menu but design it such
// that it's intuitive and space saving"): the per-app id lists behind "Always
// Open in", the decision whether a window that just opened is one of them, and
// the boxes of a row cut into segments. No compositor types, so the host tests
// (tests/unit/dock_menu_model_test.cpp) build it alone.

#include <algorithm>
#include <cmath>
#include <istream>
#include <string>
#include <vector>

namespace Hyprdrive::Dock::MenuModel {

    // An id list file: one desktop id per line, trailing blanks and CRs
    // dropped, empty lines skipped. The format the empty-desktop list has had
    // since 2026-10-29, so a file written by an older build reads the same.
    inline std::vector<std::string> parseIdLines(std::istream& in) {
        std::vector<std::string> ids;
        std::string              line;
        while (std::getline(in, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
                line.pop_back();
            if (!line.empty())
                ids.emplace_back(line);
        }
        return ids;
    }

    // `ids` with `id` taken out if it was there, else added at the end. An
    // empty id changes nothing (an app no desktop entry claims has no id).
    inline std::vector<std::string> toggledId(std::vector<std::string> ids, const std::string& id) {
        if (id.empty())
            return ids;
        if (const auto IT = std::ranges::find(ids, id); IT != ids.end())
            ids.erase(IT);
        else
            ids.emplace_back(id);
        return ids;
    }

    inline std::string serializeIds(const std::vector<std::string>& ids) {
        std::string out;
        for (const auto& I : ids)
            out += I + "\n";
        return out;
    }

    // A window that just opened, as the "Always Open in" chips see it.
    struct SOpenedWindow {
        bool listed   = false; // its app's id is on the list
        bool floating = false; // a dialog, a splash, picture-in-picture
        bool child    = false; // it has a parent window
        bool modal    = false;
        bool special  = false; // on a special workspace (a scratchpad)
        bool applied  = false; // already sent fullscreen once: never again
    };
    // Only an app's MAIN windows, the same test the empty-desktop list uses,
    // and once per window: a window the user took out of fullscreen stays out.
    inline bool shouldOpenFullscreen(const SOpenedWindow& w) {
        return w.listed && !w.floating && !w.child && !w.modal && !w.special && !w.applied;
    }

    // A segment of a row, in the row's pixels. Empty (w == 0) when it would be
    // under 2 px or not finite: the caller draws nothing and hit-tests nothing.
    struct SSpan {
        double x = 0, w = 0;
    };
    inline SSpan guarded(double x, double w) {
        if (!std::isfinite(x) || !std::isfinite(w) || w < 2.0)
            return {};
        return {std::round(x), std::round(w)};
    }

    // [x0, x0 + width) cut into `n` equal segments with `gap` between them --
    // the Maximize | Fullscreen row. Each edge is rounded on its own, so the
    // segments tile without a 1 px seam drifting between them.
    inline std::vector<SSpan> splitSpan(double x0, double width, int n, double gap) {
        std::vector<SSpan> out;
        if (n <= 0 || !std::isfinite(x0) || !std::isfinite(width) || !std::isfinite(gap))
            return out;
        const double EACH = (width - gap * (n - 1)) / n;
        for (int i = 0; i < n; ++i) {
            const double L = std::round(x0 + i * (EACH + gap));
            const double R = std::round(x0 + i * (EACH + gap) + EACH);
            out.push_back(guarded(L, R - L));
        }
        return out;
    }

    // Chips of the given widths laid right to left, the last ending at
    // `right`, `gap` between -- the "Always Open in" row's toggles.
    inline std::vector<SSpan> chipsEndingAt(double right, const std::vector<double>& widths, double gap) {
        std::vector<SSpan> out(widths.size());
        double             x = right;
        for (size_t k = widths.size(); k-- > 0;) {
            const double W = std::round(widths[k]);
            out[k]         = guarded(x - W, W);
            x -= W + gap;
        }
        return out;
    }
}
