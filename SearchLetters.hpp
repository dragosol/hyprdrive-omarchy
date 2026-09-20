#pragma once

// Per-glyph bounce for the overview search field.
//
// Ported from Serpantinum's Quickshell Input.qml (ilyamiro/serpantinum,
// src/quickshell/reusables/Input.qml, the ListView add/remove/displaced
// transitions on the character row). Typing pops a letter in from below with
// OutBack overshoot; backspace flings it up and shrinks it; surviving letters
// slide with a smaller OutBack. Durations and overshoot values are the
// Serpantinum numbers, not the rice's house appleDecel/appleAccel pair -- the
// house curves are the standing rule for tiles and chrome, and this field is
// the exception the user asked to keep.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace SearchLetters {

using clock = std::chrono::steady_clock;

inline constexpr float ENTRANCE_MS         = 420.F;
inline constexpr float ENTRANCE_OVERSHOOT  = 3.2F;
inline constexpr float ENTRANCE_SCALE0     = 0.3F;
inline constexpr float ENTRANCE_Y0         = 10.F;
inline constexpr float ENTRANCE_OPACITY_MS = 140.F;
inline constexpr float EXIT_MS             = 160.F;
inline constexpr float EXIT_SCALE          = 0.3F;
inline constexpr float EXIT_Y              = -8.F;
inline constexpr float EXIT_OPACITY_FRAC   = 0.9F;
inline constexpr float DISPLACE_MS         = 260.F;
inline constexpr float DISPLACE_OVERSHOOT  = 1.6F;
inline constexpr float INBACK_OVERSHOOT    = 1.70158F;

inline float clamp01(float t) {
    return t < 0.F ? 0.F : (t > 1.F ? 1.F : t);
}

// Qt Easing.OutBack / InBack, cubic.
inline float easeOutBack(float t, float s) {
    t = clamp01(t) - 1.F;
    return t * t * ((s + 1.F) * t + s) + 1.F;
}

inline float easeInBack(float t, float s = INBACK_OVERSHOOT) {
    t = clamp01(t);
    return t * t * ((s + 1.F) * t - s);
}

inline float easeOutCubic(float t) {
    const float INV = 1.F - clamp01(t);
    return 1.F - INV * INV * INV;
}

inline float easeInCubic(float t) {
    t = clamp01(t);
    return t * t * t;
}

inline size_t utf8Step(unsigned char c) {
    if ((c & 0x80) == 0)
        return 1;
    if ((c & 0xE0) == 0xC0)
        return 2;
    if ((c & 0xF0) == 0xE0)
        return 3;
    if ((c & 0xF8) == 0xF0)
        return 4;
    return 1;
}

inline std::vector<std::string> codepoints(const std::string& s) {
    std::vector<std::string> out;
    for (size_t i = 0; i < s.size();) {
        size_t n = utf8Step(static_cast<unsigned char>(s[i]));
        if (n < 1)
            n = 1;
        if (i + n > s.size())
            n = s.size() - i;
        out.emplace_back(s.substr(i, n));
        i += n;
    }
    return out;
}

struct SEdit {
    size_t                   prefix      = 0;
    size_t                   suffix      = 0;
    size_t                   deleteCount = 0;
    std::vector<std::string> inserted;
};

// Same prefix/suffix walk Serpantinum's syncModel uses, so a middle edit
// removes only the changed span and the rest keep their birth times.
inline SEdit diff(const std::vector<std::string>& oldLive, const std::vector<std::string>& neu) {
    const size_t OLDN = oldLive.size();
    const size_t NEWN = neu.size();
    size_t       prefix = 0;
    while (prefix < OLDN && prefix < NEWN && oldLive[prefix] == neu[prefix])
        ++prefix;
    size_t suffix = 0;
    while (suffix < (OLDN - prefix) && suffix < (NEWN - prefix) && oldLive[OLDN - 1 - suffix] == neu[NEWN - 1 - suffix])
        ++suffix;
    SEdit e;
    e.prefix      = prefix;
    e.suffix      = suffix;
    e.deleteCount = OLDN - prefix - suffix;
    e.inserted.assign(neu.begin() + static_cast<std::ptrdiff_t>(prefix), neu.begin() + static_cast<std::ptrdiff_t>(NEWN - suffix));
    return e;
}

