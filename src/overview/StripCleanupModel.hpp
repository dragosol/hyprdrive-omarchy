#pragma once

// THE STRIP'S CLEAN UP BUTTON, its pure parts (user, 2026-11-11: "if I hold
// super over the workspace strip it puts a button under the strip near my
// cursor to clean up empty desktops"). When it shows, where it goes, how far
// the windows under it are bumped, and what a click is worth -- no compositor
// types, so tests/unit/strip_cleanup_test.cpp builds it alone. The drawing and
// the click are OverviewStripCleanup.cpp; the clean-up itself is the display
// change's compaction (DesktopPlan::compactMoves via DesktopOrder::compact),
// one mechanism for both.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace Hyprdrive::StripCleanup {

    // A desktop of the monitor's row, as its strip cell has it.
    struct SDesk {
        long long slot      = 0;     // its place in the row, 1-based; 0 = not in the row (a visitor)
        bool      populated = false; // mapped windows on it
        bool      shown     = false; // the monitor shows it
    };

    // The empty desktops a click removes: empty, not the one shown (that is
    // where the user is: it stays, after the others), and BEFORE the row's last
    // desktop with windows -- a hole the populated ones close. Empty ones past
    // the last populated one are not holes: the trailing spare the overview
    // always offers is one of them, and the compaction would not move them
    // either, so a button for them would do nothing. Visitors never count.
    inline size_t removable(const std::vector<SDesk>& desks) {
        long long last = 0;
        for (const auto& D : desks)
            if (D.slot > 0 && D.populated)
                last = std::max(last, D.slot);
        size_t n = 0;
        for (const auto& D : desks)
            if (D.slot > 0 && D.slot < last && !D.populated && !D.shown)
                ++n;
        return n;
    }

    // Whether the button wants to be up this frame.
    struct SWhen {
        bool   enabled  = true;  // overview:strip_cleanup_button
        bool   super    = false; // Super held: the button lives only while it is ("hold super")
        bool   onStrip  = false; // the pointer on the strip's plate (a cell or the band between)
        bool   onButton = false; // ...or on the button itself
        bool   grace    = false; // ...or it left one of them a moment ago (on its way between)
        bool   busy     = false; // a strip or window drag, a desktop switch, the overview closing
        size_t removable = 0;
    };
    inline bool wanted(const SWhen& w) {
        return w.enabled && w.super && !w.busy && w.removable > 0 && (w.onStrip || w.onButton || w.grace);
    }

    // The pointer leaving the strip for the button crosses the gap between
    // them: the dock pane's own grace (PANEL_GRACE_MS) bridges it.
    inline constexpr double GRACE_MS = 180.0;
    inline bool inGrace(double msSinceOn) {
        return std::isfinite(msSinceOn) && msSinceOn >= 0.0 && msSinceOn < GRACE_MS;
    }

    // The button's left edge: centred on the pointer, kept inside [lo, hi]
    // (the strip's plate, itself kept on the monitor). Narrower room than the
    // button: centred on the room.
    inline double placeX(double pointerX, double width, double lo, double hi) {
        if (!std::isfinite(pointerX) || !std::isfinite(width) || !std::isfinite(lo) || !std::isfinite(hi))
            return std::isfinite(lo) ? lo : 0.0;
        if (hi - lo <= width)
            return lo + (hi - lo - width) / 2.0;
        return std::clamp(pointerX - width / 2.0, lo, hi - width);
    }

    // Toward `want` like the strip's gliding cells (KGLIDE): exponential, by
    // the animation clock's step `dt` (s), time constant `tau`. A step of 0
    // holds; a snapping clock's step (>= tau * 8) lands. Not finite: lands.
    inline double follow(double now, double want, double dt, double tau = 0.085) {
        if (!std::isfinite(now) || !std::isfinite(want) || !std::isfinite(dt) || tau <= 0.0)
            return want;
        if (dt <= 0.0)
            return now;
        const double K = 1.0 - std::exp(-dt / tau);
        const double X = now + (want - now) * K;
        return std::abs(want - X) < 0.25 ? want : X;
    }

    struct SBox {
        double x = 0, y = 0, w = 0, h = 0;
        double right() const {
            return x + w;
        }
        double bottom() const {
            return y + h;
        }
    };
    // Finite and at least 2 px each way: anything else is drawn and hit-tested as nothing.
    inline bool drawable(const SBox& b) {
        return std::isfinite(b.x) && std::isfinite(b.y) && std::isfinite(b.w) && std::isfinite(b.h) && b.w >= 2.0 && b.h >= 2.0;
    }

    // THE LABEL'S RULE, for the button: how far (px) the things under it move
    // down so the button keeps `gap` to them, as the label keeps it to the
    // windows. `under` are their boxes AT REST (without any bump already
    // applied); only those that start below `floorY` (the strip's plate foot
    // -- the windows, not the strip) and share some of the button's width
    // (gap included) count. Nothing in the way: 0, nothing moves.
    inline double bumpFor(const SBox& button, double gap, const std::vector<SBox>& under, double floorY) {
        if (!drawable(button) || !std::isfinite(gap) || !std::isfinite(floorY))
            return 0.0;
        double need = 0.0;
        for (const auto& U : under) {
            if (!drawable(U) || U.y < floorY)
                continue;
            if (U.right() <= button.x - gap || U.x >= button.right() + gap)
                continue;
            need = std::max(need, button.bottom() + gap - U.y);
        }
        return std::max(0.0, need);
    }
}
