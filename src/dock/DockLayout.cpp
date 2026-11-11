// Geometry: autohide reveal, the arrangement animation, the base layout
// and layout().
#include "DockParts.hpp"
#include "MotionSites.hpp"
#include "MotionClock.hpp"

namespace Hyprdrive::Dock {
using namespace DockDetail;
namespace DockDetail {

// ----------------------------------------------------------------------------

// X75 (user, 2026-08-06: "behave exactly like the autohide behavior in the
// dash to dock gnome extension"). dash-to-dock eases the slider with
// Clutter's EASE_OUT_QUAD in BOTH directions -- `_animateIn` and `_animateOut`
// pass the same mode -- so the hide does NOT accelerate away the way this
// rice's house exit curve does. Matching the reference means dropping the
// house asymmetry here; that is the instruction, not an oversight. The curve is
// Motion::ESite::DOCK_REVEAL (MotionSites.hpp); direction is not a parameter,
// because the curve does not depend on it.

// 0 = fully out, 1 = fully tucked below the bottom edge.
float hiddenFraction(PHLMONITOR monitor) {
    if (!monitor)
        return 0.F;

    // Overview owns the slide while it exists: the 3-finger swipe warps
    // openProgress every update, so the pill tracks the fingers the same way
    // the cards do. Super-tap uses the scale animation, same number.
    if (const auto OV = scrollOverviewForMonitor(monitor); OV && Config::getValue<bool>(KEY_AUTOHIDE))
        return 1.F - OV->openProgress();

    const auto& R  = g_reveal[monitor->m_id];
    const int   MS = std::max(1, Config::getValue<int>(KEY_AUTOHIDE_MS));
    const float E  = Motion::curve(Motion::ESite::DOCK_REVEAL, std::clamp(Motion::Clock::animMs(R.changedAt) / sc<float>(MS), 0.F, 1.F));

    // From wherever it WAS to wherever it is going, which is what makes a
    // reversal continuous instead of a jump to the far end.
    return R.from + ((R.shown ? 0.F : 1.F) - R.from) * E;
}

// How far down the pill currently sits, mid-slide included. Shared with
// layout() below so the HIT TEST and the RENDER can never disagree about where
// the pill is -- which is the whole bug X77 fixes.
double slideOffsetPx(PHLMONITOR monitor, double pillHeight) {
    if (!monitor)
        return 0.0;

    return std::round((pillHeight + EDGE_MARGIN * monitor->m_scale) * hiddenFraction(monitor));
}

// The pill WHERE IT IS, not where it rests. Empty once it is fully away.
CBox slidPill(PHLMONITOR monitor) {
    auto pill = baseLayout(monitor).pill;
    if (pill.empty() || hiddenFraction(monitor) >= 1.F)
        return CBox{};

    pill.y += slideOffsetPx(monitor, pill.height);
    return pill;
}

bool autohideAnimating(PHLMONITOR monitor) {
    if (!monitor)
        return false;

    const auto& R = g_reveal[monitor->m_id];

    // AN IDLE OVERVIEW IS NOT AN ANIMATION. While an overview is open over this
    // monitor the pill's position is openProgress, not this timer -- and
    // updateAutohide stamps changedAt on EVERY call to keep the close hand-off
    // continuous. The elapsed test below therefore read "still sliding" forever,
    // so an overview left sitting open pumped a frame per monitor per frame,
    // for as long as it was open (user, 2026-09-14). The overview's own scale
    // animation asks for the frames openProgress actually needs while it moves;
    // a flight still asks for its own.
    if (const auto OV = scrollOverviewForMonitor(monitor); OV && Config::getValue<bool>(KEY_AUTOHIDE))
        return R.pending || Flight::anyOn(monitor);

    // X54. A flight counts as animating: the window is mid-air toward the icon
    // and nothing else on an idle desktop would ask for the frames it needs.
    return R.pending || Flight::anyOn(monitor) || Motion::Clock::animMs(R.changedAt) < sc<float>(std::max(1, Config::getValue<int>(KEY_AUTOHIDE_MS)));
}

// The glide is Motion::ESite::DOCK_ARRANGE (ease-out cubic) and a new icon
// Motion::ESite::DOCK_SPAWN (ease-out back): MotionSites.hpp.

CBox scaledAboutCentre(const CBox& box, float s) {
    const double W = box.width * s, H = box.height * s;
    return CBox{box.x + (box.width - W) / 2.0, box.y + (box.height - H) / 2.0, W, H};
}

// The resting layout as it is to be DISPLAYED right now. Pure apart from
// retiring an arrangement that has finished.
SLayout arranged(MONITORID id, const SLayout& resting) {
    const auto IT = g_arrange.find(id);
    if (IT == g_arrange.end() || !IT->second.active)
        return resting;

    auto&       A = IT->second;
    // On the animation clock, so plugin:hyprdrive:anim_scale (the panel's
    // speed) stretches it too; 0 finishes it at once.
    const float T = Hyprdrive::Config::animElapsedMs(A.since) / ARRANGE_MS;
    if (T >= 1.F) {
        A.active = false;
        A.fromBoxes.clear();
        return resting;
    }

    const float E   = Motion::curve(Motion::ESite::DOCK_ARRANGE, T);
    SLayout     out = resting;
    out.pill        = Motion::lerp(A.fromPill, resting.pill, E);
    if (!A.fromShowApps.empty() && !resting.showApps.empty())
        out.showApps = Motion::lerp(A.fromShowApps, resting.showApps, E);

    for (auto& item : out.items) {
        if (const auto FROM = A.fromBoxes.find(item.subject); FROM != A.fromBoxes.end()) {
            item.iconBox = Motion::lerp(FROM->second, item.iconBox, E);
            continue;
        }
        const float S = std::max(0.F, Motion::curve(Motion::ESite::DOCK_SPAWN, T));
        item.iconBox  = scaledAboutCentre(item.iconBox, S);
        item.appear   = std::clamp(T * 2.F, 0.F, 1.F); // opaque by half-way
    }

    return out;
}

// Called by baseLayout() with the layout that is ABOUT TO BE REPLACED, before
// it is: what is on screen is computed from the old resting layout.
void beginArrangement(MONITORID id, const SLayout& oldResting, const SLayout& newResting) {
    if (oldResting.pill.empty() || newResting.pill.empty())
        return; // from or to nothing is the dock appearing, which has its own slide

    const SLayout SHOWN = arranged(id, oldResting);

    // Most item-list changes move nothing: a window changing desktop bumps the
    // generation with the same icons in the same slots. Asking for 320ms of
    // frames to lerp a box onto itself is waste, so only a real difference
    // starts one.
    const auto SAMEBOX = [](const CBox& a, const CBox& b) {
        return std::abs(a.x - b.x) < 0.5 && std::abs(a.y - b.y) < 0.5 && std::abs(a.width - b.width) < 0.5 && std::abs(a.height - b.height) < 0.5;
    };
    if (SAMEBOX(SHOWN.pill, newResting.pill) && SHOWN.items.size() == newResting.items.size() &&
        std::ranges::equal(SHOWN.items, newResting.items, [&](const SItem& a, const SItem& b) { return a.subject == b.subject && SAMEBOX(a.iconBox, b.iconBox); }))
        return;

    auto& A        = g_arrange[id];
    A.active       = true;
    A.since        = Time::steadyNow();
    A.fromPill     = SHOWN.pill;
    A.fromShowApps = SHOWN.showApps;
    A.fromBoxes.clear();

    std::unordered_set<std::string> staying;
    for (const auto& ITEM : newResting.items)
        staying.insert(ITEM.subject);

    for (const auto& ITEM : SHOWN.items) {
        if (ITEM.subject.empty())
            continue;
        if (staying.contains(ITEM.subject))
            A.fromBoxes[ITEM.subject] = ITEM.iconBox;
        else if (ITEM.appear > 0.05F && !ITEM.icon.empty())
            A.exits.push_back({ITEM, Time::steadyNow()});
    }
}

bool arrangeAnimating(MONITORID id) {
    const auto IT = g_arrange.find(id);
    if (IT == g_arrange.end())
        return false;
    auto& A = IT->second;
    std::erase_if(A.exits, [](const SArrangeExit& X) { return Hyprdrive::Config::animElapsedMs(X.since) >= EXIT_MS; });
    return (A.active && Hyprdrive::Config::animElapsedMs(A.since) < ARRANGE_MS) || !A.exits.empty();
}

SLayout buildBaseLayout(PHLMONITOR monitor) {
    SLayout out;

    if (!monitor || !onMonitor(monitor) || monitor->m_size.x < 1 || monitor->m_size.y < 1)
        return out;

    auto items = dockItems();

    // X46. A minimized window used to be appended to the pill as a preview slot
    // of its own. It is not any more: the pill's cells, centres and width must
    // not move when something is minimized, because the user asked for the
    // window to go INTO its icon rather than to sprout a second entry. Live
    // previews still exist -- X47 raises them on HOVER, out of its own layout.
    //
    // The slot's machinery outlived the decision by an empty vector: baseLayout
    // built `previews` from a list that was `const std::vector<PHLWINDOW>` and
    // never assigned, so every reader of SLayout::previews -- the slide offset,
    // the click hit-test, the render, the frame-pump predicate -- was walking a
    // container that could not have an element in it. Deleted, with the struct.

    // X32. The button is a slot of its own, so it widens the pill and competes
    // for icon size with everything else -- and it is the ONE thing that can put
    // a pill on screen by itself, on a desktop with nothing running and nothing
    // pinned. That is the reference's behaviour: `show-show-apps-button` does
    // not go away when the dash is empty.
    const bool SHOWAPPS = Config::getValue<bool>(KEY_SHOW_APPS);
    if (items.empty() && !SHOWAPPS)
        return out;

    const float SLOTS    = sc<float>(items.size()) + (SHOWAPPS ? 1.F : 0.F);
    const float SCALE    = std::max(monitor->m_scale, 0.01F);
    const float ICONMAX  = sc<float>(std::clamp(Config::getValue<int>(KEY_ICON_SIZE), 8, 512));
    const float FRACTION = std::clamp(Config::getValue<float>(KEY_FRACTION), 0.05F, 1.F);
    const auto  M        = metrics(items.size() + (SHOWAPPS ? 1 : 0), SLOTS, ICONMAX, FRACTION * sc<float>(monitor->m_size.x));

    // `extend-height` false and `always-center-icons` false: a floating pill
    // sized to its contents, centred on the monitor, clear of the bottom edge.
    const float XL = (sc<float>(monitor->m_size.x) - M.width) / 2.F;
    const float YL = sc<float>(monitor->m_size.y) - EDGE_MARGIN - M.height;

    out.pill = CBox{XL * SCALE, YL * SCALE, M.width * SCALE, M.height * SCALE};
    out.pill.round();
    out.rounding = std::max(0, sc<int>(std::round(sc<float>(Hyprdrive::Config::dockCornerRadius()) * SCALE)));

    float cursor = XL + CONTAINER_PAD_X + ITEM_MARGIN_X;
    out.items = std::move(items);
    for (auto& item : out.items) {
        item.iconBox = CBox{cursor * SCALE, (YL + ITEM_PAD_TOP) * SCALE, M.icon * SCALE, M.icon * SCALE};
        item.iconBox.round();

        cursor += M.icon + 2.F * ITEM_MARGIN_X;
    }

    // X32 Show Applications. `show-apps-at-top` FALSE puts it at the far end of
    // a bottom dock, and `show-apps-always-in-the-edge` TRUE is what keeps it
    // there rather than letting it float in with the apps -- so it lands after
    // the previews, which is the last slot the cursor has reached.
    //
    // `at_top` true is honoured by laying it out FIRST instead. The pair is the
    // reference's, and one without the other would be half a setting.
    if (SHOWAPPS) {
        const bool  FIRST = Config::getValue<bool>(KEY_SHOW_APPS_TOP) || !Config::getValue<bool>(KEY_SHOW_APPS_EDGE);
        const float X     = FIRST ? XL + CONTAINER_PAD_X + ITEM_MARGIN_X : cursor;

        out.showApps = CBox{X * SCALE, (YL + ITEM_PAD_TOP) * SCALE, M.icon * SCALE, M.icon * SCALE};
        out.showApps.round();

        // Laid out first: everything else slides one slot right, rather than
        // being overdrawn by it.
        if (FIRST) {
            const double SHIFT = (M.icon + 2.F * ITEM_MARGIN_X) * SCALE;
            for (auto& item : out.items)
                item.iconBox.x += SHIFT;
        }
    }

    return out;
}

const SLayout& baseLayout(PHLMONITOR monitor) {
    // An output that is not there has no pill, and it must not get a cache row
    // either -- MONITOR_INVALID is a key a real output could later be handed.
    static const SLayout EMPTY;
    if (!monitor)
        return EMPTY;

    // BEFORE the key is built, not inside buildBaseLayout: the key consults
    // dockItems(), which scans the app inventory and walks every window. A dock
    // that is switched off must not pay for an item model nobody will draw --
    // and damageIfChanged still runs on every pointer motion (user,
    // 2026-09-14).
    if (!onMonitor(monitor) || monitor->m_size.x < 1 || monitor->m_size.y < 1)
        return EMPTY;

    SBaseLayoutKey key;
    key.items    = itemsGeneration();
    key.width    = monitor->m_size.x;
    key.height   = monitor->m_size.y;
    key.scale    = monitor->m_scale;
    key.iconSize = Config::getValue<int>(KEY_ICON_SIZE);
    key.radius   = Hyprdrive::Config::dockCornerRadius();
    key.fraction = Config::getValue<float>(KEY_FRACTION);
    key.flags    = (Config::getValue<bool>(KEY_ENABLED) ? 1U : 0U) |          //
        (Config::getValue<bool>(KEY_SHOW_APPS) ? 2U : 0U) |                   //
        (Config::getValue<bool>(KEY_SHOW_APPS_TOP) ? 4U : 0U) |               //
        (Config::getValue<bool>(KEY_SHOW_APPS_EDGE) ? 8U : 0U);

    // std::unordered_map, so the reference this hands back survives a later
    // call for a DIFFERENT monitor inserting its own row.
    auto& SLOT = g_baseLayouts[monitor->m_id];
    if (!SLOT.filled || !(SLOT.key == key)) {
        auto       fresh    = buildBaseLayout(monitor);
        // Only the ITEMS changed: that is a new arrangement, and it glides.
        // Anything else is a new geometry and snaps (see the arrangement note).
        auto       sameGeom = SLOT.key;
        sameGeom.items      = key.items;
        if (SLOT.filled && sameGeom == key)
            beginArrangement(monitor->m_id, SLOT.layout, fresh);
        else if (const auto IT = g_arrange.find(monitor->m_id); IT != g_arrange.end())
            IT->second = SArrange{};

        SLOT.key    = key;
        SLOT.filled = true;
        SLOT.layout = std::move(fresh);
    }

    return SLOT.layout;
}

void updateAutohide(PHLMONITOR monitor, const STally& tally) {
    if (!monitor)
        return;

    auto& R = g_reveal[monitor->m_id];

    // Pinned: autohide switched off, or the overview is open over this monitor.
    // Neither waits out `show-delay` -- that delay belongs to the pressure
    // barrier, and making the overview's own dock crawl in behind it would be
    // reading the reference wrong.
    // While the overview exists the visual is driven by openProgress, not this
    // timer. Keep `from` at the current fraction so a later handoff (overview
    // gone) continues from wherever the swipe or scale animation left the pill.
    if (const auto OV = scrollOverviewForMonitor(monitor); OV && Config::getValue<bool>(KEY_AUTOHIDE)) {
        R.from = 1.F - OV->openProgress();
        // Visual is driven by openProgress while the overview exists. Pinning
        // shown=true here made a gesture close hand off as "hidden, but going
        // visible", so the pill bounced up once then autohide hid it again.
        R.shown     = false;
        R.changedAt = Time::steadyNow();
        R.pending   = false;
        R.pressure  = 0.F;
        return;
    }

    if (!Config::getValue<bool>(KEY_AUTOHIDE)) {
        if (!R.shown) {
            R.from      = hiddenFraction(monitor);
            R.shown     = true;
            R.changedAt = Time::steadyNow();
        }
        R.pending  = false;
        R.pressure = 0.F;
        return;
    }

    // X31 `show-dock-urgent-notify` true: an app demanding attention brings the
    // dock out on its own. It is ORed into BOTH branches below, not just the
    // hidden one -- an urgent app also has to stop the pointer leaving from
    // sliding the dock away again, or the wiggle would play to nobody.
    //
    // ...WITHDRAWN (user, 2026-09-24): an urgent app no longer pins the dock
    // out. It hops from the edge instead (renderUrgentHop), and the dock stays
    // wherever autohide has it.

    bool       want      = false;
    // A reversal that catches the pill still on screen does not wait out a
    // delay; see the X77 note below.
    bool       immediate = false;

    const auto BOX   = monitor->logicalBox();
    const auto POINT = g_pInputManager->getMouseCoordsInternal();
    const bool ATEDGE = BOX.containsPoint(POINT) && POINT.y >= BOX.y + BOX.h - REVEAL_BAND;

    if (R.shown) {
        // Up already: it stays up while the pointer is on it, or while its
        // right-click menu is -- the menu floats ABOVE the pill, so reaching for
        // a row takes the pointer off the pill and would otherwise slide the
        // dock out from under the menu the user is aiming at.
        // Y1. The hover preview panel holds the dock for the same reason the
        // menu does, and more sharply: the panel floats a gap ABOVE the pill, so
        // reaching into it necessarily takes the pointer off the pill. Without
        // this the dock slides out from under the previews the user is reaching
        // for, which is the second half of the reported bug.
        // V3. And an icon being dragged holds it too, for the third time and
        // the sharpest reason: pulling one OFF the pill takes the pointer off
        // the pill by definition, so without this the dock slides away under a
        // gesture that started on it -- and render() returns early on an empty
        // pill, so the icon under the pointer would go with it. The return
        // flight holds it for the other half: an icon cannot land in a cell
        // that has left the screen.
        //
        // Position at the bottom edge of the monitor holds the dock as well:
        // the user can trigger the reveal anywhere along the bottom edge, and
        // holding the cursor there outside the physical dock pill must keep it
        // visible rather than oscillating (hide/show loop).
        // And a FILE pulled out of a stack's fan (DockDrop): the fan folds as
        // the drag starts, and the Trash or another stack may be where it is
        // headed (user, 2026-09-24) -- the dock stays until the drag ends.
        if (menuOpen() || panelHoldsDock(monitor) || Stacks::holdsDock(monitor) || dragging() || ghostFlying() || DockDrop::draggingOut() || ATEDGE)
            want = true;
        else if (CBox reach = slidPill(monitor); !reach.empty()) {
            // X77: the SLID pill, not the resting one. While it is still easing
            // out it sits BELOW its resting box, so a pointer on the part that
            // is actually on screen fell outside the resting box and read as
            // "left the dock" -- which started a hide the user never asked for.
            reach.height += EDGE_MARGIN * monitor->m_scale; // the margin under the pill still counts as on it
            want = reach.containsPoint((POINT - monitor->m_position) * monitor->m_scale);
        }
    } else {
        // `require-pressure-to-show`, plus a POSITION test that every pointer
        // can satisfy.
        //
        // The pressure barrier alone was measured dead on this guest, and the
        // reason generalises: its feed hooks `CInputManager::onMouseMoved`,
        // which is the RELATIVE motion path, while the pointer here is a
        // `qemu-qemu-usb-tablet` -- an ABSOLUTE device. No relative event ever
        // arrives, so pressure never accumulates and the dock never comes out.
        // (An earlier test "passed" only because ydotool synthesised relative
        // deltas, which is not how the user's pointer moves.) Position is the
        // one thing every device agrees on; `show_delay_ms` is what keeps it
        // from being twitchy, and pressure still works for a relative mouse.
        const bool PRESSED = R.pressure >= Config::getValue<float>(KEY_PRESSURE);
        want               = Config::getValue<bool>(KEY_REQ_PRESSURE) ? (ATEDGE && PRESSED) : ATEDGE;

        // X77 (user, 2026-08-06): "if I move my cursor off it, it starts hiding
        // and I can't unhide until it's done hiding".
        //
        // `shown` flips FALSE the moment the hide begins, which switched this
        // test to the 4px screen-edge band -- while the pill was still sitting
        // on screen under the cursor, 60-odd px above that band. So coming back
        // to a dock that is visibly right there could not re-show it, and the
        // user had to wait out the slide and push into the very bottom edge
        // again. dash-to-dock has no such window: its hover lives on the actor,
        // which stays interactive for the whole slide.
        //
        // Being ON the pill that is still on screen counts, and it counts
        // IMMEDIATELY -- `show-delay` belongs to the pressure barrier, not to a
        // pointer that never really left.
        if (ATEDGE && hiddenFraction(monitor) < 1.F)
            immediate = true;

        if (!want) {
            if (CBox reach = slidPill(monitor); !reach.empty()) {
                reach.height += EDGE_MARGIN * monitor->m_scale;
                if (reach.containsPoint((POINT - monitor->m_position) * monitor->m_scale)) {
                    want      = true;
                    immediate = true;
                }
            }
        }
    }

    // X34 `hotkeys-show-dock`, ORed in beside URGENT for the same reason: it has
    // to HOLD the pill up as well as bring it out, or the pointer sitting
    // anywhere else would slide it away under the numbers the press just drew.
    // X54. A flight HOLDS the dock out, for the same reason X34's hotkey overlay
    // does: outside the overview the pill is normally hidden, so a minimize had
    // no icon to fly to -- the target lookup failed and the flight was dropped
    // on its first frame, which is why it teleported. The dock now comes out for
    // the flight and slides away after it.
    want = want || overlayHoldsDock() || Flight::anyOn(monitor);

    if (want == R.shown) {
        R.pending = false;
        return;
    }

    if (!R.pending) {
        R.pending      = true;
        R.pendingSince = Time::steadyNow();
    }

    if (!immediate && Hyprdrive::Config::realElapsedMs(R.pendingSince) < sc<float>(std::max(0, Config::getValue<int>(want ? KEY_SHOW_DELAY : KEY_HIDE_DELAY))))
        return;

    R.from      = hiddenFraction(monitor);
    R.shown     = want;
    R.changedAt = Time::steadyNow();
    R.pending   = false;
    R.pressure  = 0.F;
}
}


// dock:monitor names the one output that gets the dock: a connector name, or
// `desc:` + a prefix of the description, which survives a USB-C dock handing
// the same screen a different DP-N. A chosen screen that is not connected
// (the laptop off its desk) must not leave the user with no dock at all, so
// the choice only applies while it matches something.
bool onMonitor(PHLMONITOR monitor) {
    if (!monitor || !Config::getValue<bool>(KEY_ENABLED))
        return false;
    const auto WANT = Config::getValue<std::string>(KEY_MONITOR);
    if (WANT.empty())
        return true;
    const auto MATCHES = [&WANT](const PHLMONITOR& m) {
        if (!m)
            return false;
        if (WANT.starts_with("desc:"))
            return m->m_description.starts_with(std::string_view{WANT}.substr(5));
        return m->m_name == WANT;
    };
    if (MATCHES(monitor))
        return true;
    for (const auto& MON : State::monitorState()->monitors()) {
        if (MON && MON->m_enabled && MATCHES(MON))
            return false;
    }
    return true;
}

SLayout layout(PHLMONITOR monitor) {
    SLayout out = monitor ? arranged(monitor->m_id, baseLayout(monitor)) : baseLayout(monitor);
    if (out.pill.empty())
        return out;

    const float F = hiddenFraction(monitor);
    if (F <= 0.F)
        return out;

    // Fully away: no pill and, because everything else asks this function, no
    // hit area either -- so a click lands on the window underneath instead of
    // being swallowed by a dock that is not on screen.
    if (F >= 1.F)
        return SLayout{};

    const double DY = slideOffsetPx(monitor, out.pill.height);
    out.pill.y += DY;
    for (auto& item : out.items)
        item.iconBox.y += DY;
    if (!out.showApps.empty())
        out.showApps.y += DY;

    return out;
}
}
