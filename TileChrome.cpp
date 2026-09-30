#include "TileChrome.hpp"
#include "Motion.hpp"

#include "Config.hpp"
#include "globals.hpp"

#include "AppSearch.hpp"
#include "Dock.hpp"
#include "OverviewRender.hpp"
#include "WindowPill.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

#include <hyprland/src/helpers/time/Time.hpp>

#define private public
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/RectPassElement.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>
#undef private

namespace ScrollOverview::TileChrome {

namespace {

// X7 asked for a "playful bouncy" blow-up borrowed from the deprecated
// overview's Easing.OutBack. **That clause is SUPERSEDED** by the standing
// Apple-motion rule and by the user's direct report on 2026-08-03 that the
// hover "is still bouncy and non-apple like". Zero overshoot now, both ways.
// Do NOT restore easeOutBack here as an X7 regression fix.
//
// X73 (user, 2026-08-06): "in gnome the mouseover animation is smoother and
// different curves, can you adopt the same animation".
//
// GNOME's hover is a CSS transition on the tile: ONE timing function both ways
// and ONE duration both ways, on an ease-out. That is deliberately NOT this
// rice's house rule -- entrances decelerate, exits accelerate and run ~25%
// shorter -- and the difference is exactly what the user is hearing as "less
// smooth": an exit that is quicker than its entrance reads as a snap-back on an
// interaction the pointer is still inside.
//
// So the HOVER, and only the hover, moves to GNOME's shape. The press keeps the
// house curves (it is a discrete acknowledgement, not a hover state) and
// houseEase is untouched for everything else in the rice.
constexpr float HOVER_GROW    = 0.05F;
constexpr float PRESS_SHRINK  = 0.04F;
constexpr float HOVER_IN_MS   = 200.F;
constexpr float HOVER_OUT_MS  = 150.F; // exits run 20-30% faster, house rule -- the one deviation the 2026-08-07 GNOME-numbers audit found

// User, 2026-08-06: a tile the cursor has left stays blown up for a beat, "that
// way it gives me time to decide whether to press the x button or not". Only
// the tile the cursor left into EMPTY SPACE lingers -- moving to another tile
// shrinks the old one at once, so two tiles are never big together.
constexpr float HOVER_LINGER_MS = 2000.F;
constexpr float PRESS_MS      = 90.F;

// X8 / close button proportions, as fractions of the tile's SHORT side so they
// stay put when the grid rescales, clamped so they stay legible in a 12-window
// grid and stop growing in a 1-window one.
constexpr float ICON_FRACTION   = 0.20F;
constexpr float ICON_MIN        = 28.F;
constexpr float ICON_MAX        = 72.F;
// X55. The host's `.window-close`, from gnome-shell-theme.gresource:
//     width/height 32, padding 3, StIcon icon-size 24, border-radius 999
//     background rgba(63,63,70,0.98), colour #fafafb, transition 100ms
// so the glyph is 24/32 = 0.75 of the button, and 32 is the CAP here rather
// than a fixed size -- the host's previews are always large, ours can be a
// twelfth of a screen, and a 32px button on a small tile would swallow it.
constexpr float CLOSE_FRACTION  = 0.15F;
constexpr float CLOSE_MIN       = 22.F;
constexpr float CLOSE_MAX       = 32.F;
// X55: ON the corner, not inside it. The host centres the button on the
// preview's top-right corner so it overhangs on both axes; this was 0.62
// button-diameters INWARD, which is the "inside corner" the user rejected.
constexpr float CLOSE_INSET     = 0.F;
constexpr float CLOSE_GLYPH     = 0.75F;

// The tile has to be most of the way hovered before the button appears, so a
// cursor crossing a tile on its way somewhere else does not strobe buttons.
constexpr float CLOSE_VISIBLE_AT = 0.55F;

// X57. The caption clears X8's app icon, which straddles the tile's bottom
// edge, and CAPTION_H is the plate's own height budget -- labelPill anchors by
// its BOTTOM, so this is what pushes it clear rather than over the icon.
// Slop around the close button's hit area. The button is 22-32px and sits half
// off the tile, so a few px either side is the difference between a target you
// aim at and one you stab at.
constexpr float CLOSE_SLOP  = 8.F;

constexpr float CAPTION_GAP = 6.F;
constexpr float CAPTION_H   = 40.F;

constexpr const char* CLOSE_ICON = "window-close-symbolic";

struct SEase {
    float           from  = 0.F;
    float           to    = 0.F;
    float           ms    = 1.F;
    Time::steady_tp start = {};

