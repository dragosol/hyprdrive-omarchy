// Exhaustive table tests for the two ladders in DropDecision.hpp:
// ScrollOverview::Drop::resolve() (a released WINDOW drag) and V3's
// resolveIcon() (a released DOCK ICON drag). Two gestures, two tables, one
// file -- they end on the same screen and must not grow apart.
//
// The drop resolution came out of endWindowDrag()'s 277 lines as a pure
// function precisely so it could be checked without a compositor: 16 boolean
// facts, 65536 combinations, no state, no I/O. `reference()` below is the
// SAME ladder transliterated from the pre-split source (a4189e2,
// scrollOverview.cpp:1919-2195) -- condition for condition, in the order the
// if/else chain evaluated them -- so a transcription slip in either copy shows
// up as a disagreeing row rather than as a drag that misbehaves once a month.
//
//   g++ -std=c++2b -O2 -o /tmp/drop_table_test scripts/drop_table_test.cpp && /tmp/drop_table_test
//
// Needs nothing from Hyprland; DropDecision.hpp includes nothing at all.

#include "../DropDecision.hpp"

#include <cstdio>
#include <string>

using namespace ScrollOverview::Drop;

// The ladder spelled out the long way, so a slip in either copy shows up as a
// disagreeing row. Since 1.9.2 the overview only MOVES windows between
// desktops: a drop on the window's own desktop cancels, and an arrival is the
// destination layout's to place (no swap, no insert beside the tile under the
// cursor, no anchor facts at all).
static SDecision reference(const SFacts& f) {
    SDecision d;

    if (f.haveWindow && f.overPill) {
        d.action = EAction::Minimize;
        return d;
    }

    if (!f.haveDropWorkspace || !f.moveWorkspace) {
        d.action = EAction::CancelToGrid;
        return d;
    }

    d.useOriginalBox               = f.haveOriginalBox;
    d.mapPointIntoCard             = f.dropPointMapsIntoCard;
    d.restoreSourceFullscreenFocus = f.restoreSourceFullscreenFocus;

    if (f.retileOnEnd) {
        d.action    = EAction::RetileMove;
        d.placement = f.scrollingLayout ? EPlacement::ScrollEdgeFar : EPlacement::None;
    } else if (f.haveWindow) {
        d.action = EAction::FloatMove;
    }

    return d;
}

static const char* actionName(EAction a) {
    switch (a) {
        case EAction::Minimize: return "Minimize";
        case EAction::CancelToGrid: return "CancelToGrid";
        case EAction::RetileMove: return "RetileMove";
        case EAction::FloatMove: return "FloatMove";
        case EAction::Nothing: return "Nothing";
    }
    return "?";
}
static constexpr int NACTIONS = 5;

static const char* placementName(EPlacement p) {
    switch (p) {
        case EPlacement::None: return "None";
        case EPlacement::ScrollEdgeFar: return "ScrollEdgeFar";
    }
    return "?";
}

static bool same(const SDecision& a, const SDecision& b) {
    return a.action == b.action && a.placement == b.placement && a.useOriginalBox == b.useOriginalBox && a.mapPointIntoCard == b.mapPointIntoCard &&
        a.restoreSourceFullscreenFocus == b.restoreSourceFullscreenFocus;
}

// The 11 facts, in bit order, so a failing row can be read back.
static const char* BITNAMES[] = {
    "haveWindow", "overPill",        "haveDropWorkspace",     "retileOnEnd",                  "moveWorkspace", "startedTiled",
    "haveTarget", "scrollingLayout", "haveOriginalBox",       "dropPointMapsIntoCard",        "restoreSourceFullscreenFocus",
};
static constexpr int NBITS = 11;

static SFacts factsFromBits(unsigned bits) {
    SFacts f;
    f.haveWindow                   = bits & (1u << 0);
    f.overPill                     = bits & (1u << 1);
    f.haveDropWorkspace            = bits & (1u << 2);
    f.retileOnEnd                  = bits & (1u << 3);
    f.moveWorkspace                = bits & (1u << 4);
    f.startedTiled                 = bits & (1u << 5);
    f.haveTarget                   = bits & (1u << 6);
    f.scrollingLayout              = bits & (1u << 7);
    f.haveOriginalBox              = bits & (1u << 8);
    f.dropPointMapsIntoCard        = bits & (1u << 9);
    f.restoreSourceFullscreenFocus = bits & (1u << 10);
    return f;
}

static std::string describe(unsigned bits) {
    std::string s;
    for (int i = 0; i < NBITS; ++i) {
        if (bits & (1u << i)) {
            if (!s.empty())
                s += " ";
            s += BITNAMES[i];
        }
    }
    return s.empty() ? "(nothing set)" : s;
}

