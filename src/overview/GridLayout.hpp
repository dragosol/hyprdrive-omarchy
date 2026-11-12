#pragma once

// The mission-control GRID: membership, slot layout, and the two animations
// that carry a tile between where it is and where the grid wants it (reflow
// inbound, release outbound). OVERVIEW_CPP_SPEC section 5 B1/B2 and 13.2 F3-b.
//
// WHY IT IS ITS OWN MODULE. This was ~380 lines in the middle of
// OverviewInternal.hpp, and with it lived four mutable globals -- the reflow
// map, the release map, the "something is still moving" flag and the window a
// drag has taken out of the layout. Because that header is `inline` and
// included by every translation unit, those globals had no owner: any file
// could reach them and nothing named the thing they collectively are. They are
// one thing -- the grid's animation state -- and SGridAnimator is that thing,
// defined ONCE in GridLayout.cpp with one accessor.
//
// ONE ANIMATOR, NOT ONE PER MONITOR, and that is a decision rather than an
// oversight. Both maps are keyed by window, and a window is on exactly one
// output at a time, so per-output maps would be disjoint by construction --
// except across a move between outputs, where a per-output split would silently
// restart a reflow that is mid-flight. That is a behaviour change no gate in
// this tree can observe (the rig has one output), bought for no benefit. The
// other two members are global by nature: there is one drag, and one flag for
// "ask for another frame". If multi-output grid state ever becomes a real
// question, the seam is here: give SGridAnimator a monitor id and key the
// accessor on it.
//
// The pure arithmetic these functions used to carry inline -- the lerp, the
// open-progress ramp, the half-pixel comparison -- is in OverviewGeometry.hpp
// now and unit-tested there.

#include "Overview.hpp"
#include "GridReflowModel.hpp"

