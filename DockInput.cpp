#include "DockInput.hpp"

#include "DockStacks.hpp"

#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/devices/IKeyboard.hpp>

using namespace ScrollOverview;

namespace {
bool shiftHeld() {
    const auto KEEB = g_pSeatManager ? g_pSeatManager->m_keyboard.lock() : nullptr;
    return KEEB && (KEEB->getModifiers() & HL_MODIFIER_SHIFT);
}
bool ctrlHeld() {
    const auto KEEB = g_pSeatManager ? g_pSeatManager->m_keyboard.lock() : nullptr;
    return KEEB && (KEEB->getModifiers() & HL_MODIFIER_CTRL);
}
}

namespace ScrollOverview::DockInput {

SClaim handleMenu(PHLMONITOR mon, const Vector2D& local, EButton role, bool pressed, bool windowDragActive, bool menuMissLetsThrough) {
    SClaim out;

    // §13.9 X28. Right/resize press on the pill (re)opens the menu, ahead of every
    // other gesture -- it floats over whatever is beneath exactly as the pill
    // does. Only the PRESS opens; the release that follows lands outside the plate
    // and treating it as a click would dismiss the menu in the same gesture.
    if (role == EButton::Menu && pressed && !windowDragActive && Dock::pillContains(mon, local)) {
        // V11 V4. A right-click on a cell is the stack's settings, so the fan
        // must get out of the way first -- two plates over one cell, one of
        // them modal, is the stack the menu branch already refuses to draw.
        Stacks::close();
        Stacks::abandonItemDrag();
        Dock::openMenu(mon, local);
        out.claimed = true;
        return out;
    }

    // While the menu is up it is modal and takes every press: a press inside runs
    // the row, a press that misses closes it. On the plain desktop that miss is
    // LET THROUGH (menuMissLetsThrough) -- the menu there may have no drawn
    // geometry, and swallowing a miss into a dead menu ate every press until a
    // restart (measured 2026-08-06). The overview always draws, so it swallows.
    // A release is always swallowed; the pill is not a window.
    if (Dock::menuOpen()) {
        if (!pressed) {
            out.claimed = true;
            return out;
        }
        if (Dock::menuContains(mon, local)) {
            Dock::menuClick(mon, local);
            out.claimed = true;
            return out;
        }
        Dock::closeMenu();
        out.claimed = !menuMissLetsThrough;
        return out;
    }

    // A stray menu-button edge over the pill with no menu open is swallowed: the
    // pill is not a window and a right press/release must not reach through to it.
    if (role == EButton::Menu && Dock::pillContains(mon, local)) {
        out.claimed = true;
        return out;
    }

    // X33. The middle button on the pill launches/raises the app, and it MUST be
    // claimed here: BTN_MIDDLE is also the overview's secondary drag button, so a
    // miss would start a workspace pan instead of launching. Press acts, both
    // edges eaten.
    if (role == EButton::Launch && !windowDragActive && Dock::pillContains(mon, local)) {
        if (pressed)
            Dock::click(mon, local, shiftHeld(), true);
        out.claimed = true;
        return out;
    }

    return out;
}

SClaim handlePill(PHLMONITOR mon, const Vector2D& local, EButton role, bool pressed, bool windowDragActive) {
    SClaim out;
    if (role != EButton::Main)
        return out;

    // EVERY left release ends a press, first and unconditionally. The branches
    // below return early for the stack popup and the hover panel, and a press on
    // a non-app icon (trash, a stack, the scratchpad) has no drag state for a
    // release off the pill to find -- any of those would leave the icon shrunk.
    if (!pressed)
        Dock::pressEnd();

    // V11 V4. The stack popup floats above BOTH the pill and the hover panel,
    // so it is asked before either. It also answers a RELEASE that has wandered
    // off it, because a release always belongs to the press that started it --
    // that release is how a file gets moved onto another cell.
    Stacks::traceEvent(std::string("pill button ") + (pressed ? "DOWN" : "UP  ") + " at " + std::to_string(sc<int>(local.x)) + "," + std::to_string(sc<int>(local.y)));
    if (!windowDragActive && Stacks::click(mon, local, !pressed)) {
        out.claimed    = true;
        out.needsFrame = true;
        return out;
    }

    // Z2. The hover panel floats above the pill, so it is asked first -- a press
    // over a preview would otherwise reach the tile below. Behind the window-drag
    // guard: a window released over the panel belongs to the drag, not the panel.
    if (!windowDragActive && Dock::panelClick(mon, local, !pressed)) {
        out.claimed = true;
        return out;
    }

    // X32. The pill press only remembers what is under it; motion past the
    // threshold turns that into a reorder, and the RELEASE decides which of the
    // two gestures happened. Both edges are eaten -- the pill is not a window and
    // must not leak a click through to the tile beneath.
    if (!windowDragActive && Dock::pillContains(mon, local)) {
        if (pressed) {
            Dock::pressBegin(mon, local);
            Dock::dragBegin(mon, local);
        }
        else if (!Dock::dragRelease(mon, local)) {
            Stacks::traceEvent("  pill release -> Dock::click");
            Dock::click(mon, local, shiftHeld(), false, ctrlHeld()); // Y4: CTRL cycles, a plain click minimizes the app
        } else
            Stacks::traceEvent("  pill release -> taken by dragRelease (no click)");
        out.claimed = true;
        return out;
    }

    // X32. A release OFF the pill still belongs to the press that started on it,
    // so it must not read as a click on whatever the pointer has since wandered
    // over. V3 gave that release a MEANING as well: the same dragRelease decides
    // whether it launched the app on the desktop under it, on the strip card
    // under it, or missed and sent the icon home. The frame is asked for either
    // way -- a cancel's flight and a launch's blow-up both start here, and the
    // overview asks for frames its own way.
    if (!pressed && Dock::dragPending()) {
        Dock::dragRelease(mon, local);
        out.claimed    = true;
        out.needsFrame = true;
        return out;
    }

    return out;
}

}