inline float msSince(clock::time_point now, clock::time_point then) {
    return std::chrono::duration<float, std::milli>(now - then).count();
}

struct SPose {
    float scale    = 1.F;
    float y        = 0.F; // logical px, +down, times the pill scale at draw
    float opacity  = 1.F;
};

inline SPose entrancePose(float ms) {
    const float T = clamp01(ms / ENTRANCE_MS);
    const float E = easeOutBack(T, ENTRANCE_OVERSHOOT);
    SPose       p;
    p.scale   = ENTRANCE_SCALE0 + (1.F - ENTRANCE_SCALE0) * E;
    p.y       = ENTRANCE_Y0 * (1.F - easeOutBack(T, ENTRANCE_OVERSHOOT * 0.8F));
    p.opacity = easeOutCubic(clamp01(ms / ENTRANCE_OPACITY_MS));
    return p;
}

inline SPose exitPose(float ms) {
    const float T = clamp01(ms / EXIT_MS);
    SPose       p;
    p.scale   = 1.F + (EXIT_SCALE - 1.F) * easeInBack(T);
    p.y       = EXIT_Y * easeInCubic(T);
    p.opacity = 1.F - easeInCubic(clamp01(ms / (EXIT_MS * EXIT_OPACITY_FRAC)));
    return p;
}

inline float displaceEase(float ms) {
    return easeOutBack(clamp01(ms / DISPLACE_MS), DISPLACE_OVERSHOOT);
}

struct SPlaced {
    std::string glyph;
    float       x        = 0.F;
    float       y        = 0.F;
    float       scale    = 1.F;
    float       opacity  = 1.F;
    bool        dying    = false;
};

// Where one letter sits in ONE view -- one monitor's search field. Positions are
// in that view's pixels, and every monitor draws the same letters at its own
// scale, so a single shared restX was rewritten by each monitor in turn: each
// write looked like the text moving, restarted the displacement ease, and the
// whole query shivered between the two monitors' positions every frame (user,
// 2026-09-14). The letters themselves -- what they are, when they were born and
// died -- stay shared: that is the query, and there is one query.
struct SPlacement {
    int               view    = 0;
    float             restX   = 0.F;
    float             fromX   = 0.F;
    clock::time_point movedAt{};
};

struct SLetter {
    std::string                      glyph;
    clock::time_point                bornAt{};
    std::optional<clock::time_point> dyingAt;
    std::vector<SPlacement>          placements; // one per view that has laid it out

    SPlacement* placementIn(int view) {
        for (auto& P : placements)
            if (P.view == view)
                return &P;
        return nullptr;
    }
};

struct SAnimator {
    std::vector<SLetter> letters;

    // DIAGNOSTIC (2026-09-19). How many letters had their displacement ease
    // RE-AIMED during the last place(). A retarget on a frame where nothing was
    // typed means the layout input moved under the animation, which restarts a
    // 260ms OutBack from wherever it had reached -- an ease that never finishes
    // its curve reads as drag, and the error multiplies along the run because
    // every letter's target is the sum of the ones before it.
    int lastRetargets = 0;

    void reset() { letters.clear(); }

    bool empty() const { return letters.empty(); }

    std::vector<std::string> liveGlyphs() const {
        std::vector<std::string> g;
        g.reserve(letters.size());
        for (const auto& L : letters) {
            if (!L.dyingAt)
                g.push_back(L.glyph);
        }
        return g;
    }

    void prune(clock::time_point now) {
        std::erase_if(letters, [&](const SLetter& L) {
            return L.dyingAt && msSince(now, *L.dyingAt) >= EXIT_MS;
        });
    }

