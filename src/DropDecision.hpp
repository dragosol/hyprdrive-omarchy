#pragma once

// Where a released window drag goes, as a pure function.
//
// endWindowDrag() was 277 lines in which the facts about the drop, the choice
// that followed from them and the compositor calls that carried it out were the
// same code. The ladder below is that choice and nothing else: no Hyprland
// types, no state, no side effects, so it can be walked exhaustively off the
// compositor -- scripts/drop_table_test.cpp compiles this header on the host and
// checks every reachable combination against the branch structure it replaced.
//
// The caller gathers facts in three stages and asks again after each, because
// the gathering itself is not free: the pill branch must return before anything
// queries a drop workspace. resolve() is total at every stage --
// the fields a stage has not filled in yet are false, which is why the two
// early actions are decided by the facts that ARE known first.

namespace ScrollOverview::Drop {

    struct SFacts {
        // stage 1 -- the window and where the button came up
        bool haveWindow        = false;
        bool overPill          = false;
        // stage 2 -- the destination
        bool haveDropWorkspace = false;
        bool retileOnEnd       = false; // startedTiled AND a live layout target/space/algorithm
        bool moveWorkspace     = false; // the drop workspace is not the one the drag started on
        bool startedTiled      = false;
        bool haveTarget        = false;
        bool scrollingLayout   = false;
        bool haveOriginalBox   = false;
        // stage 3 -- where on the destination card
        bool dropPointMapsIntoCard        = false;
        bool restoreSourceFullscreenFocus = false;
    };

    enum class EAction {
        Minimize,     // released on the dock pill: the drop is a minimize and nothing else
        CancelToGrid, // nowhere to go, or its own desktop: keep everything, ease the tile home
        RetileMove,   // a tiled window, onto another workspace
        FloatMove,    // a floating window, onto another workspace
        Nothing,      // a drop with nothing to move (no window, or no layout target)
    };

    // What to do with the layout target once the window is where it belongs.
    enum class EPlacement {
        None,
        ScrollEdgeFar, // push to the far edge of a scrolling workspace
    };

    struct SDecision {
        EAction    action    = EAction::Nothing;
        EPlacement placement = EPlacement::None;
        // A move commits the window's ORIGINAL global size, not the size of the
        // mission-control slot it was drawn at (X41/X42).
        bool useOriginalBox   = false;
        // The cursor can be read as a point inside the destination card, so a
        // floating window lands where it was dropped rather than where it was.
        bool mapPointIntoCard = false;
        bool restoreSourceFullscreenFocus = false;
    };

    // The overview MOVES windows between desktops; it never arranges them. A
    // drop on the window's own desktop is no drop at all (the tile eases back to
    // its slot), and a window arriving on another desktop goes where that
    // desktop's layout puts a new window -- never beside / swapped with the tile
    // under the cursor. (Until 1.9.2 a same-desktop drop swapped or inserted by
    // direction and a drop indicator highlighted the target; the user: "that's
    // not the purpose of the overview at any point ever".)
    constexpr SDecision resolve(const SFacts& f) {
        SDecision d;

        if (f.haveWindow && f.overPill) {
            d.action = EAction::Minimize;
            return d;
        }

        if (!f.haveDropWorkspace || !f.moveWorkspace) {
            d.action = EAction::CancelToGrid;
            return d;
        }

        d.restoreSourceFullscreenFocus = f.restoreSourceFullscreenFocus;
        d.useOriginalBox               = f.haveOriginalBox;
        d.mapPointIntoCard             = f.dropPointMapsIntoCard;

        if (f.retileOnEnd) {
            d.action = EAction::RetileMove;
            if (f.scrollingLayout)
                d.placement = EPlacement::ScrollEdgeFar;
            return d;
        }

        if (f.haveWindow)
            d.action = EAction::FloatMove;

        return d;
    }

    // ---------------------------------------------------------------- V3 --
    // WHERE A RELEASED DOCK-ICON DRAG GOES, on the same terms: facts in, one
    // choice out, no compositor. The window drop above and this one are two
    // gestures that end on the same screen and had every reason to grow two
    // unrelated if-ladders inside two input hooks; they are one file so a
    // reader can see both tables at once, and scripts/drop_table_test.cpp walks
    // this one exhaustively beside it.
    //
    // The ORDER is the whole of the design and it is not free:
    //   * a release near its own press is a CLICK whatever the pointer did in
    //     between -- X16's lesson, arriving here for the same reason it arrived
    //     at the pill (a real hand wobbles 10-20px, and a wobble that ate the
    //     click read as a dead dock);
    //   * the pill answers before any desktop does, exactly as it does for a
    //     window drop, because it floats over whatever is beneath it;
    //   * with the overview UP the desktops ARE the strip cards. The main area
    //     is a grid of windows, not a desktop, so a drop there names nothing
    //     and cancels -- which is also what gives the gesture a near miss to
    //     have (X26 P2's 28px above the pill, in the other direction).
    //
    // ASKED IN TWO STAGES, like resolve() above and for the same reason: the
    // gathering is not free, and every dock CLICK in the session comes through
    // here. The first four fields are what the dock already knows, and because
    // Click and Reorder sit ABOVE every overview question in the ladder, those
    // two answers are FINAL from stage one alone -- whatever the overview is
    // doing cannot change them. Anything else means the caller has to go and
    // look at the strip and ask again.
    struct SIconFacts {
        bool haveItem          = false; // the press landed on an icon with a desktop id
        bool live              = false; // the pointer passed the drag threshold
        bool nearStart         = false; // ...and came back: released within the click band
        bool onPill            = false; // released inside the dock pill
        bool overviewOpen      = false;
        bool onStripCard       = false; // released on a workspace card in the strip
        bool stripCardIsActive = false; // ...and that card is the desktop being viewed
    };

    enum class EIconAction {
        Click,       // not a drag at all: the caller's click ladder owns this release
        Reorder,     // released on the pill: X32's favourite move, unchanged
        LaunchHere,  // launch on the ACTIVE desktop, focused -- it is where the user is
        LaunchThere, // launch on the named desktop, SILENT: nothing moves under them
        Cancel,      // missed everything: the icon flies home
    };

    constexpr EIconAction resolveIcon(const SIconFacts& f) {
        if (!f.haveItem || !f.live || f.nearStart)
            return EIconAction::Click;

        if (f.onPill)
            return EIconAction::Reorder;

        if (!f.overviewOpen)
            return EIconAction::LaunchHere;

        if (f.onStripCard)
            return f.stripCardIsActive ? EIconAction::LaunchHere : EIconAction::LaunchThere;

        return EIconAction::Cancel;
    }

}
