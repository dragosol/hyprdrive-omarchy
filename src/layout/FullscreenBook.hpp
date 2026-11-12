#pragma once

// THE FULLSCREEN DESKTOP'S BOOKKEEPING, PURE (FullscreenDesktop.cpp keeps the
// trips and debts; these are the decisions it takes about them). Desktops are
// numbers here: no Hyprland types, so tests/unit/fullscreen_book_test.cpp can
// run thousands of random fullscreen on/off, open-beside, close, monitor and
// spam sequences through the same decisions and check that the desktops come
// back to the numbers they started on.
//
// A PLACE is a desktop number. A trip, and a debt, keep theirs by number (`at`)
// next to the desktop in it: the desktop can go (Hyprland drops an empty one
// the moment nothing holds it -- a fullscreen app closed while its desktop is
// not on screen), and the number is what is owed back. The old debt took its
// number from the desktop alone, -1 when it had already gone, and was paid back
// at place -1: shift(0, -1).

#include "Row.hpp"

#include <cstdint>
#include <cstdio>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Hyprdrive::FullscreenBook {
    using Id                  = long long;
    inline constexpr Id NO_PLACE = -1;

    // Where an owed place is now: the number of the desktop in it while it
    // lives, else the number kept.
    constexpr Id placeOf(bool alive, Id liveId, Id kept) {
        return alive && liveId >= 1 ? liveId : kept;
    }
    // The desktops from `from` on moved `delta` places: a kept number of a place
    // with no desktop in it moves with them (one with a desktop follows it).
    constexpr Id movedPlace(Id kept, Id from, int delta) {
        return kept >= from ? kept + delta : kept;
    }

    // ON A MONITOR'S ROW (Row.hpp): places are the row's slots. The place
    // after `id` is its next slot -- a block's last desktop is followed by
    // the row's first overflow place, never by the next block's first id --
    // NO_PLACE for an id outside the row (a visitor's). A kept number moves
    // only with its own row's desktops. The implicit row is the number line.
    constexpr Id nextPlace(const Row& row, Id id) {
        if (!row.bounded())
            return id + 1;
        const Id S = row.slot(id);
        const Id N = S ? row.idOf(S + 1) : 0;
        return N ? N : NO_PLACE;
    }
    constexpr Id movedPlace(const Row& row, Id kept, Id from, int delta) {
        if (!row.bounded())
            return movedPlace(kept, from, delta);
        const Id K = row.slot(kept), F = row.slot(from);
        if (!K || !F || K < F)
            return kept;
        const Id TO = row.idOf(K + delta);
        return TO > 0 ? TO : kept;
    }

    // GOING FULLSCREEN with others on its desktop (`home`):
    //   stay, or alone      -> settle: it keeps its desktop
    //   next (the default)  -> home + 1: that place if it is free, else room is
    //                          made there (owed back)
    //   free                -> the first free place after home; nothing moves
    struct SStart {
        bool settle   = false;
        Id   at       = NO_PLACE;
        bool makeRoom = false;
    };
    // On a row, "after" is along the row (nextPlace); a home outside it has
    // no place after it: it settles.
    template <class FREE>
    SStart start(std::string_view mode, const Row& row, Id home, int othersOnHome, FREE&& isFree) {
        if (mode == "stay" || othersOnHome == 0 || home < 1)
            return {.settle = true};
        Id at = nextPlace(row, home);
        if (at == NO_PLACE)
            return {.settle = true};
        if (mode == "free") {
            for (int n = 0; !isFree(at) && n < 1000; ++n)
                if (const Id NEXT = nextPlace(row, at); NEXT != NO_PLACE)
                    at = NEXT;
                else
                    return {.settle = true};
            return {.at = at};
        }
        return {.at = at, .makeRoom = !isFree(at)};
    }
    template <class FREE>
    SStart start(std::string_view mode, Id home, int othersOnHome, FREE&& isFree) {
        return start(mode, Row{}, home, othersOnHome, isFree);
    }

    // A WINDOW OPENED OVER A FULLSCREEN APP on desktop `appDesk`: it gets the
    // next place, made if it is not free -- and then owed back, like a trip's.
    // In the app's monitor's row: a block's 10th desktop is followed by the
    // row's overflow, never by the next block's first id.
    template <class FREE>
    SStart beside(const Row& row, Id appDesk, FREE&& isFree) {
        const Id AT = appDesk < 1 ? NO_PLACE : nextPlace(row, appDesk);
        if (AT == NO_PLACE)
            return {.settle = true};
        return {.at = AT, .makeRoom = !isFree(AT)};
    }
    template <class FREE>
    SStart beside(Id appDesk, FREE&& isFree) {
        return beside(Row{}, appDesk, isFree);
    }
    // A BESIDE LINK (Placement.cpp, FullscreenDesktop::beside): windows that
    // opened over a fullscreen app were given the desktop after it. While the
    // link lives that desktop is the app's companion: further windows of the
    // same app join it, and when the last of them closes the user who was
    // there for them (he asked for it -- `wanted` -- and is looking at it) is
    // taken back to the app (the way back, owner 2026-11-11: "upon closing the
    // window, user should be sent back to the fullscreen app"). Then the link
    // ends, and what it owes is a Beside debt like any other.
    //   Hold        keep it (windows there, a strip sliding, a remap's gap)
    //   ReturnToApp the view goes back to the app, then the link ends
    //   Drop        the link ends, nobody moves
    enum class ELink { Hold, ReturnToApp, Drop };
    struct SLinkFacts {
        bool appAlive      = false; // the fullscreen app's window is mapped
        bool appFullscreen = false; // ...and still fullscreen on its own desktop
        bool spaceAlive    = false; // the link's desktop exists
        bool any           = false; // ...holding windows of the link
        bool foreign       = false; // ...or a window of someone else
        bool wanted        = false; // prompted, or the user went to it since
        bool viewOn        = false; // the monitor shows the link's desktop
        bool driving       = false; // a desktop strip is sliding
        bool settling      = false; // emptied a moment ago (a Wine remap is a close, then a map)
    };
    constexpr ELink link(const SLinkFacts& f) {
        if (!f.spaceAlive || f.foreign)
            return ELink::Drop;
        if (f.any)
            return f.appAlive && f.appFullscreen ? ELink::Hold : ELink::Drop;
        // Empty.
        if (!f.appAlive || !f.appFullscreen)
            return ELink::Drop;
        if (f.settling || f.driving)
            return ELink::Hold;
        return f.wanted && f.viewOn ? ELink::ReturnToApp : ELink::Drop;
    }

    // A WINDOW OPENING ON A BESIDE DESKTOP AFTER ITS LINK ENDED (the 30-cycle
    // race, 2026-11-11): the way back ended the link and handed its place
    // over as a debt; a window launched from that desktop a moment before
    // then maps on it (during the glide back or after). Counted as someone
    // else's, it made the place "a desktop of its own" -- nothing owed -- and
    // once it closed, the room made for the link was never given back. It is
    // the link's: while the app is still fullscreen on its desktop, the debt
    // becomes the link again, owing what it owed.
    struct SRelinkFacts {
        bool besideDebt    = false; // the window's desktop is a beside debt's
        bool appFullscreen = false; // ...whose app is mapped, fullscreen on its own desktop
        bool sameMonitor   = false; // ...on the debt's monitor
    };
    constexpr bool relink(const SRelinkFacts& f) {
        return f.besideDebt && f.appFullscreen && f.sameMonitor;
    }

    // PAYING A DEBT BACK. Two kinds:
    //   Trip    a fullscreen app's place: once nothing of anyone else is in it
    //           (its own leftovers go home first). A window of another app put
    //           there meanwhile makes it a desktop of its own: nothing owed.
    //   Beside  the place a window opened over a fullscreen app was given: once
    //           that desktop is empty -- that app's windows on it all gone. A
    //           window of another app put there makes it a desktop of its own,
    //           as above.
    // Either way: never while the place's desktop is on screen or a desktop
    // strip is sliding (its row is numbers). A place a fullscreen app holds now
    // is that trip's debt (it pays when it is done). A place below 1 is no
    // place: the debt is dropped, never paid at it.
    enum class EKind { Trip, Beside };
    enum class EPay {
        Refuse,      // no place: drop it, pay nothing
        NothingOwed, // a desktop of its own now: drop it
        ToTrip,      // a fullscreen app is there: its trip owes it now
        WaitScreen,  // on screen / a strip sliding: look again shortly
        WaitWindows, // windows still there (Beside): look again when one goes
        Pay,         // pay it back now
    };
    struct SFacts {
        Id   at       = NO_PLACE;
        bool exists   = false; // a desktop is in the place
        bool foreign  = false; // ...holding a window that is not the app's own
        bool any      = false; // ...holding any window
        bool trip     = false; // ...that is a fullscreen app's desktop
        bool onScreen = false; // ...shown on a monitor
        bool driving  = false; // a desktop strip is sliding
        bool homeGone = false; // the app's own windows there have no home to go to
    };
    constexpr EPay decide(EKind kind, const SFacts& f) {
        if (f.at < 1)
            return EPay::Refuse;
        if (f.exists) {
            if (f.trip)
                return EPay::ToTrip;
            if (f.foreign)
                return EPay::NothingOwed;
            // (Trip: the app's own windows there and no home to send them --
            // its home emptied meanwhile -- it is theirs now.)
            if (kind == EKind::Trip && f.any && f.homeGone)
                return EPay::NothingOwed;
            if (kind == EKind::Beside && f.any)
                return EPay::WaitWindows;
            if (f.onScreen || f.driving)
                return EPay::WaitScreen;
            return EPay::Pay;
        }
        return f.driving ? EPay::WaitScreen : EPay::Pay;
    }

    // AFTER A COMPACTION (DesktopPlan::compactMoves, at a display change). `row`
    // is (old number -> new number) of every desktop that holds windows -- the
    // ones numbered 1..N. A place, by its number before the compaction:
    //   - held a desktop of the row -> it is that desktop's new number, and what
    //     is owed there stays owed (it is still a place in the row: a payback
    //     closes it once it empties);
    //   - held nothing, or an empty desktop (sent after the row) -> NO_PLACE: the
    //     compaction closed it. A debt there is SETTLED by the compaction and
    //     dropped -- paid later, it would move every desktop after it down a
    //     second time. A trip whose own desktop has gone owes nothing more; a
    //     trip's home that has gone has no place to come back to (the app
    //     stays where it is when it leaves fullscreen).
    // And every trip whose desktop is in the row owes its place from then on
    // (FullscreenDesktop.cpp): there is no free place below the top any more,
    // so the place it holds was counted into the row, and when it is done the
    // desktops after it must come back. The same for a window opened over a
    // fullscreen app on a place that was free.
    inline Id compactedPlace(Id at, const std::vector<std::pair<Id, Id>>& row) {
        if (at >= 1)
            for (const auto& [FROM, TO] : row)
                if (FROM == at)
                    return TO;
        return NO_PLACE;
    }

    // ACROSS A PLUGIN RELOAD (hyprctl plugin unload/load, an update). The trips,
    // debts and settled windows lived in the plugin's own memory: a reload
    // forgot them, and an app on a fullscreen trip then stayed on the desktop
    // made for it for good, the room made for it never given back. They are
    // handed over through the compositor process's environment (gone with the
    // process: nothing outlives the session) by identity -- the window's,
    // desktops' and monitor's addresses, matched against the live ones at load,
    // never dereferenced -- and the next reconcile checks each against what
    // Hyprland shows, like any other.
    struct SSavedTrip {
        std::uintptr_t win = 0, home = 0, space = 0, mon = 0;
        Id             at = NO_PLACE, homeAt = NO_PLACE;
        bool           owes = false;
    };
    struct SSavedDebt {
        std::uintptr_t space = 0, mon = 0, home = 0;
        Id             at     = NO_PLACE;
        long long      pid    = 0;
        bool           beside = false;
        std::string    cls;
    };
    struct SSavedBeside {
        std::uintptr_t app = 0, appSpace = 0, space = 0, mon = 0;
        Id             at = NO_PLACE;
        bool           owes = false, wanted = false;
        long long      pid  = 0;
        std::string    cls;
    };
    struct SSaved {
        std::vector<SSavedTrip>     trips;
        std::vector<SSavedDebt>     debts;
        std::vector<std::uintptr_t> settled;
        std::vector<SSavedBeside>   besides;
    };
    // fsdesk2 adds the beside links (B); an fsdesk1 handover (the build before)
    // still loads.
    inline constexpr std::string_view SAVED_TAG    = "fsdesk2";
    inline constexpr std::string_view SAVED_TAG_V1 = "fsdesk1";

    inline std::string hexOf(std::string_view s) {
        std::string out;
        char        b[3];
        for (const unsigned char C : s) {
            std::snprintf(b, sizeof b, "%02x", C);
            out += b;
        }
        return out.empty() ? "-" : out;
    }
    inline std::optional<std::string> unhex(std::string_view s) {
        if (s == "-")
            return std::string{};
        if (s.size() % 2)
            return std::nullopt;
        std::string out;
        for (size_t i = 0; i < s.size(); i += 2) {
            unsigned v = 0;
            for (size_t j = i; j < i + 2; ++j) {
                const char C = s[j];
                v            = v * 16 + (C >= '0' && C <= '9' ? C - '0' : C >= 'a' && C <= 'f' ? C - 'a' + 10 : 99);
                if (v > 255)
                    return std::nullopt;
            }
            out += char(v);
        }
        return out;
    }

    inline std::string save(const SSaved& s) {
        std::ostringstream o;
        o << SAVED_TAG;
        for (const auto& T : s.trips)
            o << " T " << T.win << ' ' << T.home << ' ' << T.space << ' ' << T.mon << ' ' << T.at << ' ' << T.homeAt << ' ' << int(T.owes);
        for (const auto& D : s.debts)
            o << " D " << D.space << ' ' << D.mon << ' ' << D.home << ' ' << D.at << ' ' << D.pid << ' ' << int(D.beside) << ' ' << hexOf(D.cls);
        for (const auto W : s.settled)
            o << " S " << W;
        for (const auto& B : s.besides)
            o << " B " << B.app << ' ' << B.appSpace << ' ' << B.space << ' ' << B.mon << ' ' << B.at << ' ' << int(B.owes) << ' ' << int(B.wanted) << ' ' << B.pid << ' '
              << hexOf(B.cls);
        return o.str();
    }
    // Anything malformed, or another build's format: nothing (a fresh start,
    // as before).
    inline std::optional<SSaved> load(std::string_view text) {
        std::istringstream in{std::string{text}};
        std::string        tag;
        if (!(in >> tag) || (tag != SAVED_TAG && tag != SAVED_TAG_V1))
            return std::nullopt;
        const bool V2 = tag == SAVED_TAG;
        SSaved      out;
        std::string kind;
        while (in >> kind) {
            if (kind == "T") {
                SSavedTrip T;
                int        owes = 0;
                if (!(in >> T.win >> T.home >> T.space >> T.mon >> T.at >> T.homeAt >> owes))
                    return std::nullopt;
                T.owes = owes != 0;
                out.trips.push_back(T);
            } else if (kind == "D") {
                SSavedDebt  D;
                int         beside = 0;
                std::string cls;
                if (!(in >> D.space >> D.mon >> D.home >> D.at >> D.pid >> beside >> cls))
                    return std::nullopt;
                const auto CLS = unhex(cls);
                if (!CLS)
                    return std::nullopt;
                D.beside = beside != 0;
                D.cls    = *CLS;
                out.debts.push_back(D);
            } else if (kind == "S") {
                std::uintptr_t W = 0;
                if (!(in >> W))
                    return std::nullopt;
                out.settled.push_back(W);
            } else if (kind == "B" && V2) {
                SSavedBeside B;
                int          owes = 0, wanted = 0;
                std::string  cls;
                if (!(in >> B.app >> B.appSpace >> B.space >> B.mon >> B.at >> owes >> wanted >> B.pid >> cls))
                    return std::nullopt;
                const auto CLS = unhex(cls);
                if (!CLS)
                    return std::nullopt;
                B.owes   = owes != 0;
                B.wanted = wanted != 0;
                B.cls    = *CLS;
                out.besides.push_back(B);
            } else
                return std::nullopt;
        }
        return out;
    }
}