    float value() const {
        const float ELAPSED = std::chrono::duration<float, std::milli>(Time::steadyNow() - start).count();
        // A start in the FUTURE is a delayed ease: hold at `from` until it is due.
        if (ELAPSED <= 0.F)
            return from;
        return ELAPSED >= ms ? to : from + (to - from) * (ELAPSED / ms);
    }

    bool running() const { return std::chrono::duration<float, std::milli>(Time::steadyNow() - start).count() < ms; }

    void retarget(float target, float durationMs, float delayMs = 0.F) {
        if (std::abs(to - target) < 0.001F)
            return;

        from  = value();
        to    = target;
        ms    = std::max(1.F, durationMs);
        start = Time::steadyNow() + std::chrono::milliseconds(sc<int64_t>(std::max(0.F, delayMs)));
    }

    // Drop a delay that has not started yet, keeping the same target. Used when
    // a new tile takes the hover and the old one must not sit there waiting.
    void hurry() {
        if (Time::steadyNow() < start) {
            from  = value();
            start = Time::steadyNow();
        }
    }
};

struct SState {
    SEase hover;
    SEase press;
    // The output this tile was on when its ease was armed. Only used to decide
    // which overview owes the frames; the ease itself is output-agnostic.
    MONITORID monitor = MONITOR_INVALID;
};

std::unordered_map<Desktop::View::CWindow*, SState> g_states;
PHLWINDOWREF                                        g_hovered;
PHLWINDOWREF                                        g_pressed;
// The tile serving out its post-hover linger. Not the same thing as g_hovered:
// the cursor is no longer on it.
PHLWINDOWREF                                        g_lingering;

// Captions are queued during TileChrome::render and flushed after the dock, so
// a title hanging off a window at the bottom of the active card is not covered.
struct SCaption {
    PHLWINDOWREF window;
    CBox         tileBox;
    float        fade  = 1.F;
    float        scale = 1.F;
};

std::vector<SCaption> g_captions;

SState* stateFor(const PHLWINDOW& window, bool create) {
    if (!window)
        return nullptr;

    const auto stamp = [&window](SState& state) -> SState* {
        // Re-stamped on every lookup rather than only at creation: a window
        // dragged to the other output has to start owing ITS frames.
        if (const auto MON = window->m_monitor.lock())
            state.monitor = MON->m_id;
        return &state;
    };

    // ponytail: only ever as many entries as tiles the cursor has touched this
    // session, and forget() empties it when the overview closes. Reaping dead
    // windows would need a hook for a map that never gets big.
    if (const auto IT = g_states.find(window.get()); IT != g_states.end())
        return stamp(IT->second);

    return create ? stamp(g_states[window.get()]) : nullptr;
}

// Growing is an entrance and shrinking back is an exit, so the direction of the
// retarget picks the curve -- the same two the compositor and the shell use.
// Used by the PRESS. The hover has its own curve; see easedHover.
float eased(const SEase& ease) {
    const float PROGRESS = std::clamp(std::chrono::duration<float, std::milli>(Time::steadyNow() - ease.start).count() / ease.ms, 0.F, 1.F);
    const float CURVE    = houseEase(ease.to > ease.from, PROGRESS);

    return ease.from + (ease.to - ease.from) * CURVE;
}

// X73. Clutter's EASE_OUT_QUAD, which is what GNOME Shell puts on its overview
// and dash hover: 1 - (1-t)^2. Symmetric -- the same curve and the same duration
// in both directions -- because that is what a CSS transition does, and the
// symmetry is most of why GNOME's hover feels smoother than an accelerated exit.
float easedHover(const SEase& ease) {
    // The animation clock: the panel's speed stretches the hover too.
    const float T   = std::clamp(ScrollOverview::Config::animElapsedMs(ease.start) / std::max(1.F, sc<float>(ease.ms)), 0.F, 1.F);
    const float INV = 1.F - T;
    const float E   = Motion::housePreset() ? 1.F - INV * INV : Motion::ease(Motion::ERole::DECEL, T);

    return ease.from + (ease.to - ease.from) * E;
}

float shortSide(const CBox& box) {
    return sc<float>(std::min(box.w, box.h));
}

void drawCaption(PHLMONITOR monitor, const PHLWINDOW& window, const CBox& tileBox, float fade, float scale) {
    if (!monitor || !window || tileBox.w < 1 || tileBox.h < 1)
        return;

    const auto* const HSTATE = stateFor(window, false);
    if (!HSTATE)
        return;

    const float A = std::clamp(easedHover(HSTATE->hover), 0.F, 1.F) * fade;
    if (A <= 0.01F)
        return;

    // RESTING side, not the growing app-icon: the caption is a hover surface
    // and is only ever seen at FADE 1, and pinning it here stops the pill
    // sliding up the screen if a hover lands mid-open.
    const float ICONSIDE = std::clamp(shortSide(tileBox) * ICON_FRACTION, ICON_MIN * scale, ICON_MAX * scale);
    const float BOTTOM   = sc<float>(tileBox.y + tileBox.h) + ICONSIDE / 2.F + CAPTION_GAP * scale;
    float       plateBottom = BOTTOM + CAPTION_H * scale;
    // Keep the title readable without covering dock icons. The plate is still
    // drawn after the dock (z-order); this only moves it up off the pill.
    const auto DOCK = Dock::layout(monitor).pill;
    if (!DOCK.empty() && plateBottom > sc<float>(DOCK.y))
        plateBottom = sc<float>(DOCK.y) - CAPTION_GAP * scale;
    OverviewRender::labelPill(monitor, window->m_title, sc<float>(tileBox.x + tileBox.w / 2.0), plateBottom, A, scale);
}

}

void setHovered(const PHLWINDOW& window, bool lingerOnLeave) {
    if (g_hovered.lock() == window) {
        // Already not on a tile, but a linger may still be counting down.
        // The dock asks for this so a title sitting over the pill drops now.
        if (!window && !lingerOnLeave) {
            if (const auto LINGERING = g_lingering.lock()) {
                if (auto* const LINGER = stateFor(LINGERING, false))
                    LINGER->hover.hurry();
            }
            g_lingering.reset();
        }
        return;
    }

    // Arming a new tile ends any linger immediately, so only one tile is ever
    // grown. Re-arming the LINGERING tile is not a cancel -- that is the cursor
    // coming back, and the retarget below carries it.
    if (window) {
        if (const auto LINGERING = g_lingering.lock(); LINGERING && LINGERING != window) {
            if (auto* const LINGER = stateFor(LINGERING, false))
                LINGER->hover.hurry();
        }
        g_lingering.reset();
    }

    if (const auto PREVWINDOW = g_hovered.lock()) {
        if (auto* const PREV = stateFor(PREVWINDOW, false)) {
            // Leaving for empty space holds the blow-up so the close button
            // stays reachable; leaving for another tile, or for the dock, does
            // not.
            const bool LINGER = !window && lingerOnLeave;
            PREV->hover.retarget(0.F, HOVER_OUT_MS, LINGER ? HOVER_LINGER_MS : 0.F);
            if (LINGER)
                g_lingering = PREVWINDOW;
            else
                PREV->hover.hurry();
        }
    }

    g_hovered = window;

    if (auto* const STATE = stateFor(window, true))
        STATE->hover.retarget(1.F, HOVER_IN_MS);
}

void setPressed(const PHLWINDOW& window, bool down) {
    if (!down) {
        if (auto* const STATE = stateFor(g_pressed.lock(), false))
            STATE->press.retarget(0.F, PRESS_MS);

        g_pressed.reset();
        return;
    }

    if (!window)
        return;

    g_pressed = window;
    if (auto* const STATE = stateFor(window, true))
        STATE->press.retarget(1.F, PRESS_MS);
}

void forget() {
    g_states.clear();
    g_hovered.reset();
    g_pressed.reset();
    g_lingering.reset();
    g_captions.clear();
}

PHLWINDOW hovered() {
    // A LINGERING tile answers as hovered. It is still grown and still carrying
    // its close button, so everything that asks "which tile is the cursor
    // working with" -- the button's hit test, the click handler, the selection
    // ring -- has to keep getting the same answer. Drawing a button that is not
    // clickable would be the exact opposite of what the linger is for.
    if (const auto LIVE = g_hovered.lock())
        return LIVE;

    // ...but only while the linger is actually still running. g_lingering is
    // only ever reassigned when ANOTHER tile takes the hover, so without this
    // the last tile the cursor touched answered as hovered forever: its ring and
    // its close button survived the shrink and sat there until the overview
    // closed. Measured 2026-08-06 -- 3.2s after the cursor left, the ring was
    // still drawn, at the UNGROWN box size, which is what gave it away.
    if (const auto LINGERING = g_lingering.lock()) {
        if (const auto* const STATE = stateFor(LINGERING, false); STATE && easedHover(STATE->hover) > 0.001F)
            return LINGERING;

        g_lingering.reset();
    }

    return {};
}

bool animating() {
    return std::ranges::any_of(g_states, [](const auto& entry) { return entry.second.hover.running() || entry.second.press.running(); });
}

bool animatingOn(PHLMONITOR monitor) {
    if (!monitor)
        return false;

    const auto ID = monitor->m_id;
    return std::ranges::any_of(g_states, [ID](const auto& entry) {
        return entry.second.monitor == ID && (entry.second.hover.running() || entry.second.press.running());
    });
}

CBox scaledBox(const PHLWINDOW& window, const CBox& base) {
    const auto* const STATE = stateFor(window, false);
    if (!STATE || base.w < 1 || base.h < 1)
        return base;

    const float FACTOR = (1.F + HOVER_GROW * easedHover(STATE->hover)) * (1.F - PRESS_SHRINK * std::clamp(eased(STATE->press), 0.F, 1.F));
    if (std::abs(FACTOR - 1.F) < 0.0005F)
        return base;

    const float DW = sc<float>(base.w) * (FACTOR - 1.F);
    const float DH = sc<float>(base.h) * (FACTOR - 1.F);

    // X73 (user: "when mousedover, some of that outline disappears, the bottom
    // is missing"). ROUNDED, and that is a correctness fix rather than tidiness.
    // The grow is a fraction of the tile, so an un-rounded box lands on
    // fractional coordinates, and the border is drawn in the annulus between the
    // box and the box grown by borderSize: at 20px that annulus survives the
    // fractional edges, at 1px the RIGHT and BOTTOM sides round away to nothing
    // while left and top keep theirs. Measured -- top 356px and left 344px of
    // ring against bottom 0 and right 1. Whole pixels in, four edges out.
    CBox grown{base.x - DW / 2.F, base.y - DH / 2.F, base.w + DW, base.h + DH};
    grown.round();

    return grown;
}

// The button's HIT area, as opposed to what is drawn.
//
// Two differences, and both are why the button was 'barely clickable': it
// ignores the visibility ramp, and it carries slop. Since X55 put the button ON
// the corner it overhangs the tile by half its diameter, so for the whole
// approach the cursor is OUTSIDE the tile's own hit area and the tile is already
// un-hovering -- which fades the button out and, with the old gate, emptied its
// hit box at exactly the moment the cursor arrived. A hit area that blinks in
// time with the thing you are reaching for cannot be hit.
// WHERE THE BUTTON IS, in one copy. Both entry points below need the same disc
// and differ only in the question they ask ABOUT it -- can it be seen, can it be
// reached -- so the geometry was written out twice, four identical lines each.
// X55 has already moved this corner once; a second copy is a second place to
// forget.
static CBox closeButtonDisc(const CBox& tileBox, float scale) {
    const float D  = std::clamp(shortSide(tileBox) * CLOSE_FRACTION, CLOSE_MIN * scale, CLOSE_MAX * scale);
    // With CLOSE_INSET 0 those are exactly the tile's top-right corner, and the
    // box is centred on it -- so it overhangs by D/2 on both axes.
    const float CX = sc<float>(tileBox.x + tileBox.w) - D * CLOSE_INSET;
    const float CY = sc<float>(tileBox.y) + D * CLOSE_INSET;

    return CBox{CX - D / 2.F, CY - D / 2.F, D, D};
}

CBox closeButtonHitBox(const PHLWINDOW& window, const CBox& tileBox, float monitorScale) {
    if (!window || tileBox.w < 1 || tileBox.h < 1)
        return {};

    const float SCALE = std::max(monitorScale, 0.01F);

    // V2. SUPER held: the pill has taken the close-X's place, and it is wider
    // than the disc it replaced. Hand back the pill's box so the hover stays
    // alive for the whole reach -- the button being aimed at is the third one
    // along, ~90 px further out than the X ever was.
    if (WindowPill::superHeld())
        return WindowPill::box(nullptr, tileBox, SCALE).expand(CLOSE_SLOP * SCALE);

    return closeButtonDisc(tileBox, SCALE).expand(CLOSE_SLOP * SCALE);
}

CBox closeButtonBox(const PHLWINDOW& window, const CBox& tileBox, float monitorScale) {
    const auto* const STATE = stateFor(window, false);
    if (!STATE || tileBox.w < 1 || tileBox.h < 1)
        return {};

    if (std::clamp(easedHover(STATE->hover), 0.F, 1.F) < CLOSE_VISIBLE_AT)
        return {};

    return closeButtonDisc(tileBox, std::max(monitorScale, 0.01F));
}

void render(PHLMONITOR monitor, const PHLWINDOW& window, const CBox& tileBox, float monitorScale, float alpha) {
    if (!monitor || !window || tileBox.w < 1 || tileBox.h < 1)
        return;

    // X79 (user, 2026-08-06: "app icons from windows sometimes don't disappear
    // until animation/transition is complete"). Everything this function draws
    // is chrome laid OVER the tile, so the tile's own fade does not touch it.
    // One multiplier on all three, at the top, so a new piece of chrome cannot
    // be added later and quietly miss it.
    const float FADE = std::clamp(alpha, 0.F, 1.F);
    if (FADE <= 0.01F)
        return;

    const float SCALE = std::max(monitorScale, 0.01F);

    // X8. GNOME's overview straddles the icon on the preview's BOTTOM edge --
    // the user's words were "little icons under the windows" -- rather than the
    // deprecated overview's top-left inset. Centred, so it reads as a label for
    // the tile rather than as part of its content.
    //
    // V1 (user, 2026-08-17): "Start of animation - size 0. End of overview
    // animation - whatever size the icons in the bottom middle of the windows
    // end up at. So both fade and grow."
    //
    // The grow rides FADE, which is the number the fade already rides: the
    // overview's OWN open progress (overviewGridProgress off the animated
    // scale). It is deliberately not a timeline -- there is no clock here, no
    // start stamp and no state -- so an open that is interrupted, reversed or
    // re-opened carries the icon with it frame for frame instead of finishing a
    // flight the overview abandoned. That is the FlightController lesson written
    // as an absence: a second timeline is the thing that cannot be kept in sync,
    // so there isn't one.
    //
    // The three laws then fall out of the driver instead of being policed on top
    // of it. gridProgress CLAMPS at 1, so the icon cannot pass its resting size
    // however the scale curve behaves near the end (zero overshoot). It reaches
    // 1 exactly when the scale reaches its target, so the icon cannot finish
    // after the overview. And the scale animates on the house DECEL bezier on
    // open, so the growth decelerates because the overview decelerates -- one
    // curve family, not a matching curve.
    //
    // Scaling SIDE alone grows it IN PLACE. The box is centred on the tile's
    // bottom-middle point, so shrinking SIDE keeps that point fixed and the icon
    // does not crawl out of a corner. Below one device pixel there is nothing
    // honest to draw -- a rounded box of zero width is not a small icon, it is a
    // no-op with a texture bound -- so the draw starts when the box does.
    if (const auto TEX = OverviewRender::appIcon(AppSearch::iconForClass(AppSearch::identityClass(window->m_class, window->m_title)))) {
        const float SIDE = std::clamp(shortSide(tileBox) * ICON_FRACTION, ICON_MIN * SCALE, ICON_MAX * SCALE) * FADE;

        CBox iconBox = CBox{sc<float>(tileBox.x + tileBox.w / 2.0) - SIDE / 2.F, sc<float>(tileBox.y + tileBox.h) - SIDE / 2.F, SIDE, SIDE};
        iconBox.round();

        if (iconBox.width >= 1 && iconBox.height >= 1) {
            CTexPassElement::SRenderData icon;
            icon.tex = TEX;
            icon.box = iconBox;
            icon.a   = FADE;
            g_pHyprRenderer->addPassElement(makeUnique<CTexPassElement>(icon));
        }
    }

    // X57. The tile's CAPTION -- queued, not drawn. Dock::render runs after
    // every tile, so a label hanging off a window at the bottom of the active
    // card used to sit under the pill. renderCaptions() flushes these last.
    g_captions.push_back({window, tileBox, FADE, SCALE});

    // V2 (user, 2026-08-17): "on tile mouseover the existing hover CLOSE button
    // is temporarily REPLACED by the 3-button pill". A CROSS-FADE rather than a
    // swap: the pill fades up on its own reveal ease and the X fades out on the
    // complement of the same number, so neither pops and they are never both
    // solid. Both ride the SAME hover ramp above CLOSE_VISIBLE_AT the X already
    // rode, so the pill appears exactly where and when the X used to.
    //
    // UNCLAMPED -- no monitor is passed -- because closeButtonHitBox has no
    // monitor to clamp against either, and a drawn box and a hit box that
    // disagree is the whole class of defect X55 was fixing.
    const float PILL = WindowPill::progress();
    if (PILL > 0.001F) {
        const auto* const PSTATE = stateFor(window, false);
        const float       PALPHA = (PSTATE ? std::clamp((easedHover(PSTATE->hover) - CLOSE_VISIBLE_AT) / (1.F - CLOSE_VISIBLE_AT), 0.F, 1.F) : 1.F) * FADE;
        if (PALPHA > 0.F)
            WindowPill::render(nullptr, tileBox, SCALE, PALPHA);
    }

    const auto BUTTON = closeButtonBox(window, tileBox, monitorScale);
    if (BUTTON.empty())
        return;

    // X55. It FADES rather than popping: the host transitions the button over
    // 100ms. Driven off the hover ramp above CLOSE_VISIBLE_AT, so it needs no
    // state of its own and cannot disagree with the threshold the hit-test uses.
    const auto* const CSTATE = stateFor(window, false);
    const float       ALPHA  = (CSTATE ? std::clamp((easedHover(CSTATE->hover) - CLOSE_VISIBLE_AT) / (1.F - CLOSE_VISIBLE_AT), 0.F, 1.F) : 1.F) * FADE * (1.F - PILL);
    if (ALPHA <= 0.F)
        return;

    const int ROUND = sc<int>(std::round(BUTTON.w / 2.0));

    // X56. The RING is Hyprland's -- size from general:border_size, colour from
    // general:col.active_border -- the same source the tooltip and the drop
    // indicator read, so the button re-themes with everything else.
    const int BORDER = std::max(0, ScrollOverview::Config::getValue<int>("general:border_size"));
    if (BORDER > 0) {
        CRectPassElement::SRectData ring;
        ring.box           = BUTTON.copy().expand(sc<double>(BORDER) * SCALE);
        ring.color         = ScrollOverview::Config::highlightColor().modifyA(ALPHA);
        ring.round         = ROUND + sc<int>(std::round(sc<float>(BORDER) * SCALE));
        ring.roundingPower = 2.F;
        g_pHyprRenderer->addPassElement(makeUnique<CRectPassElement>(ring));
    }

    // The DISC stays light, and that is a legibility constraint rather than a
    // taste one: the symbolic glyph ships as near-black artwork and this render
    // path cannot recolour a texture, so the host's dark disc would hide the X
    // completely. The host gets away with rgba(63,63,70,.98) because CSS
    // recolours its StIcon to #fafafb. Revisit if the glyph ever gets a tint.
    CRectPassElement::SRectData disc;
    disc.box           = BUTTON;
    disc.color         = CHyprColor{0.93F, 0.93F, 0.93F, 0.95F * ALPHA};
    disc.round         = ROUND;
    disc.roundingPower = 2.F;
    g_pHyprRenderer->addPassElement(makeUnique<CRectPassElement>(disc));

    if (const auto GLYPH = OverviewRender::appIcon(CLOSE_ICON)) {
        const float SIDE = sc<float>(BUTTON.w) * CLOSE_GLYPH;

        CTexPassElement::SRenderData x;
        x.tex = GLYPH;
        x.box = CBox{sc<float>(BUTTON.x + BUTTON.w / 2.0) - SIDE / 2.F, sc<float>(BUTTON.y + BUTTON.h / 2.0) - SIDE / 2.F, SIDE, SIDE};
        x.box.round();
        x.a = ALPHA;
        g_pHyprRenderer->addPassElement(makeUnique<CTexPassElement>(x));
    }
}

void renderCaptions(PHLMONITOR monitor) {
    for (const auto& cap : g_captions) {
        if (const auto window = cap.window.lock())
            drawCaption(monitor, window, cap.tileBox, cap.fade, cap.scale);
    }
    g_captions.clear();
}

}