    void sync(const std::string& query, clock::time_point now) {
        prune(now);
        const auto LIVE = liveGlyphs();
        const auto NEU  = codepoints(query);
        const auto E    = diff(LIVE, NEU);
        if (E.deleteCount == 0 && E.inserted.empty())
            return;

        size_t liveI = 0;
        size_t vecI  = 0;
        while (vecI < letters.size() && liveI < E.prefix) {
            if (!letters[vecI].dyingAt)
                ++liveI;
            ++vecI;
        }
        while (vecI < letters.size() && letters[vecI].dyingAt)
            ++vecI;

        size_t marked = 0;
        for (size_t i = vecI; i < letters.size() && marked < E.deleteCount; ++i) {
            if (letters[i].dyingAt)
                continue;
            letters[i].dyingAt = now;
            ++marked;
        }

        if (!E.inserted.empty()) {
            std::vector<SLetter> add;
            add.reserve(E.inserted.size());
            for (const auto& G : E.inserted)
                add.push_back(SLetter{.glyph = G, .bornAt = now});
            letters.insert(letters.begin() + static_cast<std::ptrdiff_t>(vecI), add.begin(), add.end());
        }
    }

    bool busy(clock::time_point now) const {
        for (const auto& L : letters) {
            if (L.dyingAt) {
                if (msSince(now, *L.dyingAt) < EXIT_MS)
                    return true;
                continue;
            }
            if (msSince(now, L.bornAt) < ENTRANCE_MS)
                return true;
            for (const auto& P : L.placements)
                if (std::abs(P.restX - P.fromX) > 0.05F && msSince(now, P.movedAt) < DISPLACE_MS)
                    return true;
        }
        return false;
    }

    // liveWidths[i] is the rendered width of the i-th live glyph. yUnit is the
    // pill's measured scale so the 10px / -8px hops track the field. `view` names
    // whose pixels those are (the monitor): each view keeps its own positions.
    std::vector<SPlaced> place(const std::vector<float>& liveWidths, float spacing, float yUnit, clock::time_point now, int view = 0) {
        prune(now);
        lastRetargets = 0;
        std::vector<SPlaced> out;
        out.reserve(letters.size());

        float  x     = 0.F;
        size_t liveI = 0;
        for (auto& L : letters) {
            SPlacement* P = L.placementIn(view);
            if (!L.dyingAt) {
                const float W      = liveI < liveWidths.size() ? liveWidths[liveI] : 0.F;
                const float TARGET = x;
                if (!P) {
                    L.placements.push_back(SPlacement{.view = view, .restX = TARGET, .fromX = TARGET, .movedAt = now});
                    P = &L.placements.back();
                } else if (std::abs(P->restX - TARGET) > 0.05F) {
                    ++lastRetargets;
                    const float TDISP = displaceEase(msSince(now, P->movedAt));
                    P->fromX          = P->fromX + (P->restX - P->fromX) * TDISP;
                    P->restX          = TARGET;
                    P->movedAt        = now;
                }
                x += W + spacing;
                ++liveI;
            }

            // A letter already dying before this view ever laid it out has
            // nowhere of its own to leave from: it leaves from where the live
            // run ends in this view.
            const float TDISP = P ? displaceEase(msSince(now, P->movedAt)) : 1.F;
            const float DRAWX = P ? P->fromX + (P->restX - P->fromX) * TDISP : x;
            const SPose POSE  = L.dyingAt ? exitPose(msSince(now, *L.dyingAt)) : entrancePose(msSince(now, L.bornAt));

            out.push_back(SPlaced{
                .glyph    = L.glyph,
                .x        = DRAWX,
                .y        = POSE.y * yUnit,
                .scale    = POSE.scale,
                .opacity  = POSE.opacity,
                .dying    = L.dyingAt.has_value(),
            });
        }
        return out;
    }

    float liveEndX(const std::vector<float>& liveWidths, float spacing) const {
        float x = 0.F;
        for (size_t i = 0; i < liveWidths.size(); ++i) {
            x += liveWidths[i];
            if (i + 1 < liveWidths.size())
                x += spacing;
        }
        return x;
    }
};

inline SAnimator g_searchLetters;

}
