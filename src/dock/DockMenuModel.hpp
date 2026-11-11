#pragma once

// The dock menu's pure parts (user, 2026-11-11: "add a maximize and fullscreen
// option and always open in fullscreen in the context menu but design it such
// that it's intuitive and space saving"): the per-app id lists behind "Always
// Open in", the decision whether a window that just opened is one of them, and
// the boxes of a row cut into segments. No compositor types, so the host tests
// (tests/unit/dock_menu_model_test.cpp) build it alone.

#include <algorithm>
#include <cmath>
#include <cstdint>
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

    // A window row's buttons, laid left to right so the last ends at `right`,
    // each `btn` square-ish px wide, touching (the row's own hover chip gives
    // them their spacing). Always `n` slots, so the buttons line up down the
    // list; a slot left empty is still a slot.
    inline std::vector<SSpan> rowButtons(double right, double btn, size_t n) {
        std::vector<SSpan> out;
        if (!std::isfinite(right) || !std::isfinite(btn))
            return std::vector<SSpan>(n);
        for (size_t b = 0; b < n; ++b)
            out.push_back(guarded(std::round(right - btn * static_cast<double>(n - b)), std::round(btn)));
        return out;
    }

    // THE WINDOW'S OWN MENU (user, 2026-11-11: "a context menu for window
    // previews too ... functions limited to that window including closing
    // it"), opened over its preview. It hangs ABOVE the preview pane, so the
    // bottom row is the one nearest the pointer: Close, then the window's size
    // and Minimize, then its moves, and its title at the top.
    enum class EWinRow : uint8_t {
        Header, // the title, with where it is
        Separator,
        MoveHere,
        MoveEmptyWorkspace,
        MoveScratchpad,
        Size, // Maximize | Fullscreen
        Minimize,
        Show,
        Close,
    };
    struct SWindowFacts {
        bool elsewhere  = false; // on another desktop than the one the monitor shows (and not minimized)
        bool minimized  = false;
        bool scratchpad = false;
    };
    inline std::vector<EWinRow> windowMenuRows(const SWindowFacts& w) {
        std::vector<EWinRow> r{EWinRow::Header, EWinRow::Separator};
        if (w.elsewhere && !w.minimized)
            r.push_back(EWinRow::MoveHere);
        r.push_back(EWinRow::MoveEmptyWorkspace);
        if (!w.scratchpad)
            r.push_back(EWinRow::MoveScratchpad);
        r.push_back(EWinRow::Separator);
        // The app menu's split row offers itself only for a window out on a
        // desktop; the same rule here.
        if (!w.minimized && !w.scratchpad)
            r.push_back(EWinRow::Size);
        r.push_back(w.minimized ? EWinRow::Show : EWinRow::Minimize);
        r.push_back(EWinRow::Separator);
        r.push_back(EWinRow::Close);
        return r;
    }

    // WHERE A WINDOW ROW'S PREVIEW GOES (user, 2026-11-11: "show me the window
    // preview next to the row (normally to the right of the row)"): `w` x `h`
    // beside the menu, `gap` off it, centred on the row's centre line; to the
    // left when the right has no room, and on whichever side has more when
    // neither does, then clamped to the monitor `edge` px in. Nothing (w == 0)
    // for a box that is not finite or under 2 px.
    struct SRect {
        double x = 0, y = 0, w = 0, h = 0;
    };
    struct SPeekPlace {
        SRect box;
        bool  left = false;
    };
    inline SPeekPlace peekPlacement(const SRect& menu, double rowCenterY, double w, double h, double gap, double edge, double monW, double monH) {
        for (const double V : {menu.x, menu.y, menu.w, menu.h, rowCenterY, w, h, gap, edge, monW, monH})
            if (!std::isfinite(V))
                return {};
        if (w < 2.0 || h < 2.0)
            return {};
        const double RIGHTX = menu.x + menu.w + gap, LEFTX = menu.x - gap - w;
        const double ROOMR = monW - edge - (RIGHTX + w), ROOML = LEFTX - edge;
        SPeekPlace   out;
        out.left  = ROOMR < 0.0 && ROOML > ROOMR;
        double x  = out.left ? LEFTX : RIGHTX;
        x         = std::clamp(x, edge, std::max(edge, monW - edge - w));
        double y  = std::clamp(rowCenterY - h / 2.0, edge, std::max(edge, monH - edge - h));
        out.box   = {std::round(x), std::round(y), std::round(w), std::round(h)};
        return out;
    }
}
