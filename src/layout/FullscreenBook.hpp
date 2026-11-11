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

#include <string_view>

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
    template <class FREE>
    SStart start(std::string_view mode, Id home, int othersOnHome, FREE&& isFree) {
        if (mode == "stay" || othersOnHome == 0 || home < 1)
            return {.settle = true};
        Id at = home + 1;
        if (mode == "free") {
            while (!isFree(at) && at < home + 1000)
                ++at;
            return {.at = at};
        }
        return {.at = at, .makeRoom = !isFree(at)};
    }

    // A WINDOW OPENED OVER A FULLSCREEN APP on desktop `appDesk`: it gets the
    // next place, made if it is not free -- and then owed back, like a trip's.
    template <class FREE>
    SStart beside(Id appDesk, FREE&& isFree) {
        if (appDesk < 1)
            return {.settle = true};
        return {.at = appDesk + 1, .makeRoom = !isFree(appDesk + 1)};
    }

    // PAYING A DEBT BACK. Two kinds:
    //   Trip    a fullscreen app's place: once nothing of anyone else is in it
    //           (its own leftovers go home first). A window of another app put
    //           there meanwhile makes it a desktop of its own: nothing owed.
    //   Beside  the place a window opened over a fullscreen app was given: once
    //           that desktop is empty -- every window on it gone.
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
    };
    constexpr EPay decide(EKind kind, const SFacts& f) {
        if (f.at < 1)
            return EPay::Refuse;
        if (f.exists) {
            if (f.trip)
                return EPay::ToTrip;
            if (kind == EKind::Trip && f.foreign)
                return EPay::NothingOwed;
            if (kind == EKind::Beside && f.any)
                return EPay::WaitWindows;
            if (f.onScreen || f.driving)
                return EPay::WaitScreen;
            return EPay::Pay;
        }
        return f.driving ? EPay::WaitScreen : EPay::Pay;
    }
}