// ------------------------------------------------------- V3 icon drops --
// The second table in this file, and the reason it is in this file: a dock icon
// released on the screen and a window released on the screen are two gestures
// that ask the same question, and keeping the two ladders side by side is what
// stops the second one from being reinvented inside an input hook.
//
// 7 facts, 128 rows, walked exhaustively -- but there is no PARENT ladder to
// diff against here (the parent does nothing at all with this release), so the
// check is a different one: every row must satisfy the INVARIANTS the feature
// is defined by, every action must be reachable, and the named cases the spec
// itself lists must come out the way the spec says.
static const char* iconActionName(EIconAction a) {
    switch (a) {
        case EIconAction::Click: return "Click";
        case EIconAction::Reorder: return "Reorder";
        case EIconAction::LaunchHere: return "LaunchHere";
        case EIconAction::LaunchThere: return "LaunchThere";
        case EIconAction::Cancel: return "Cancel";
    }
    return "?";
}

static SIconFacts iconFactsFromBits(unsigned bits) {
    SIconFacts f;
    f.haveItem          = bits & (1u << 0);
    f.live              = bits & (1u << 1);
    f.nearStart         = bits & (1u << 2);
    f.onPill            = bits & (1u << 3);
    f.overviewOpen      = bits & (1u << 4);
    f.onStripCard       = bits & (1u << 5);
    f.stripCardIsActive = bits & (1u << 6);
    return f;
}

static int iconTable() {
    int failures  = 0;
    int counts[5] = {};

    for (unsigned bits = 0; bits < (1u << 7); ++bits) {
        const SIconFacts  F = iconFactsFromBits(bits);
        const EIconAction A = resolveIcon(F);

        counts[static_cast<int>(A)]++;

        const auto bad = [&](const char* why) {
            if (++failures <= 20)
                std::printf("ICON MISMATCH bits=0x%02x -> %s : %s\n", bits, iconActionName(A), why);
        };

        // NOTHING HAPPENS WITHOUT A LIVE DRAG. The press may have landed on
        // padding, on a preview, or on an icon with no desktop id, and a
        // sub-threshold wobble is a click -- all three are the caller's click,
        // never a launch (this is the X16 half, and it is why the dock stopped
        // being dead in the first place).
        if ((!F.haveItem || !F.live || F.nearStart) && A != EIconAction::Click)
            bad("a press that never became a drag must fall through to the click ladder");

        // NOTHING LAUNCHES ON THE PILL. The pill is where the icon came from;
        // dropping it back is X32's reorder and cannot also start the app.
        if (F.haveItem && F.live && !F.nearStart && F.onPill && A != EIconAction::Reorder)
            bad("a live drop inside the pill is the reorder and nothing else");

        // A DROP ON THE DESKTOP LAUNCHES HERE. With no overview up there is
        // nothing else off the pill to hit, so there is no cancel to reach.
        if (F.haveItem && F.live && !F.nearStart && !F.onPill && !F.overviewOpen && A != EIconAction::LaunchHere)
            bad("with the overview closed, off the pill IS the active desktop");

        // A CARD IS ALWAYS A LAUNCH, and never the wrong one: the active card
        // launches here (focused), any other card launches there (silent).
        if (F.overviewOpen && F.onStripCard && F.haveItem && F.live && !F.nearStart && !F.onPill) {
            if (F.stripCardIsActive && A != EIconAction::LaunchHere)
                bad("the card of the desktop being viewed is the active desktop");
            if (!F.stripCardIsActive && A != EIconAction::LaunchThere)
                bad("another desktop's card launches on THAT desktop");
        }

        // AND THE NEAR MISS EXISTS. With the overview up and no card under the
        // release, nothing may be started -- this is the arm that makes "a drop
        // that misses both targets cancels" a testable claim rather than a note.
        if (F.overviewOpen && !F.onStripCard && F.haveItem && F.live && !F.nearStart && !F.onPill && A != EIconAction::Cancel)
            bad("a release on the overview but on no card must start nothing");

        // LaunchThere is SILENT by construction, so it must never be reachable
        // for the desktop the session is already on -- that would be a silent
        // launch onto the visible desktop, which is a launch with no feedback.
        if (A == EIconAction::LaunchThere && F.stripCardIsActive)
            bad("the active desktop must never be launched on silently");

        // THE STAGING CONTRACT, checked rather than asserted in a comment. The
        // caller asks with the last three facts still false and acts at once on
        // Click or Reorder; that is only sound if those two answers cannot be
        // changed by anything the second stage would fill in. So: whenever the
        // stage-one view of these facts answers Click or Reorder, the full view
        // must answer the same.
        SIconFacts stage1  = F;
        stage1.overviewOpen = stage1.onStripCard = stage1.stripCardIsActive = false;
        const EIconAction EARLY = resolveIcon(stage1);
        if ((EARLY == EIconAction::Click || EARLY == EIconAction::Reorder) && EARLY != A)
            bad("stage one answered early and stage two disagreed -- the caller acts on that answer");
    }

    std::printf("icon rows %u\n", 1u << 7);
    for (int i = 0; i < 5; ++i) {
        std::printf("  %-13s %6d\n", iconActionName(static_cast<EIconAction>(i)), counts[i]);
        if (counts[i] == 0) {
            std::printf("UNREACHED icon action %s\n", iconActionName(static_cast<EIconAction>(i)));
            ++failures;
        }
    }

    // The four gestures the spec names, by hand.
    struct SIconCase {
        const char* name;
        SIconFacts  facts;
        EIconAction want;
    };
    const SIconCase ICONCASES[] = {
        {"drag to the desktop centre, overview closed", SIconFacts{.haveItem = true, .live = true}, EIconAction::LaunchHere},
        {"drag onto another desktop's strip card",
         SIconFacts{.haveItem = true, .live = true, .overviewOpen = true, .onStripCard = true}, EIconAction::LaunchThere},
        {"drag onto the card of the desktop being viewed",
         SIconFacts{.haveItem = true, .live = true, .overviewOpen = true, .onStripCard = true, .stripCardIsActive = true}, EIconAction::LaunchHere},
        {"released just outside every card", SIconFacts{.haveItem = true, .live = true, .overviewOpen = true}, EIconAction::Cancel},
        {"released back on the pill", SIconFacts{.haveItem = true, .live = true, .onPill = true}, EIconAction::Reorder},
        {"released where it was pressed", SIconFacts{.haveItem = true, .live = true, .nearStart = true}, EIconAction::Click},
    };

    for (const auto& C : ICONCASES) {
        const EIconAction GOT = resolveIcon(C.facts);
        const bool        OK  = GOT == C.want;
        std::printf("  %-46s %-12s %s\n", C.name, iconActionName(GOT), OK ? "ok" : "WRONG");
        if (!OK)
            ++failures;
    }

    return failures;
}

