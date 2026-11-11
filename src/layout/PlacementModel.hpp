#pragma once

// WHERE A NEW WINDOW GOES, PURE (Placement.cpp gathers the facts and applies
// the answer). The owner, 2026-11-11: "setup an enforcer rule to prevent
// windows ever being floating unless the user has intentionally toggled
// floating, and this rule should apply to any wine and proton windows too. if
// a floating window shows up over a fullscreen one, it should be moved to the
// next desktop ... make this an elegant break resistant mechanism ... not a
// collection of rules that might conflict with each other upon further
// expansion."
//
// So ONE decision, taken once as the window maps (window.openEarly: before
// Hyprland files it into a layout and before its fullscreen takeover), from a
// closed set of facts to a closed set of outcomes. Everything that used to
// decide part of it on its own (Mosaic's adopt-floating, its fixed-size
// exemption, besideFullscreen's "not for floats", the dock's dialog lists)
// reads this instead.
//
// The rulings it encodes (owner, 2026-11-11):
//   * the ENFORCER -- no window floats unless the user floated it -- is on
//     where general:open_tiled is on (every layout), and ALWAYS on a Mosaic
//     desktop (enforce);
//   * a static float RULE in the config is honoured: Float. Only what the app
//     or toolkit asks for (a dialog hint, a transient, min == max, a layout's
//     refusal) is overridden;
//   * the `hyprdrive-float` tag, and the user's own toggle, are intent: Float;
//   * splash screens and utility / tool palettes are NOT tiled: Leave;
//   * BESIDE: a window that opens over a fullscreen app goes to the next
//     desktop -- whatever the option, tiled or floating, a dialog or not --
//     except under fullscreen_desktop = stay, and except the app's own passing
//     windows (its splash, its tool palette stay with it).
//
// No Hyprland types: tests/unit/placement_model_test.cpp.

#include <cstdint>
#include <deque>

namespace Hyprdrive::PlacementModel {
    // What the USER said about this window's floating (Placement keeps it).
    enum class EIntent : uint8_t {
        None,
        Float, // Super+T / hyprctl / the pill / the trackpad float, or the tag
        Tile,  // the user tiled it
    };

    struct SWindowFacts {
        // Not a window that can be placed at all: an X11 override-redirect
        // window, a menu / tooltip / notification / dock type, a pinned one,
        // one on a special desktop (the minimize stash, a scratchpad).
        bool placeable = true;
        // A splash screen, a utility or toolbar palette (ruling 5): Hyprland
        // places it as it always has.
        bool passing   = false;
        bool floating  = false; // floating as it maps, for whatever reason
        bool ruleFloat = false; // ...because a static window rule says so
        bool tagged    = false; // the hyprdrive-float tag
        bool fixedSize = false; // min == max (OwnSizeTile::fixedSize)
        bool child     = false; // a parent window (xdg parent, X11 transient-for)
        bool modal     = false;
    };

    struct SPlaceFacts {
        bool enforce       = false; // general:open_tiled, or a Mosaic desktop
        bool fullscreenApp = false; // a fullscreen app we track is on the desktop (not this window)
        bool sameApp       = false; // ...and this window is that app's
        bool stay          = false; // fullscreen_desktop = stay
    };

    enum class EOutcome : uint8_t {
        Leave,  // not ours: Hyprland as it is
        Float,  // it floats (the user's intent, a rule, or nobody enforcing)
        Beside, // it goes to the next desktop after the fullscreen app
        Tile,   // the desktop's layout takes it (own size when fixed)
    };
    enum class EWhy : uint8_t {
        NotPlaceable,
        Passing,
        Intent,
        Rule,
        NotEnforced,
        Tiled,       // it was tiled already
        Enforced,    // an app's float, overridden
        UserTiled,
        OverFullscreen,
    };

    struct SOutcome {
        EOutcome kind     = EOutcome::Leave;
        EWhy     why      = EWhy::NotPlaceable;
        bool     ownSize  = false; // Tile: at its own size (fixed-size)
        bool     prompted = false; // Beside: the user asked for it
        constexpr bool operator==(const SOutcome&) const = default;
    };

    constexpr const char* whyName(EWhy w) {
        switch (w) {
            case EWhy::NotPlaceable: return "not placeable";
            case EWhy::Passing: return "a splash or palette";
            case EWhy::Intent: return "the user floated it";
            case EWhy::Rule: return "a float rule";
            case EWhy::NotEnforced: return "the app's float (enforcer off here)";
            case EWhy::Tiled: return "tiled";
            case EWhy::Enforced: return "the app's float, overridden";
            case EWhy::UserTiled: return "the user tiled it";
            case EWhy::OverFullscreen: return "over a fullscreen app";
        }
        return "?";
    }

    // THE DECISION, first match.
    constexpr SOutcome decide(const SWindowFacts& w, EIntent intent, const SPlaceFacts& p, bool prompted) {
        if (!w.placeable)
            return {.kind = EOutcome::Leave, .why = EWhy::NotPlaceable};
        if (p.fullscreenApp && !p.stay && !(w.passing && p.sameApp))
            return {.kind = EOutcome::Beside, .why = EWhy::OverFullscreen, .prompted = prompted};
        if (w.passing)
            return {.kind = EOutcome::Leave, .why = EWhy::Passing};
        if (intent == EIntent::Float || w.tagged)
            return {.kind = EOutcome::Float, .why = EWhy::Intent};
        if (intent == EIntent::Tile)
            return {.kind = EOutcome::Tile, .why = EWhy::UserTiled, .ownSize = w.fixedSize};
        if (w.ruleFloat)
            return {.kind = EOutcome::Float, .why = EWhy::Rule};
        if (!w.floating)
            return {.kind = EOutcome::Tile, .why = EWhy::Tiled, .ownSize = w.fixedSize};
        if (!p.enforce)
            return {.kind = EOutcome::Float, .why = EWhy::NotEnforced};
        return {.kind = EOutcome::Tile, .why = EWhy::Enforced, .ownSize = w.fixedSize};
    }

    // A WINDOW THAT BECAME FLOATING LATER (the funnel: a layout refused it,
    // a group, a swap -- never the user, a drag or the stash): tiled again
    // when the decision for it, now, is Tile by enforcement.
    constexpr bool retile(const SWindowFacts& w, EIntent intent, const SPlaceFacts& p) {
        const SOutcome O = decide(w, intent, {.enforce = p.enforce}, false);
        return O.kind == EOutcome::Tile && O.why == EWhy::Enforced;
    }

    // THE ENFORCEMENT BUDGET (Battle.net's login: a fixed-size window a layout
    // keeps refusing, or a client that keeps changing its size hints). At most
    // RETILES re-tiles within SPAN ms; past that the window is left floating
    // for the rest of its life (logged once by the caller).
    class CBudget {
      public:
        static constexpr int     RETILES = 2;
        static constexpr int64_t SPAN    = 3000;

        // A re-tile is wanted at `now`: true when it may go ahead.
        bool spend(int64_t now) {
            if (m_spent)
                return false;
            while (!m_at.empty() && now - m_at.front() >= SPAN)
                m_at.pop_front();
            if (static_cast<int>(m_at.size()) >= RETILES) {
                m_spent = true;
                return false;
            }
            m_at.push_back(now);
            return true;
        }
        bool spent() const {
            return m_spent;
        }

      private:
        std::deque<int64_t> m_at;
        bool                m_spent = false;
    };
}
