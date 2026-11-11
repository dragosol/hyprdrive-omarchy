// Pointer and keyboard input: hit tests, drag-reorder, press, click,
// scroll, the menu's keys and clicks, and the info-poll timer.
#include "DockParts.hpp"
#include "MotionCurves.hpp"

namespace Hyprdrive::Dock {
using namespace DockDetail;


void feedPointerMotion(const Vector2D& delta) {
    const auto POINT = g_pInputManager->getMouseCoordsInternal();

    for (const auto& MONITOR : State::monitorState()->monitors()) {
        if (!MONITOR)
            continue;

        auto&      R   = g_reveal[MONITOR->m_id];
        const auto BOX = MONITOR->logicalBox();

        // Pressure does not carry: leave the barrier, or the monitor, and the
        // push starts again from zero.
        if (!BOX.containsPoint(POINT) || POINT.y < BOX.y + BOX.h - EDGE_BAND)
            R.pressure = 0.F;
        else
            R.pressure += sc<float>(std::max(0.0, delta.y)); // only DOWNWARD motion presses a bottom edge

        // Not updateAutohide: this must also pump the frame that the flip it may
        // have just armed needs, and damageIfChanged is where that lives.
        damageIfChanged(MONITOR);
    }
}
namespace DockDetail {

// Deliberately the overview's own threshold: two drags on one screen that start
// at different distances would read as one of them being sticky.
float dragThresholdPx(PHLMONITOR monitor) {
    return Config::getDragThreshold() * std::max(monitor ? sc<float>(monitor->m_scale) : 1.F, 0.01F);
}

// The favourite slots, in draw order. Only these take part in a reorder: a
// running app that is not pinned has no stored position to move, and the
// stretch of pill it occupies means "after the last favourite" -- which is what
// size() is.
std::vector<size_t> favoriteSlots(const SLayout& layout) {
    std::vector<size_t> out;
    for (size_t i = 0; i < layout.items.size(); ++i)
        if (layout.items[i].favorite || layout.items[i].placed)
            out.emplace_back(i);
    return out;
}

std::vector<size_t> stackSlots(const SLayout& layout) {
    std::vector<size_t> out;
    for (size_t i = 0; i < layout.items.size(); ++i)
        if (layout.items[i].stack && !layout.items[i].placeholder)
            out.emplace_back(i);
    return out;
}

bool draggingStack() {
    return g_dockDrag.drag && !g_dockDrag.drag->stackPath.empty();
}

int caretIndex() {
    return g_dockDrag.drag && g_dockDrag.drag->live ? g_dockDrag.drag->insert : g_dockDrag.foreignInsert;
}

// V3. The dragged icon's box THIS FRAME: the cell it came out of, carried to
// where the pointer is and clamped to the monitor so it cannot be dragged off
// the screen edge and become impossible to see coming back.
CBox ghostBoxAt(PHLMONITOR monitor, const CBox& home, const Vector2D& grab, const Vector2D& at) {
    if (!monitor || home.width < 1)
        return {};

    const float MONW = sc<float>(monitor->m_size.x) * monitor->m_scale;
    const float MONH = sc<float>(monitor->m_size.y) * monitor->m_scale;
    const float W    = sc<float>(home.width);
    const float H    = sc<float>(home.height);

    return CBox{std::clamp(sc<float>(at.x - grab.x), 0.F, std::max(0.F, MONW - W)), std::clamp(sc<float>(at.y - grab.y), 0.F, std::max(0.F, MONH - H)), W, H}.round();
}

// V3. Is a dragged icon being drawn under the pointer right now? Only once it
// is OFF the pill: over the pill the gesture is X32's reorder, which already
// says what it is doing with the insertion caret, and a second icon floating
// over the row it is being inserted into would be two answers to one question.
bool ghostShowing() {
    return g_dockDrag.drag && g_dockDrag.drag->live && g_dockDrag.drag->insert < 0 && !g_dockDrag.drag->icon.empty();
}

bool ghostFlying() {
    return g_dockDrag.ghostHome.has_value();
}

// Where the caret sits for an insertion index: hard against the left edge of
// the icon that would follow it, or past the last favourite when it is the end.
CBox caretBox(const SLayout& layout, int index, float scale) {
    // A stack drag's caret runs over the stack cells, not the favourites.
    const auto SLOTS = draggingStack() && !foldersAnywhere() ? stackSlots(layout) : favoriteSlots(layout);
    if (index < 0 || SLOTS.empty())
        return {};

    const float W = std::max(2.F, std::round(3.F * scale));
    const auto& REF = layout.items[SLOTS[std::min(sc<size_t>(index), SLOTS.size() - 1)]].iconBox;
    const bool  END = sc<size_t>(index) >= SLOTS.size();

    const double X = END ? REF.x + REF.width + ITEM_MARGIN_X * scale - W / 2.0 : REF.x - ITEM_MARGIN_X * scale - W / 2.0;
    return CBox{X, REF.y, W, REF.height}.round();
}
}


CBox cellHitBox(const CBox& iconBox, float scale) {
    return hoverBox(iconBox, scale);
}

bool pillContains(PHLMONITOR monitor, const Vector2D& point) {
    if (!monitor)
        return false;

    const auto PILL = layout(monitor).pill;
    return PILL.width >= 1 && PILL.height >= 1 && PILL.containsPoint(point);
}

SDropHit dropCellAt(PHLMONITOR monitor, const Vector2D& point, bool folderOffer) {
    using DockDropModel::ECell;
    SDropHit hit;
    if (!pillContains(monitor, point))
        return hit;
    const auto LAYOUT = layout(monitor);

    // The gap index: the midpoint rule over the REAL stacks. The placeholder is
    // not one of them, so while it sits under the pointer its neighbours are
    // pushed clear of it and the index holds still.
    const auto SLOTS = stackSlots(LAYOUT);
    hit.gapIndex     = sc<int>(SLOTS.size());
    for (size_t k = 0; k < SLOTS.size(); ++k)
        if (point.x < LAYOUT.items[SLOTS[k]].iconBox.x + LAYOUT.items[SLOTS[k]].iconBox.width / 2.0) {
            hit.gapIndex = sc<int>(k);
            break;
        }

    hit.cell        = folderOffer ? ECell::Gap : ECell::App;
    const auto ITEM = std::ranges::find_if(LAYOUT.items, [&](const SItem& item) { return hoverBox(item.iconBox, monitor->m_scale).containsPoint(point); });
    if (ITEM == LAYOUT.items.end())
        return hit;
    if (ITEM->placeholder)
        return hit; // Gap for a folder
    if (ITEM->stack && ITEM->stackDir) {
        // A folder is dropped INTO a stack only at its middle; its outer
        // quarters open a gap beside it, or no stack could ever be passed.
        const double REL = (point.x - ITEM->iconBox.x) / std::max(1.0, ITEM->iconBox.width);
        if (!folderOffer || (REL > 0.25 && REL < 0.75)) {
            hit.cell      = ECell::Stack;
            hit.stackPath = ITEM->stackPath;
        }
        return hit;
    }
    if (!ITEM->stack && ITEM->uri == "trash:///") {
        hit.cell = ECell::Trash;
        return hit;
    }
    if (!folderOffer)
        hit.cell = ECell::App;
    return hit;
}

bool uiContains(PHLMONITOR monitor, const Vector2D& point) {
    // EVERY dock surface, not just the pill. "The hover-preview plate is only
    // reached after the pointer is already on the pill" was true of the path,
    // not of where the pointer then IS: once it moved up onto the plate the
    // pill test failed, and the overview lit the tile BEHIND the preview as if
    // the preview were not there (user, 2026-09-29). Same for the context menu
    // and a stack's fan/pane.
    return occupiesPointer(monitor, point);
}

bool menuScroll(PHLMONITOR monitor, const Vector2D& point, bool up) {
    if (!menuContains(monitor, point))
        return false;
    // One row a tick; renderMenu clamps it to the list. A touchpad sends a
    // stream of small events: a short dead time keeps it a row at a time.
    static Time::steady_tp s_last{};
    if (elapsedMs(s_last) >= 35.F) {
        s_last = Time::steadyNow();
        g_menu->scrollTop += up ? -1 : 1;
        if (g_menu->scrollTop < 0)
            g_menu->scrollTop = 0;
        for (const auto& M : State::monitorState()->monitors())
            if (M && M->m_id == g_menu->monitor)
                g_pHyprRenderer->damageMonitor(M);
    }
    return true;
}

bool scroll(PHLMONITOR monitor, const Vector2D& point, bool up) {
    // The stack pane floats above everything else of the dock's (2026-09-24:
    // its wheel ticks were going to the window under it).
    if (Stacks::scroll(monitor, point, up))
        return true;
    // The context menu's window list (it scrolls when it would not fit).
    if (menuScroll(monitor, point, up))
        return true;
    if (!pillContains(monitor, point))
        return occupiesPointer(monitor, point); // hover plate / menu: swallow, do not leak

    const auto ACTION = Config::getValue<std::string>(KEY_SCROLL_ACTION);
    if (ACTION != "cycle-windows" && ACTION != "switch-workspace")
        return true; // `do-nothing` still SWALLOWS: the pill is not a window.

    // The dead time is checked before the hit-test and after the action, so a
    // swallowed tick does not start the clock and `do-nothing` never does. One
    // clock for both actions, as in the reference -- they can never both be on.
    static Time::steady_tp s_last{};
    static bool            s_seen = false;

    if (s_seen && elapsedMs(s_last) < SCROLL_DEAD_MS)
        return true;

    s_last = Time::steadyNow();
    s_seen = true;

    if (ACTION == "switch-workspace") {
        // `activeWs.get_neighbor(direction)`: up is the PREVIOUS workspace, and
        // an edge is a no-op rather than a wrap -- get_neighbor hands back the
        // same workspace when there is nothing past it.
        const auto ACTIVE = monitor->m_activeWorkspace;
        if (!ACTIVE)
            return true;

        const WORKSPACEID WANTED = ACTIVE->m_id + (up ? -1 : 1);
        for (const auto& CANDIDATE : State::workspaceState()->workspaces()) {
            const auto TARGET = CANDIDATE.lock();
            if (TARGET && TARGET->m_id == WANTED && TARGET != ACTIVE) {
                monitor->changeWorkspace(TARGET, false, true, true);
                break;
            }
        }

        return true;
    }

    // `cycle-windows` is an ICON action: the reference binds it to the AppIcon,
    // so the pill's own background does nothing under this setting. It still
    // swallowed the tick above, which is the point of the pill.
    const auto LAYOUT = layout(monitor);
    const auto ITEM   = std::ranges::find_if(LAYOUT.items, [&](const SItem& item) { return hoverBox(item.iconBox, monitor->m_scale).containsPoint(point); });
    if (ITEM == LAYOUT.items.end())
        return true;

    cycleOrActivate(*ITEM, up);
    if (g_dockSeatHeld)
        stealDockSeat();
    return true;
}

void showOverlay() {
    if (!Config::getValue<bool>(KEY_HOT_KEYS))
        return;

    // `_optionalNumberOverlay`: the extra shortcut is registered at all only
    // when one of the two is on, and each effect is gated again on its own key
    // inside `_showOverlay`. With both off this is a no-op, which is why the
    // deadline is not moved rather than moved and then ignored twice.
    if (!Config::getValue<bool>(KEY_HOTKEY_OVERLAY) && !Config::getValue<bool>(KEY_HOTKEY_DOCK))
        return;

    const float SECS = std::max(0.F, Config::getValue<float>(KEY_SHORTCUT_SECS));
    g_overlayUntil   = Time::steadyNow() + std::chrono::milliseconds(sc<int>(std::round(SECS * 1000.F)));

    // A keypress is not a frame, and render.pre only fires inside one, so the
    // first repaint has to be asked for here. Every monitor, because the
    // reference shows the numbers on every dock it has, not just the focused
    // one.
    for (const auto& MONITOR : State::monitorState()->monitors()) {
        if (MONITOR)
            damageIfChanged(MONITOR);
    }
}
namespace DockDetail {

// THE POINTER GOES WITH THE FOCUS (user, 2026-11-09: "make clicking the app
// or dock preview on the dock immediately focus cursor on the app in
// question", a toggle in the dock settings; then: "clicking its icon will
// minimize ... ctrl-clicking it should also focus it ... it needs to CENTER
// the cursor on it"): a window preview click, or a Ctrl+click on an icon,
// that brings a window forward puts the pointer on its middle -- where it lands, so it rides a
// desktop switch or a canvas pan. Not while the overview is up (its close
// places the pointer itself), not when the click focused nothing new.
bool pointerFollowsClick() {
    return Config::getValue<bool>("plugin:hyprdrive:dock:pointer_to_window");
}

void pointerToWindow(const PHLWINDOW& window) {
    if (!window || !Desktop::View::validMapped(window) || Minimize::isMinimized(window) || Desktop::focusState()->window() != window)
        return;
    const CBox BOX{window->m_realPosition->goal(), window->m_realSize->goal()};
    if (BOX.width >= 1 && BOX.height >= 1)
        Pointer::pointerController()->warpTo(BOX.middle(), true);
}
}

static bool clickImpl(PHLMONITOR monitor, const Vector2D& point, bool shift, bool middle, bool ctrl);

bool click(PHLMONITOR monitor, const Vector2D& point, bool shift, bool middle, bool ctrl) {
    // The app clicked, and its windows, BEFORE the click changes the dock: the
    // pointer follows only to one of them (a click that minimizes hands the
    // focus to some other window, and the pointer stays on the dock).
    std::vector<PHLWINDOW> mine;
    // Only a Ctrl+click (user, 2026-11-09: a plain click on the icon is the
    // minimize/restore toggle, and the pointer has to stay on the dock for
    // the next one).
    if (monitor && ctrl && !middle && pointerFollowsClick()) {
        const auto LAYOUT = layout(monitor);
        const auto ITEM   = std::ranges::find_if(LAYOUT.items, [&](const SItem& item) { return hoverBox(item.iconBox, monitor->m_scale).containsPoint(point); });
        if (ITEM != LAYOUT.items.end() && !ITEM->stack && ITEM->uri.empty())
            mine = itemWindows(*ITEM);
    }
    const auto BEFORE  = Desktop::focusState()->window();
    const bool HANDLED = clickImpl(monitor, point, shift, middle, ctrl);
    if (HANDLED && !mine.empty() && !scrollOverviewForMonitor(monitor)) {
        const auto NOW = Desktop::focusState()->window();
        if (NOW && NOW != BEFORE && std::ranges::find(mine, NOW) != mine.end())
            pointerToWindow(NOW);
    }
    return HANDLED;
}

static bool clickImpl(PHLMONITOR monitor, const Vector2D& point, bool shift, bool middle, bool ctrl) {
    if (!monitor)
        return false;

    const auto LAYOUT = layout(monitor);
    if (LAYOUT.pill.width < 1 || LAYOUT.pill.height < 1)
        return false;

    // X33. The middle button reaches ICONS only. A preview is one window and
    // "launch a new one" is not a thing to do to it, and Show Applications is a
    // button rather than an app -- the reference's middle-click lives on the
    // AppIcon and nowhere else.
    if (middle) {
        const auto ITEM =
            std::ranges::find_if(LAYOUT.items, [&](const SItem& item) { return hoverBox(item.iconBox, monitor->m_scale).containsPoint(point); });
        if (ITEM == LAYOUT.items.end())
            return false;

        applyClickAction(monitor, *ITEM, Config::getValue<std::string>(shift ? KEY_SHIFT_MIDDLE : KEY_MIDDLE_CLICK), shift);
        return true;
    }

    // X32 Show Applications. Ahead of the icons because its cell is laid out
    // like one and would otherwise have to be excluded from their search.
    if (!LAYOUT.showApps.empty() && hoverBox(LAYOUT.showApps, monitor->m_scale).containsPoint(point)) {
        // The app GRID this opens is G5's, and G5 is not built. What X32 owns is
        // the button: its slot, its edge, and the fact that it opens the surface
        // apps are searched from. So on the desktop it opens the overview; with
        // the overview already up there is nothing further to show yet, and it
        // deliberately does NOT toggle it shut -- a button that closes the thing
        // it is supposed to open would be worse than one that waits for G5.
        //
        // A fresh open from the dock starts CLEAN (orchestrator, 2026-08-11).
        // The button is the other way into the surface the dispatcher opens,
        // and it was the only one that did not begin a new search session --
        // so a query typed into a previous open survived a close and came back
        // with it. The reset belongs here, at the entry point, and NOT inside
        // openOverview(): that is also the reopen path for a toggle landing on
        // a closing overview, where dropping the filter would pop the windows
        // it was hiding back into a close animation already under way.
        // Gate: tests/gates/x32_showapps_fresh.
        // Show Overview / Exit Overview (user, 2026-09-24): the same button
        // opens the overview from the desktop and, inside it, takes you back.
        if (const auto OVERVIEW = scrollOverviewForMonitor(monitor); !OVERVIEW) {
            resetSearchSession();
            openScrollOverview(monitor);
        } else
            OVERVIEW->close();

        return true;
    }

    // Hit-test the same cell the hover highlight lights up, so what looks
    // clickable is what is clickable.
    const auto ITEM = std::ranges::find_if(LAYOUT.items, [&](const SItem& item) { return hoverBox(item.iconBox, monitor->m_scale).containsPoint(point); });
    if (ITEM == LAYOUT.items.end())
        return false;

    // SUPER / ALT / CTRL held: "open it in an empty workspace" when it is not
    // running. Read HERE, not in activateItem -- SUPER+number hotkeys go
    // through activateItem too, with SUPER held by definition.
    const uint32_t MODS  = g_pInputManager ? g_pInputManager->getModsFromAllKBs() : 0;
    const bool     EMPTY = ctrl || (MODS & (HL_MODIFIER_META | HL_MODIFIER_ALT | HL_MODIFIER_CTRL)) != 0;
    // A folder: the fan (or grid) and the file manager are its two opens, and
    // Super picks the one its view does not (View in Files: click = the file
    // manager, Super+click = the fan; the other views the other way round).
    if (ITEM->stack && ITEM->stackDir && !ITEM->uri.empty()) {
        bool files = false;
        for (const auto& S : Stacks::all())
            if (S.path == ITEM->stackPath)
                files = S.view == StackModel::EView::Files;
        if (((MODS & HL_MODIFIER_META) != 0) != files) {
            Locations::activate(ITEM->uri);
            return true;
        }
    }
    activateItem(monitor, *ITEM, shift, ctrl, EMPTY);
    return true;
}

int dropIndexAt(PHLMONITOR monitor, const Vector2D& point) {
    if (!monitor)
        return -1;

    const auto LAYOUT = layout(monitor);
    if (LAYOUT.pill.width < 1 || !LAYOUT.pill.containsPoint(point))
        return -1;

    const auto SLOTS = favoriteSlots(LAYOUT);
    for (size_t k = 0; k < SLOTS.size(); ++k) {
        // The MIDPOINT, not the edge. Dropping on an icon's left half puts the
        // dragged app before it; an edge test leaves a dead strip between icons
        // where the drop has no answer at all.
        const auto& BOX = LAYOUT.items[SLOTS[k]].iconBox;
        if (point.x < BOX.x + BOX.width / 2.0)
            return sc<int>(k);
    }

    return sc<int>(SLOTS.size());
}

// Where a dragged STACK would land among the stacks: the same midpoint rule as
// dropIndexAt, over the stack cells only. -1 off the pill.
int stackDropIndexAt(PHLMONITOR monitor, const Vector2D& point) {
    if (!monitor)
        return -1;
    const auto LAYOUT = layout(monitor);
    if (LAYOUT.pill.width < 1 || !LAYOUT.pill.containsPoint(point))
        return -1;
    const auto SLOTS = stackSlots(LAYOUT);
    for (size_t k = 0; k < SLOTS.size(); ++k) {
        const auto& BOX = LAYOUT.items[SLOTS[k]].iconBox;
        if (point.x < BOX.x + BOX.width / 2.0)
            return sc<int>(k);
    }
    return sc<int>(SLOTS.size());
}

bool showAppsContains(PHLMONITOR monitor, const Vector2D& point) {
    if (!monitor)
        return false;

    const auto LAYOUT = layout(monitor);
    return !LAYOUT.showApps.empty() && hoverBox(LAYOUT.showApps, monitor->m_scale).containsPoint(point);
}

void pinAt(const std::string& desktopId, int index) {
    if (desktopId.empty() || index < 0)
        return;

    auto& FAV = favorites();

    // Where the id sits TODAY, counted the way `index` is counted. Removing the
    // dragged icon shifts everything after it one place left, so a move to a
    // later slot would otherwise land one short of the caret the user saw.
    int from = -1;
    for (int seen = 0; const auto& ID : FAV) {
        if (!drawnFavorite(ID))
            continue;
        if (ID == desktopId) {
            from = seen;
            break;
        }
        ++seen;
    }

    if (from >= 0) {
        std::erase(FAV, desktopId);
        if (from < index)
            --index;
    }

    // `index` counts DRAWN favourites, but the store also holds ids this machine
    // does not provide -- the seed is the user's HOST list. Walk the store to
    // find the position the index-th drawn favourite occupies, so the undrawn
    // ones keep their places instead of being silently reordered around.
    size_t at = FAV.size();
    for (size_t i = 0, seen = 0; i < FAV.size(); ++i) {
        if (!drawnFavorite(FAV[i]))
            continue;
        if (seen == sc<size_t>(index)) {
            at = i;
            break;
        }
        ++seen;
    }

    FAV.insert(FAV.begin() + sc<long>(at), desktopId);
    writeFavorites();
}

void unpin(const std::string& desktopId) {
    if (desktopId.empty())
        return;

    if (std::erase(favorites(), desktopId) > 0)
        writeFavorites();
}

void beginDesktopTearOut(PHLMONITOR monitor, const PHLWINDOW& window, const CBox& home) {
    if (!monitor || !window)
        return;

    // FIRST, before the move and the focus below change it: the window the user
    // was working in is what the drop tiles beside (Placement.hpp).
    Placement::noteDragAnchor(Desktop::focusState()->window());

    if (const auto ACTIVE = monitor->m_activeWorkspace; ACTIVE && window->m_workspace != ACTIVE)
        Desktop::globalWindowController()->moveWindowToWorkspace(window, ACTIVE);

    const float SCALE = std::max(monitor->m_scale, 0.01F);
    if (home.width >= 1.0 && home.height >= 1.0 && window->m_realSize && window->m_realPosition) {
        window->m_realPosition->m_Value = Vector2D{home.x / SCALE + monitor->m_position.x, home.y / SCALE + monitor->m_position.y};
        window->m_realSize->m_Value     = Vector2D{std::max(4.0, home.width / SCALE), std::max(4.0, home.height / SCALE)};
    }

    Desktop::focusState()->fullWindowFocus(window, Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);

    if (const auto TARGET = window->layoutTarget(); TARGET && g_layoutManager && g_layoutManager->dragController()) {
        g_layoutManager->dragController()->dragBegin(TARGET, MBIND_MOVE);
        g_dockNativeDrag = true;
    }
}


// A damped spring released from full depth at rest: 1 at t=0 with zero
// velocity, swinging through 0 into a small negative overshoot and settling.
float releaseResponse(float ms) {
    if (ms <= 0.F)
        return 1.F;
    if (ms >= RELEASE_SETTLE_MS)
        return 0.F;

    const float T     = ms / 1000.F;
    const float ROOT  = std::sqrt(1.F - RELEASE_ZETA * RELEASE_ZETA);
    const float OMEGD = RELEASE_OMEGA * ROOT;
    return std::exp(-RELEASE_ZETA * RELEASE_OMEGA * T) * (std::cos(OMEGD * T) + (RELEASE_ZETA / ROOT) * std::sin(OMEGD * T));
}

// 1 fully pressed, 0 at rest, NEGATIVE during the release overshoot.
float pressDepth(MONITORID monitor, const std::string& subject) {
    if (!g_press || g_press->monitor != monitor || g_press->subject != subject)
        return 0.F;

    const float IN = Hyprdrive::Motion::outQuint(std::clamp(elapsedMs(g_press->since) / PRESS_IN_MS, 0.F, 1.F));
    if (!g_press->released)
        return IN;

    // upAt can still be in the future: a quick click finishing its press-in.
    const float SINCEUP = elapsedMs(g_press->upAt);
    return SINCEUP < 0.F ? IN : releaseResponse(SINCEUP);
}

Vector2D pressAt(MONITORID monitor, const std::string& subject) {
    return g_press && g_press->monitor == monitor && g_press->subject == subject ? g_press->at : Vector2D{};
}

bool pressAnimating(MONITORID monitor) {
    if (!g_press)
        return false;

    // THE EXPIRY IS NOT GATED ON THE MONITOR, and the order matters. Whichever
    // dock renders next retires a press that has finished, so a press cannot
    // outlive an output that has gone quiet -- which would leave that pill
    // holding a squish nothing was left to spring back.
    if (g_press->released && elapsedMs(g_press->upAt) >= RELEASE_SETTLE_MS + PRESS_TAIL_MS) {
        g_press.reset();
        return false;
    }

    // ...but only the pill that was pressed is the one moving.
    if (g_press->monitor != monitor)
        return false;

    // Held and fully in: nothing moves, so nothing needs a frame.
    if (!g_press->released)
        return elapsedMs(g_press->since) < PRESS_IN_MS;

    return true;
}

// Normalised against the icon itself, clamped: the hover box is a little larger
// than the icon, and a point in its margin leans fully, not beyond.
Vector2D normalizedIn(const CBox& icon, const Vector2D& point) {
    return {std::clamp((point.x - (icon.x + icon.width / 2.0)) / std::max(icon.width / 2.0, 1.0), -1.0, 1.0),
            std::clamp((point.y - (icon.y + icon.height / 2.0)) / std::max(icon.height / 2.0, 1.0), -1.0, 1.0)};
}

// The pointer as DockTilt's hover wants it: over this cell, or not at all.
std::optional<Vector2D> pointerIn(const CBox& icon, const Vector2D& mouse, double scale) {
    if (icon.width < 1 || icon.height < 1 || !hoverBox(icon, scale).containsPoint(mouse))
        return std::nullopt;
    return normalizedIn(icon, mouse);
}

void pressBegin(PHLMONITOR monitor, const Vector2D& point) {
    g_press.reset();
    if (!monitor)
        return;

    const auto LAYOUT = layout(monitor);

    std::string subject;
    CBox        icon;
    if (const auto ITEM = std::ranges::find_if(LAYOUT.items, [&](const SItem& item) { return hoverBox(item.iconBox, monitor->m_scale).containsPoint(point); });
        ITEM != LAYOUT.items.end()) {
        subject = ITEM->subject;
        icon    = ITEM->iconBox;
    } else if (!LAYOUT.showApps.empty() && hoverBox(LAYOUT.showApps, monitor->m_scale).containsPoint(point)) {
        subject = "show-apps";
        icon    = LAYOUT.showApps;
    }

    if (subject.empty() || icon.width < 1 || icon.height < 1)
        return;

    g_press = SPress{.subject = subject, .monitor = monitor->m_id, .since = Time::steadyNow(), .at = normalizedIn(icon, point)};
}

void pressEnd() {
    if (!g_press || g_press->released)
        return;

    g_press->released = true;
    g_press->upAt     = std::max(Time::steadyNow(), g_press->since + std::chrono::milliseconds(sc<int>(PRESS_IN_MS)));
}

bool dragBegin(PHLMONITOR monitor, const Vector2D& point) {
    g_dockDrag.drag.reset();

    if (!monitor)
        return false;

    const auto LAYOUT = layout(monitor);
    const auto ITEM   = std::ranges::find_if(LAYOUT.items, [&](const SItem& item) { return hoverBox(item.iconBox, monitor->m_scale).containsPoint(point); });
    if (ITEM == LAYOUT.items.end() || (ITEM->desktopId.empty() && !ITEM->stack))
        return false;

    g_dockDrag.drag = SDockDrag{.id        = ITEM->desktopId,
                                .monitor   = monitor->m_id,
                                .start     = point,
                                .icon      = ITEM->icon,
                                .home      = ITEM->iconBox,
                                .grab      = point - Vector2D{ITEM->iconBox.x, ITEM->iconBox.y},
                                .at        = point,
                                .stackPath = ITEM->stack ? ITEM->stackPath : std::string{}};
    return true;
}

bool dragMove(PHLMONITOR monitor, const Vector2D& point) {
    if (!monitor)
        return false;

    // A preview being torn out owns the motion; it goes live on the same
    // threshold the icon drag uses, so the two gestures cannot feel different.
    if (g_dockDrag.panel) {
        g_dockDrag.panel->at = point;
        if (!g_dockDrag.panel->live) {
            const float T = dragThresholdPx(monitor);
            if (g_dockDrag.panel->start.distanceSq(point) <= T * T)
                return false;

            // HAND OFF, and stop owning the gesture. Past the threshold this is
            // the REAL window being dragged -- not a ghost of it -- so the dock
            // gets out of the way entirely and the drag machinery that already
            // knows how to shrink at the dock boundary and minimize on a drop
            // back into it takes over. The dock's job was to say which window
            // and from which box.
            const auto WINDOW = g_dockDrag.panel->window.lock();
            const CBox HOME   = g_dockDrag.panel->home;
            g_dockDrag.panel.reset();
            if (!WINDOW)
                return false;

            dismissHoverPanelNow();

            // A MINIMIZED window is stashed: it sits on the stash workspace with
            // no tile, so the overview's drag refuses it (shouldShowOverviewWindow
            // / layoutTarget both fail) and the desktop's has no target to hand
            // Hyprland -- which is why tearing one out did nothing whatsoever.
            // Un-stash it first and the rest of the gesture is the ordinary one.
            // The dock panel lists minimized windows on purpose (itemWindows
            // includes them, so a minimized window is reachable from its icon),
            // so this is a case the tear-out has to answer for, not reject.
            if (Minimize::isMinimized(WINDOW))
                Minimize::restore(WINDOW);

            if (const auto OVERVIEW = scrollOverviewForMonitor(monitor)) {
                // Same story for a window the SEARCH hides (user, 2026-09-24:
                // dragging another app's preview under Show Windows "refuses to
                // do it"): the filter goes, and the drag is the ordinary one.
                overviewAdmitWindow(WINDOW);
                OVERVIEW->beginWindowDragFrom(WINDOW, HOME); // grows out of the cell
                // IMMEDIATELY, not on the next frame. The overview syncs this
                // flag once per frame (Overview.cpp, end of render), and a
                // release that lands before that frame would be claimed by the
                // dock's own ladder instead of ending the drag -- leaving
                // drag.activeWindow set forever. A window held in that state is
                // excluded from the overview's tiles (it is meant to be drawn as
                // the drag proxy), so it goes invisible with no way back.
                setWindowDragActive(true);
            } else
                beginDesktopTearOut(monitor, WINDOW, HOME);
            return true;
        }
        return true;
    }

    if (!g_dockDrag.drag)
        return false;

    g_dockDrag.drag->at = point;

    if (!g_dockDrag.drag->live) {
        const float T = dragThresholdPx(monitor);
        if (g_dockDrag.drag->start.distanceSq(point) <= T * T)
            return false;

        g_dockDrag.drag->live = true;
        // Past the threshold it is a DRAG: the ghost carries the icon now, and
        // the cell it left must not stay pressed in behind it.
        pressEnd();
    }

    g_dockDrag.drag->insert = g_dockDrag.drag->stackPath.empty() || foldersAnywhere() ? dropIndexAt(monitor, point) : stackDropIndexAt(monitor, point);
    return true;
}

void dragCancel() {
    pressEnd();
    g_dockDrag.drag.reset();
    g_dockDrag.panel.reset();
}

bool dragPending() {
    return g_dockDrag.drag.has_value() || g_dockDrag.panel.has_value();
}

bool dragging() {
    return (g_dockDrag.drag && g_dockDrag.drag->live) || (g_dockDrag.panel && g_dockDrag.panel->live);
}

void setForeignInsert(int index) {
    g_dockDrag.foreignInsert = index;
}

bool openMenu(PHLMONITOR monitor, const Vector2D& point) {
    closeMenu();

    if (!monitor)
        return false;

    const auto LAYOUT = layout(monitor);
    if (LAYOUT.pill.empty())
        return false;

    // The same cell the hover highlight and the left-click use, so the three
    // gestures cannot disagree about which icon is under the pointer.
    const auto ITEM = std::ranges::find_if(LAYOUT.items, [&](const SItem& item) { return hoverBox(item.iconBox, monitor->m_scale).containsPoint(point); });
    if (ITEM == LAYOUT.items.end())
        return false;

    g_menuGhost.reset();
    g_menu = SMenu{.monitor = monitor->m_id, .item = *ITEM, .anchor = ITEM->iconBox, .box = {}, .rows = buildRows(*ITEM, monitor)};
    g_menu->openedAt = Time::steadyNow();
    return true;
}

// THE WINDOW'S OWN MENU, off a right-click on its preview in the hover pane.
// The pane stays up under it (Dock.cpp's PANELWANTED), the cell is what the
// plate grows out of, and the item is the pane's app, so the rows act as the
// app menu's rows for that window would.
bool openWindowMenu(PHLMONITOR monitor, const Vector2D& point) {
    if (!monitor || !g_hover.panel || !panelContains(monitor, point))
        return false;
    const auto CELL = panelCellAt(monitor, point);
    if (!CELL || !CELL->second)
        return false;
    closeMenu();

    const auto& P      = *g_hover.panel;
    const auto  LAYOUT = layout(monitor);
    SItem       item;
    if (const auto IT = std::ranges::find_if(LAYOUT.items, [&](const SItem& i) { return i.wmClass == P.wmClass && i.desktopId == P.desktopId && i.scratchpadName == P.scratchpadName; });
        IT != LAYOUT.items.end())
        item = *IT;
    else {
        item.wmClass        = P.wmClass;
        item.desktopId      = P.desktopId;
        item.scratchpadName = P.scratchpadName;
    }

    g_menuGhost.reset();
    g_menu = SMenu{.monitor = monitor->m_id, .item = item, .anchor = CELL->first, .box = {}, .rows = buildWindowRows(item, CELL->second, monitor)};
    g_menu->windowScope = true;
    g_menu->scope       = CELL->second;
    g_menu->openedAt    = Time::steadyNow();
    return true;
}

// A window closed. Its own menu goes with it; an app menu listing it drops
// its row at the next draw (the window is unmapped by then).
void onWindowClose(const PHLWINDOW& window) {
    if (!g_menu || !window)
        return;
    if (g_menu->windowScope) {
        const auto SCOPE = g_menu->scope.lock();
        if (!SCOPE || SCOPE == window)
            closeMenu();
    } else if (std::ranges::any_of(g_menu->rows, [&](const SRow& r) { return r.kind == ERow::Window && r.window.lock() == window; }))
        g_menu->stale = true;
    else
        return;
    for (const auto& MON : State::monitorState()->monitors())
        if (MON && MON->m_id == (g_menu ? g_menu->monitor : g_menuGhost ? g_menuGhost->menu.monitor : MONITOR_INVALID))
            g_pHyprRenderer->damageMonitor(MON);
}

bool menuOpen() {
    return g_menu.has_value();
}

void closeMenu() {
    if (g_menu && !g_menu->box.empty() && menuAnimOn() && !g_menu->editing) {
        const float P = menuOpenP(*g_menu);
        g_menuGhost   = SMenuGhost{.menu = std::move(*g_menu), .closedAt = Time::steadyNow(), .from = std::min(P, 1.F)};
    }
    g_menu.reset();
}

bool menuWantsKeyboard() {
    return g_menu && g_menu->editing;
}

// The Rename field's keys. Enter saves (an unchanged or empty name restores the
// folder's own), Esc cancels, and every other key belongs to the field while it
// is up -- nothing typed at it reaches the window underneath.
bool menuHandleKey(uint32_t keycode) {
    if (!menuWantsKeyboard())
        return false;
    auto& M = *g_menu;
    if (keycode == 28) { // ENTER
        const auto BASE = Stacks::labelFor(M.item.stackPath);
        Stacks::setLabel(M.item.stackPath, M.editText == BASE ? std::string{} : M.editText);
        closeMenu();
        return true;
    }
    if (keycode == 1) { // ESC
        closeMenu();
        return true;
    }
    switch (keycode) {
        case 29: case 42: case 54: case 56: case 97: case 100: case 125: case 126: return false; // modifiers
        default: break;
    }
    if (keycode == 14) { // BACKSPACE: on the untouched name, clears it all
        if (M.editFresh)
            M.editText.clear();
        else
            TextInput::popCodepoint(M.editText);
    } else if (const auto K = TextInput::keyFor(overviewActiveXkbState(), keycode); K.consume) {
        if (M.editFresh)
            M.editText.clear(); // typing over the selected name, as any rename field does
        M.editText += K.text;
    } else
        return true;
    M.editFresh = false;
    M.rows.front().label = M.editText + "\u2502";
    for (const auto& MON : State::monitorState()->monitors())
        if (MON && MON->m_id == M.monitor)
            g_pHyprRenderer->damageMonitor(MON);
    return true;
}

bool menuContains(PHLMONITOR monitor, const Vector2D& point) {
    return g_menu && monitor && g_menu->monitor == monitor->m_id && !g_menu->box.empty() && g_menu->box.containsPoint(point);
}
namespace DockDetail {