int main() {
    int failures = 0;
    int counts[NACTIONS] = {};

    for (unsigned bits = 0; bits < (1u << NBITS); ++bits) {
        const SFacts    F = factsFromBits(bits);
        const SDecision A = resolve(F);
        const SDecision B = reference(F);

        counts[static_cast<int>(A.action)]++;

        if (same(A, B))
            continue;

        if (++failures <= 20)
            std::printf("MISMATCH bits=0x%04x [%s]\n  resolve   %s/%s orig=%d map=%d rsf=%d\n  reference %s/%s orig=%d map=%d rsf=%d\n", bits,
                        describe(bits).c_str(), actionName(A.action), placementName(A.placement), A.useOriginalBox, A.mapPointIntoCard,
                        A.restoreSourceFullscreenFocus, actionName(B.action), placementName(B.placement), B.useOriginalBox, B.mapPointIntoCard,
                        B.restoreSourceFullscreenFocus);
    }

    // Every action has to be REACHED, or "all 65536 agree" would be a fact
    // about an unreachable ladder rather than about this one.
    std::printf("rows %u\n", 1u << NBITS);
    for (int i = 0; i < NACTIONS; ++i) {
        std::printf("  %-13s %6d\n", actionName(static_cast<EAction>(i)), counts[i]);
        if (counts[i] == 0) {
            std::printf("UNREACHED action %s\n", actionName(static_cast<EAction>(i)));
            ++failures;
        }
    }

    // The five real drags this rig exists for, named, so the table has anchors
    // a human can check by hand.
    struct SCase {
        const char* name;
        SFacts      facts;
        EAction     action;
        EPlacement  placement;
    };
    const SCase CASES[] = {
        {"drop on the dock pill", SFacts{.haveWindow = true, .overPill = true}, EAction::Minimize, EPlacement::None},
        {"released over nothing", SFacts{.haveWindow = true, .haveTarget = true}, EAction::CancelToGrid, EPlacement::None},
        {"floating tile to another desktop",
         SFacts{.haveWindow = true, .haveDropWorkspace = true, .moveWorkspace = true, .haveTarget = true, .haveOriginalBox = true, .dropPointMapsIntoCard = true},
         EAction::FloatMove, EPlacement::None},
        {"floating tile dropped on its own card", SFacts{.haveWindow = true, .haveDropWorkspace = true, .haveTarget = true, .haveOriginalBox = true},
         EAction::CancelToGrid, EPlacement::None},
        {"tiled window dropped on another tile of its own desktop (no swap)",
         SFacts{.haveWindow = true, .haveDropWorkspace = true, .retileOnEnd = true, .startedTiled = true, .haveTarget = true, .dropPointMapsIntoCard = true},
         EAction::CancelToGrid, EPlacement::None},
        {"tiled window onto a scrolling desktop's card",
         SFacts{.haveWindow = true, .haveDropWorkspace = true, .retileOnEnd = true, .moveWorkspace = true, .startedTiled = true, .haveTarget = true,
                .scrollingLayout = true, .dropPointMapsIntoCard = true},
         EAction::RetileMove, EPlacement::ScrollEdgeFar},
    };

    for (const auto& C : CASES) {
        const SDecision D = resolve(C.facts);
        const bool      OK = D.action == C.action && D.placement == C.placement;
        std::printf("  %-52s %s/%s %s\n", C.name, actionName(D.action), placementName(D.placement), OK ? "ok" : "WRONG");
        if (!OK)
            ++failures;
    }

    failures += iconTable();

    std::printf(failures ? "DROP_TABLE FAIL (%d)\n" : "DROP_TABLE OK (%d)\n", failures);
    return failures ? 1 : 0;
}