#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace OvInternal {

    // The outbound half of F3-b. A promoted tile that stops matching keeps its
    // grid membership for one reflow so it has somewhere to travel FROM.
    //
    // The clock is anchored to the FIRST FRAME THE MATCH IS SEEN TO BE GONE, not
    // to the last frame it was seen present. That distinction is the whole
    // ballgame: this runs only when something renders, and a static overview
    // renders nothing. Timestamping "last seen matching" therefore measured idle
    // time -- after a couple of idle seconds the release was already expired on
    // the frame it was born and the tile cut. Measured: it made the fix land or
    // not land at random, 1 of 2 runs at 300ms and 1 of 2 at 2000ms.
    struct SGridRelease {
        PHLWINDOWREF    window;
        Time::steady_tp start;
        bool            started = false; // armed while matching, started when the match dies
    };

    // The grid is recomputed from the live window list every frame, so a window
    // joining or leaving the focused workspace would teleport every other tile
    // straight to its new slot. Each window keeps its own reflow clock: when its
    // target slot changes it travels there over mission_control_reflow_ms.
    // OVERVIEW_CPP_SPEC B4 + user requirement U4.
    //
    // The ease itself is GridReflowModel.hpp's SEase; this adds the window it
    // belongs to, so a recycled address never inherits a dead window's ease.
    struct SGridReflow {
        PHLWINDOWREF                                window;
        Hyprdrive::GridReflow::SEase<Time::steady_tp> ease;
    };

    // The size the grid lays one window out with (GridReflowModel.hpp:
    // SSizeTrack): a change of its size animation's goal, taken by the owning
    // frame once it has moved past rounding noise and stayed moved, then the
    // animated value followed to the animation's end.
    struct SGridSize {
        PHLWINDOWREF                                     window;
        MONITORID                                        monitor = MONITOR_INVALID;
        Hyprdrive::GridReflow::SSizeTrack<Time::steady_tp> track;
    };

    // The grid's whole mutable state. See the header note for why there is one.
    struct SGridAnimator {
        std::unordered_map<Desktop::View::CWindow*, SGridReflow>  reflows;
        std::unordered_map<Desktop::View::CWindow*, SGridRelease> releases;
        std::unordered_map<Desktop::View::CWindow*, SGridSize>    sizes;

        // Which OUTPUTS are still travelling, so their overview needs another
        // frame. One bool for the whole session was wrong with two monitors up:
        // both overviews render every frame, and whichever onPreRender ran
        // first consumed and cleared the flag -- so a reflow on the external
        // monitor pumped the laptop and then stalled itself, and every hover
        // ease on either output repainted both.
        std::unordered_set<MONITORID> reflowActive;

        // The tile currently being dragged. It gives up its grid slot for the
        // duration so the survivors close the gap it left WHILE it travels --
        // F3-b-iii's "the windows already in the grid must be seen moving to
        // make room", applied to a drag instead of a search (user request,
        // 2026-08-02). Nothing else is needed: SGridReflow already eases every
        // tile whose slot changed.
        Desktop::View::CWindow* dragExcluded = nullptr;
    };

    SGridAnimator& grid();
    // The same state, read-only: what every box QUERY gets.
    const SGridAnimator& gridView();

    // WHO MAY MOVE THE GRID (GridReflowModel.hpp has the bug this answers).
    // One overview frame owns the grid's eases and its input sizes: while a
    // scope is alive for a monitor, box queries about that monitor STEP the
    // eases (advance, seed, retarget); with none alive -- a hit test, a commit
    // or damage handler, a tooltip, the drag, a debug report -- they PEEK,
    // which is const. Only COverview can open one (the constructor is
    // private), and the functions that mutate take it as an argument, so the
    // compiler holds every other caller to the read.
    class SGridOwnerScope {
      public:
        ~SGridOwnerScope();
        SGridOwnerScope(const SGridOwnerScope&)            = delete;
        SGridOwnerScope& operator=(const SGridOwnerScope&) = delete;

        // The scope owning `monitor` right now, or null.
        static const SGridOwnerScope* current(const PHLMONITOR& monitor);
        MONITORID                     monitor() const {
            return m_monitor;
        }

      private:
        friend class ::COverview;
        explicit SGridOwnerScope(const PHLMONITOR& monitor);

        MONITORID              m_monitor = MONITOR_INVALID;
        const SGridOwnerScope* m_prev    = nullptr;
    };

    // The owner's per-frame size snapshot: every window on the monitor has its
    // size looked at once, and a change the grid has not taken yet, or is
    // following, keeps the frames coming. `resizing` is the window
    // the overview's own resize handle is changing, taken without the wait.
    void gridObserveSizes(const SGridOwnerScope& owner, const PHLMONITOR& monitor, const PHLWINDOW& resizing);
    // An overview closed: its windows' sizes are taken fresh next time.
    void gridForgetSizes(MONITORID monitor);
    // A tile handed a start box by an EVENT (a cancelled drop), not a query.
    void gridReflowSeed(const PHLWINDOW& window, const PHLMONITOR& monitor, const CBox& from, const CBox& to);

    // "This output still owes a frame", and "take that debt". Free functions so
    // no caller has to remember that the flag is keyed by output now.
    void gridSetReflowActive(const PHLMONITOR& monitor);
    bool gridTakeReflowActive(const PHLMONITOR& monitor);

    // The outbound ease starts on the first releasing frame, one frame after the
    // last matching one, so the clock has to outlast the reflow or the tile is
    // cut just short of home.
    inline constexpr float GRID_RELEASE_GRACE_MS = 50.F;

    // Apple motion law, on record as user law: an exit ACCELERATES and runs
    // 20-30% faster than the matching entrance. The release is the only exit the
    // grid owns, so it is the only place this applies.
    inline constexpr float GRID_RELEASE_SPEEDUP = 0.75F;

    // How long the outbound half of F3-b runs. Both the ease and the clock that
    // decides when the release is over read THIS, never the reflow duration --
    // they disagreed once and the tile was cut a quarter of the way home.
    int gridReleaseMs();

    // --- membership -----------------------------------------------------------
    // The desktop a window LIVES ON, as the overview models it: for a stashed
    // window that is the desktop it was minimized FROM, because its own
    // m_workspace is `special:minimized` and answers no useful question. Every
    // "which card owns this window" decision has to go through here.
    WORKSPACEID            overviewGridHomeWorkspace(const PHLWINDOW& window);
    // Windows of one workspace that belong in a grid, minimized ones included at
    // the desktop they were minimized FROM.
    std::vector<PHLWINDOW> overviewGridWindows(const PHLWORKSPACE& workspace);
    // B2: with a query typed, matches from every workspace join the ACTIVE
    // workspace's grid.
    std::vector<PHLWINDOW> overviewMainGridWindows(const PHLWORKSPACE& workspace, PHLMONITOR monitor);
    bool                   overviewGridPromotable(const PHLWINDOW& window, PHLMONITOR monitor);
    bool                   overviewGridReleasing(const PHLWINDOW& window, PHLMONITOR monitor);
    bool                   overviewGridPromoted(const PHLWINDOW& window, PHLMONITOR monitor);
    Vector2D               overviewGridWindowSize(const PHLWINDOW& window);

    // --- layout ---------------------------------------------------------------
    bool overviewGridLayout(const PHLWORKSPACE& workspace, PHLMONITOR monitor, const PHLWINDOW& target, CBox& out);
    // The slot `window` owns on `monitor`, if it owns one at all. `promotedOut`
    // receives the B2 answer this function had to work out anyway: asking
    // overviewGridPromoted a second time at the call site re-armed the release
    // map for a value that cannot have changed in between.
    bool overviewGridSlot(const PHLWINDOW& window, PHLMONITOR monitor, CBox& out, bool* promotedOut = nullptr);

    // --- animation ------------------------------------------------------------
    // 0 while the overview is closed, 1 once it is fully open, so windows travel
    // between their real rect and their grid slot instead of snapping.
    float overviewGridProgress(float scale);
    // A reflow is windows arriving at new slots, i.e. an entrance: house decel.
    // A RELEASE is the one exit, and takes the house accel curve instead.
    float gridReflowEase(float t, bool exiting = false);
    bool  gridBoxesDiffer(const CBox& a, const CBox& b);
    // Called from getOverviewWindowBox, i.e. many times per window per frame from
    // render, hit-test and drag alike. Inside the owning frame
    // (SGridOwnerScope) it steps the window's ease; anywhere else it peeks,
    // so a query can never re-aim it.
    CBox  gridReflowBox(const PHLMONITOR& monitor, const PHLWINDOW& window, const CBox& slot, const CBox* enterFrom = nullptr, bool promoted = false, bool exiting = false);

} // namespace OvInternal
