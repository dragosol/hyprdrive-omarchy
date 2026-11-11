// Host test for StripCleanupModel.hpp, the overview strip's Clean Up button:
// when it shows (Super, the pointer, a removable empty desktop), where it goes
// (centred on the pointer, clamped to the strip), how it follows the pointer,
// how far the windows under it are bumped (the label's rule: only when
// something is in the way), and -- through the planner it calls,
// DesktopPlan::compactMoves -- which desktops a click removes.
#include "StripCleanupModel.hpp"
#include "DesktopPlan.hpp"

#include <cmath>
#include <cstdio>
#include <limits>
#include <map>

using namespace Hyprdrive;
namespace SC = Hyprdrive::StripCleanup;

static int g_checks = 0, g_failed = 0;
#define CHECK(c)                                                                                                                                                                   \
    do {                                                                                                                                                                           \
        ++g_checks;                                                                                                                                                                \
        if (!(c)) {                                                                                                                                                                \
            ++g_failed;                                                                                                                                                            \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);                                                                                                               \
        }                                                                                                                                                                          \
    } while (0)

static bool near(double a, double b, double eps = 1e-6) {
    return std::abs(a - b) <= eps;
}

// The strip as the overview has it: ids on a row, with/without windows, the shown one.
struct SCell {
    long long id;
    bool      populated, shown;
};
static std::vector<SC::SDesk> desks(const Row& row, const std::vector<SCell>& cells) {
    std::vector<SC::SDesk> out;
    for (const auto& C : cells)
        out.push_back({row.slot(C.id), C.populated, C.shown});
    return out;
}

// A click, through the planner: id -> new id for every cell.
static std::map<long long, long long> click(const Row& row, const std::vector<SCell>& cells) {
    std::vector<DesktopPlan::Room> rooms;
    DesktopPlan::Key               k = 1;
    for (const auto& C : cells)
        rooms.push_back({.key = k++, .id = C.id, .populated = C.populated, .shown = C.shown});
    std::map<long long, long long> to;
    for (const auto& C : cells)
        to[C.id] = C.id;
    for (const auto& M : DesktopPlan::compactMoves(rooms, row))
        to[cells[M.key - 1].id] = M.to;
    return to;
}

static void removableEmpties() {
    const Row R{};
    // 1,2,5,6 populated; 3,4 the overview's gap-fill; 7 its trailing spare.
    const std::vector<SCell> STRIP = {{1, true, true}, {2, true, false}, {3, false, false}, {4, false, false}, {5, true, false}, {6, true, false}, {7, false, false}};
    CHECK(SC::removable(desks(R, STRIP)) == 2); // 3 and 4; never the spare
    // Already compact: only the spare after them -- nothing to clean.
    CHECK(SC::removable(desks(R, {{1, true, true}, {2, true, false}, {3, false, false}})) == 0);
    // The shown desktop is empty and between: it is where the user is, not removable.
    CHECK(SC::removable(desks(R, {{1, true, false}, {2, false, true}, {3, true, false}, {4, false, false}})) == 0);
    // ...but an empty one beside it is.
    CHECK(SC::removable(desks(R, {{1, true, false}, {2, false, true}, {3, false, false}, {4, true, false}})) == 1);
    // Nothing populated: nothing between.
    CHECK(SC::removable(desks(R, {{1, false, true}, {2, false, false}})) == 0);
    // Visitors (outside an Omarchy block) never count, either way.
    const Row B = Row::block(1, 10); // 11..20, overflow 1'001'001...
    CHECK(SC::removable(desks(B, {{11, true, true}, {12, false, false}, {13, true, false}, {5, false, false}, {40, true, false}})) == 1);
    // Overflow desktops are row members past the block: an empty slot in the block before one is a hole.
    CHECK(SC::removable(desks(B, {{11, true, true}, {12, false, false}, {B.idOf(11), true, false}})) == 1);
}

static void whenItShows() {
    SC::SWhen w{.enabled = true, .super = true, .onStrip = true, .removable = 2};
    CHECK(SC::wanted(w));
    auto t = w;
    t.super = false; // "hold super"
    CHECK(!SC::wanted(t));
    t         = w;
    t.enabled = false; // the setting (the gate's failing control)
    CHECK(!SC::wanted(t));
    t           = w;
    t.removable = 0; // nothing to clean
    CHECK(!SC::wanted(t));
    t      = w;
    t.busy = true; // a drag, a switch
    CHECK(!SC::wanted(t));
    t         = w;
    t.onStrip = false; // off the strip and the button, no grace: gone
    CHECK(!SC::wanted(t));
    t.onButton = true; // down onto the button: stays
    CHECK(SC::wanted(t));
    t.onButton = false;
    t.grace    = true; // crossing the gap
    CHECK(SC::wanted(t));
    CHECK(SC::inGrace(0.0) && SC::inGrace(SC::GRACE_MS - 1.0));
    CHECK(!SC::inGrace(SC::GRACE_MS) && !SC::inGrace(-1.0) && !SC::inGrace(std::numeric_limits<double>::quiet_NaN()));
}