    int infoPollCb(void*) {
        if (!g_menu || !g_menu->infoView)
            return 0;
        for (const auto& MON : State::monitorState()->monitors())
            if (MON && MON->m_id == g_menu->monitor)
                g_pHyprRenderer->damageMonitor(MON);
        if (AppInfo::generation() == g_menu->infoGen && g_infoPoll)
            wl_event_source_timer_update(g_infoPoll, 60);
        return 0;
    }
}

void armInfoPoll() {
    if (!g_infoPoll)
        g_infoPoll = wl_event_loop_add_timer(g_pCompositor->m_wlEventLoop, infoPollCb, nullptr);
    if (g_infoPoll)
        wl_event_source_timer_update(g_infoPoll, 60);
}

void stopUrgentHopTimer() {
    if (g_hopTimer) {
        wl_event_source_remove(g_hopTimer);
        g_hopTimer = nullptr;
    }
}

void stopInfoPoll() {
    if (g_infoPoll) {
        wl_event_source_remove(g_infoPoll);
        g_infoPoll = nullptr;
    }
}

bool menuClick(PHLMONITOR monitor, const Vector2D& point) {
    if (!menuContains(monitor, point))
        return false;

    const auto ROW = std::ranges::find_if(g_menu->rows, [&](const SRow& row) { return row.kind != ERow::Separator && !row.box.empty() && row.box.containsPoint(point); });
    if (ROW == g_menu->rows.end())
        return false; // the plate, between rows: swallowed by the caller, menu stays up

    // A window row's button: the move it names, for that row's window.
    for (const auto& B : ROW->inlines) {
        if (B.box.empty() || !B.box.containsPoint(point))
            continue;
        if (B.kind == ERow::Separator)
            return true; // an empty slot: not a button, and not the row either
        const auto ITEM = g_menu->item;
        SRow       PICK{.kind = B.kind, .label = B.label, .window = ROW->window};
        // An "Always Open in" chip is a setting, not an action: it flips and
        // the menu stays up, the chip lit (or not) where the pointer is.
        if (B.kind == ERow::AlwaysEmptyWorkspace || B.kind == ERow::AlwaysFullscreen) {
            activateRow(monitor, ITEM, PICK);
            // Re-read in place, not rebuilt: the rows keep the boxes the
            // pointer is being hit-tested against, so the hover does not blink.
            const auto ID = itemDesktopId(ITEM);
            for (auto& C : ROW->inlines)
                C.on = C.kind == ERow::AlwaysFullscreen ? opensInFullscreen(ID) : C.kind == ERow::AlwaysEmptyWorkspace ? opensInEmptyDesktop(ID) : C.on;
            return true;
        }
        // A window row's close: the menu stays up for the app's other
        // windows, and the row leaves it once the window has really gone
        // (onWindowClose; a client that asks its user first keeps its row).
        // The last window's close is Quit's flow: the menu goes with it.
        if (B.kind == ERow::CloseWindow && !g_menu->windowScope) {
            const auto MINE   = ROW->window.lock();
            const bool OTHERS = std::ranges::any_of(g_menu->rows, [&](const SRow& r) {
                const auto W = r.kind == ERow::Window ? r.window.lock() : PHLWINDOW{};
                return W && W != MINE && W->m_isMapped;
            });
            if (OTHERS) {
                activateRow(monitor, ITEM, PICK);
                return true;
            }
        }
        closeMenu();
        activateRow(monitor, ITEM, PICK);
        return true;
    }
    if (ROW->segmented)
        return true; // between two segments, or on the row's own label: nothing

    // Get Info / Back: the menu stays up and changes what it shows.
    // Dock stacks A: Empty Folder asks first, in place; Rename becomes a field.
    if (ROW->kind == ERow::StackEmptyAsk) {
        const auto N = StackReader::listing(g_menu->item.stackPath).entries.size();
        g_menu->rows = {
            SRow{.kind = ERow::InfoLine, .label = "Move " + std::to_string(N) + (N == 1 ? " item" : " items") + " to Trash?", .glyph = MenuGlyph::TRASH},
            SRow{.kind = ERow::Separator},
            SRow{.kind = ERow::StackEmptyCancel, .label = "Cancel", .glyph = MenuGlyph::BACK},
            SRow{.kind = ERow::StackEmptyConfirm, .label = "Move to Trash", .glyph = MenuGlyph::TRASH, .danger = true},
        };
        return true;
    }
    // Empty Trash asks first when Nautilus will not: without it the empty is
    // `gio trash --empty`, which deletes for good and asks nothing.
    if (ROW->kind == ERow::EmptyTrash && !ROW->disabled && !Locations::emptyTrashAsksFirst()) {
        const auto N = Locations::trashCount();
        g_menu->rows = {
            SRow{.kind = ERow::InfoLine, .label = N ? "Delete " + std::to_string(N) + (N == 1 ? " item" : " items") + " for good?" : std::string{"Empty the Trash for good?"}, .glyph = MenuGlyph::TRASH},
            SRow{.kind = ERow::Separator},
            SRow{.kind = ERow::StackEmptyCancel, .label = "Cancel", .glyph = MenuGlyph::BACK},
            SRow{.kind = ERow::EmptyTrashConfirm, .label = "Empty Trash", .glyph = MenuGlyph::TRASH, .danger = true},
        };
        return true;
    }
    if (ROW->kind == ERow::StackEmptyCancel) {
        g_menu->rows = buildRows(g_menu->item, monitor);
        return true;
    }
    if (ROW->kind == ERow::StackRename) {
        g_menu->editing   = true;
        g_menu->editText  = g_menu->item.locationName;
        g_menu->editFresh = true;
        g_menu->rows     = {SRow{.kind = ERow::StackRenameField, .label = g_menu->editText + "\u2502", .glyph = MenuGlyph::PENCIL, .hint = "type to rename \u00b7 \u21b5 save"}};
        return true;
    }
    if (ROW->kind == ERow::StackRenameField || ROW->kind == ERow::InfoLine || ROW->disabled)
        return true; // the field and the question are not buttons

    if (ROW->kind == ERow::Details || ROW->kind == ERow::InfoBack) {
        const bool INFO    = ROW->kind == ERow::Details;
        const auto ITEM    = g_menu->item;
        g_menu->infoView   = INFO;
        g_menu->infoGen    = AppInfo::generation();
        g_menu->rows       = INFO ? infoRows(ITEM) : buildRows(ITEM, monitor);
        if (INFO)
            armInfoPoll();
        return true;
    }

    // Copied before the menu goes away: activating a row can close the overview,
    // and g_menu must not be read through after that.
    const auto ITEM = g_menu->item;
    const auto PICK = *ROW;
    closeMenu();

    activateRow(monitor, ITEM, PICK);
    return true;
}
}