static void placement() {
    // Centred on the pointer.
    CHECK(near(SC::placeX(500, 200, 100, 1000), 400));
    // Clamped at either end of the strip.
    CHECK(near(SC::placeX(120, 200, 100, 1000), 100));
    CHECK(near(SC::placeX(990, 200, 100, 1000), 800));
    // A strip narrower than the button: centred on it.
    CHECK(near(SC::placeX(150, 200, 100, 250), 75));
    // Not finite: the strip's start.
    CHECK(near(SC::placeX(std::numeric_limits<double>::quiet_NaN(), 200, 100, 1000), 100));
    // Following: a step closes part of the way, never overshoots, lands, holds at dt 0.
    const double X1 = SC::follow(0, 100, 0.016);
    CHECK(X1 > 0 && X1 < 100);
    CHECK(near(SC::follow(0, 100, 0.0), 0));
    CHECK(near(SC::follow(0, 100, 10.0), 100)); // a snapping clock
    double x = 0;
    for (int i = 0; i < 200; ++i)
        x = SC::follow(x, 100, 0.016);
    CHECK(near(x, 100));
    CHECK(near(SC::follow(std::numeric_limits<double>::infinity(), 100, 0.016), 100));
    // Smooth: no step jumps more than the gap it closes (not jittery).
    double prev = 0, y = 0;
    for (int i = 0; i < 30; ++i) {
        y = SC::follow(y, 100, 0.016);
        CHECK(y >= prev);
        prev = y;
    }
}

static void bump() {
    const SC::SBox BTN{400, 210, 200, 30}; // bottom 240
    const double   GAP = 8, FLOOR = 200;     // the strip plate's foot at 200
    // Nothing under it: nothing moves.
    CHECK(near(SC::bumpFor(BTN, GAP, {}, FLOOR), 0));
    // A window under it, starting at 230: down to 248 -> 18.
    CHECK(near(SC::bumpFor(BTN, GAP, {{300, 230, 400, 300}}, FLOOR), 18));
    // Already clear (starts at 260 >= 248): 0.
    CHECK(near(SC::bumpFor(BTN, GAP, {{300, 260, 400, 300}}, FLOOR), 0));
    // Beside it, past the gap: not in the way.
    CHECK(near(SC::bumpFor(BTN, GAP, {{620, 220, 100, 100}}, FLOOR), 0));
    // ...but within the gap it is.
    CHECK(near(SC::bumpFor(BTN, GAP, {{605, 220, 100, 100}}, FLOOR), 28));
    // The worst one decides.
    CHECK(near(SC::bumpFor(BTN, GAP, {{300, 230, 100, 100}, {450, 215, 50, 50}}, FLOOR), 33));
    // Things above the plate's foot (the strip itself, its cells) never count.
    CHECK(near(SC::bumpFor(BTN, GAP, {{300, 100, 400, 90}}, FLOOR), 0));
    // Bad boxes are nothing.
    CHECK(near(SC::bumpFor(BTN, GAP, {{300, 230, 1, 300}}, FLOOR), 0));
    CHECK(near(SC::bumpFor({400, 210, std::numeric_limits<double>::quiet_NaN(), 30}, GAP, {{300, 230, 400, 300}}, FLOOR), 0));
    CHECK(!SC::drawable({0, 0, 1.5, 10}) && SC::drawable({0, 0, 2, 2}));
}

static void clickThroughThePlanner() {
    // Native single row: 1,2,5,6 populated, 3,4 gap-fill, 7 spare -> 1..4, shown kept.
    {
        const Row R{};
        const auto TO = click(R, {{1, true, true}, {2, true, false}, {3, false, false}, {4, false, false}, {5, true, false}, {6, true, false}, {7, false, false}});
        CHECK(TO.at(1) == 1 && TO.at(2) == 2 && TO.at(5) == 3 && TO.at(6) == 4);
        // The empties after them (Hyprland reaps the ones nothing holds).
        CHECK(TO.at(3) == 5 && TO.at(4) == 6 && TO.at(7) == 7);
    }
    // The shown desktop empty, between: it stays (shown), after the populated ones.
    {
        const Row R{};
        const auto TO = click(R, {{1, true, false}, {2, false, false}, {3, false, true}, {4, true, false}});
        CHECK(TO.at(1) == 1 && TO.at(4) == 2 && TO.at(3) == 3 && TO.at(2) == 4);
    }
    // An Omarchy block with an overflow desktop: it flows back into the block, and
    // the result is contiguous from the block's first id.
    {
        const Row  B  = Row::block(1, 10);
        const auto OV = B.idOf(11);
        const auto TO = click(B, {{11, true, true}, {13, true, false}, {12, false, false}, {OV, true, false}});
        CHECK(TO.at(11) == 11 && TO.at(13) == 12 && TO.at(OV) == 13 && TO.at(12) == 14);
        // Every populated one inside the block: a fixed point of Omarchy's reconcile.
        CHECK(B.inBlock(TO.at(11)) && B.inBlock(TO.at(13)) && B.inBlock(TO.at(OV)));
        // A second click has nothing to do.
        const auto AGAIN = click(B, {{11, true, true}, {12, true, false}, {13, true, false}, {14, false, false}});
        CHECK(AGAIN.at(11) == 11 && AGAIN.at(12) == 12 && AGAIN.at(13) == 13 && AGAIN.at(14) == 14);
        CHECK(SC::removable(desks(B, {{11, true, true}, {12, true, false}, {13, true, false}, {14, false, false}})) == 0);
    }
}

int main() {
    removableEmpties();
    whenItShows();
    placement();
    bump();
    clickThroughThePlanner();
    std::printf("strip_cleanup_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
