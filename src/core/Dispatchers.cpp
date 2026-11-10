#include "PluginParts.hpp"
using namespace PluginDetail;

namespace PluginDetail {
SDispatchResult onNavigateDispatcher(std::string arg) {
    const auto OVERVIEW = activeScrollOverview();
    if (!OVERVIEW)
        return {};

    if (arg != "left" && arg != "right" && arg != "up" && arg != "down")
        return {.success = false, .error = "invalid arg. expected left|right|up|down"};

    OVERVIEW->moveSelection(arg);
    return {};
}

SDispatchResult onWindowDispatcher(std::string arg) {
    const auto OVERVIEW = activeScrollOverview();
    if (!OVERVIEW)
        return {};

    if (arg == "click") {
        // Only reached when onMouseButton let the press through, i.e. over a
        // TOP/OVERLAY layer: the press is that layer's (the bind is
        // non-consuming), so the overview must not act on it too.
        if (OvInternal::pointerOnTopLayer(Desktop::focusState()->monitor()))
            return {};
        OVERVIEW->performPrimaryClickAction();
        return {};
    }

    if (arg != "select" && arg != "close")
        return {.success = false, .error = "invalid arg. expected select|close|click"};

    OVERVIEW->windowDispatcherAction(arg);
    return {};
}

SDispatchResult onGestureDispatcher(std::string arg) {
    CConstVarList data(arg, 0, 's', true);
    const std::string CMD = std::string{data[0]};
    if (CMD.empty() || CMD == "help") {
        return {.success = false,
                .error   = "hyprdrive:gesture add|remove <fingers> <direction> <action> [arg] [mod:KEYS] [scale:N] [disable_inhibit]"};
    }

    const bool REMOVE = CMD == "remove" || CMD == "unset" || CMD == "del";
    const bool ADD    = CMD == "add" || CMD == "set";
    if (!ADD && !REMOVE)
        return {.success = false, .error = std::format("hyprdrive:gesture: unknown command {}", CMD)};

    size_t fingerCount = 0;
    try {
        fingerCount = std::stoul(std::string{data[1]});
    } catch (...) {
        return {.success = false, .error = "hyprdrive:gesture: fingers must be an integer"};
    }

    const std::string DIRSTR = std::string{data[2]};
    const bool        ISCLICK = DIRSTR == "click" || DIRSTR == "tap";
    const auto direction = ISCLICK ? TRACKPAD_GESTURE_DIR_NONE : g_pTrackpadGestures->dirForString(data[2]);
    if (!ISCLICK && direction == TRACKPAD_GESTURE_DIR_NONE)
        return {.success = false, .error = std::format("Invalid direction: {}", DIRSTR)};

    int      idx            = 3;
    uint32_t modMask        = 0;
    float    deltaScale     = 1.F;
    bool     disableInhibit = false;
    while (!data[idx].empty()) {
        const auto TOK = std::string{data[idx]};
        if (TOK.starts_with("mod:")) {
            modMask = g_pKeybindManager->stringToModMask(TOK.substr(4));
            idx++;
            continue;
        }
        if (TOK.starts_with("scale:")) {
            try {
                deltaScale = std::clamp(std::stof(TOK.substr(6)), 0.1F, 10.F);
            } catch (...) {
                return {.success = false, .error = "invalid scale"};
            }
            idx++;
            continue;
        }
        if (TOK == "disable_inhibit") {
            disableInhibit = true;
            idx++;
            continue;
        }
        break;
    }

    const std::string ACTION = REMOVE ? "unset" : std::string{data[idx]};
    std::string       extra;
    if (!REMOVE) {
        for (int i = idx + 1; !data[i].empty(); ++i) {
            const auto TOK = std::string{data[i]};
            if (TOK.starts_with("mod:") || TOK.starts_with("scale:") || TOK == "disable_inhibit")
                continue;
            if (!extra.empty())
                extra += " ";
            extra += TOK;
        }
    }

    if (ISCLICK) {
        if (REMOVE)
            removeClickGesture(fingerCount, modMask);
        else
            addClickGesture(fingerCount, ACTION, modMask, disableInhibit, extra);
        return {};
    }

    const auto res = applyTrackpadGesture(fingerCount, direction, ACTION, modMask, deltaScale, disableInhibit, extra);
    if (!res)
        return {.success = false, .error = res.error()};
    return {};
}

// OVERVIEW_CPP_SPEC §13.9 X24. hyprgnomedock does not exist yet, so this is how
// the minimize engine is driven and tested until its icons can call it.
SDispatchResult onMinimizeDispatcher(std::string arg) {
    if (arg == "restore") {
        // Nothing minimized is nothing to do, not an error: a bind's error is
        // Lua's "runtime error" toast.
        const auto MINIMIZED = ScrollOverview::Minimize::minimized();
        if (MINIMIZED.empty())
            return {};

        // X54: back out through the DOCK, so it flies from its icon instead of
        // reappearing. The dock owns the animation because it owns the target.
        ScrollOverview::Dock::restoreWithFlight(Desktop::focusState()->monitor(), MINIMIZED.back());
        return {};
    }

    // SUPER+H AGAIN (r/omarchy, 2026-10-24: "if I press Super+H again while in
    // the same workspace, I get a runtime error in Lua. It would be fantastic if
    // we could restore a minimized window to its original position using
    // Win+H"). With no window of this desktop focused -- the one just minimized
    // took the focus with it -- the same chord brings back the window most
    // recently minimized FROM this desktop, flying out of the dock to where it
    // was. Windows minimized from other desktops stay in the dock, and with
    // nothing to minimize or restore the press does nothing, quietly: the old
    // "no focused window" error was the Lua runtime error.
    const auto MONITOR = Desktop::focusState()->monitor();
    const auto WINDOW  = Desktop::focusState()->window();
    if (WINDOW && WINDOW->m_workspace && !ScrollOverview::Minimize::isStash(WINDOW->m_workspace)) {
        ScrollOverview::Dock::minimizeWithFlight(MONITOR, WINDOW);
        return {};
    }

    const auto HERE = MONITOR ? (MONITOR->m_activeSpecialWorkspace ? MONITOR->m_activeSpecialWorkspace : MONITOR->m_activeWorkspace) : PHLWORKSPACE{};
    if (!HERE)
        return {};
    const auto MINIMIZED = ScrollOverview::Minimize::minimized();
    for (auto it = MINIMIZED.rbegin(); it != MINIMIZED.rend(); ++it)
        if (*it && ScrollOverview::Minimize::originalWorkspace(*it) == HERE->m_id) {
            ScrollOverview::Dock::restoreWithFlight(MONITOR, *it);
            return {};
        }
    return {};
}

// OVERVIEW_CPP_SPEC §13.9 X34. All thirty accelerators and `<Super>q` land
// here; the compositor's `bind` lines carry which is which.
SDispatchResult onHotkeyDispatcher(std::string arg) {
    if (arg == "overlay") {
        ScrollOverview::Dock::showOverlay();
        return {};
    }

    bool shift = false;
    bool ctrl  = false;
    if (arg.starts_with("shift ")) {
        shift = true;
        arg   = arg.substr(6);
    } else if (arg.starts_with("ctrl ")) {
        ctrl = true;
        arg  = arg.substr(5);
    }

    ScrollOverview::Dock::hotkey(std::stoi(arg), shift, ctrl);
    return {};
}

// V11 SPEC V4. THE STACK DISPATCHER, and the reason there is one at all.
//
// A stack is meant to be created by dragging a file out of a file manager and
// onto the dock. That gesture cannot reach this plugin: in Hyprland 0.56 the
// drop target of an XDG drag is always a wl_surface (CSeatManager's
// dndPointerFocus is a WP<CWLSurfaceResource>) and this dock is not a surface
// -- it is drawn straight into the render pass -- while every member that
// would let a non-surface claim the offer is private on CWLDataDeviceProtocol
// with `friend` limited to CSeatManager. The full finding is in DockStacks.hpp.
//
// So the pin is a DISPATCHER, which is the same shape every other scriptable
// thing in this plugin has, and a Nautilus script wraps it for the file
// manager's own right-click menu (scripts/install-nautilus-pin.sh).
//
//     hyprdrive:stack pin /home/you/Downloads
//     hyprdrive:stack unpin /home/you/Downloads
//     hyprdrive:stack toggle /home/you/Downloads     -- pin if not pinned, else unpin
//     hyprdrive:stack view grid /home/you/Downloads
//     hyprdrive:stack sort mtime /home/you/Downloads
//     hyprdrive:stack clear                          -- every stack, for gate_slate
//
// A path is taken RAW to the end of the line: paths contain spaces, and any
// splitting cleverer than "the verb, then the rest" would refuse the ones that
// need this most.
SDispatchResult onStackDispatcher(std::string arg) {
    const auto CUT  = arg.find(' ');
    const auto VERB = arg.substr(0, CUT);
    auto       rest = CUT == std::string::npos ? std::string{} : arg.substr(CUT + 1);
    while (!rest.empty() && rest.front() == ' ')
        rest.erase(rest.begin());

    if (VERB == "clear") {
        ScrollOverview::Stacks::clearAll();
        return {};
    }

    // `open <path>`: open (or, if it is open, fold) that stack's fan or pane on
    // the focused monitor's dock, as a click on its cell would -- for keybinds,
    // and for testing without touching the pointer.
    if (VERB == "open") {
        const auto MONITOR = Desktop::focusState()->monitor();
        if (!MONITOR)
            return {.success = false, .error = "no focused monitor"};
        std::string want = rest;
        while (want.size() > 1 && want.back() == '/')
            want.pop_back();
        for (const auto& ITEM : ScrollOverview::Dock::layout(MONITOR).items) {
            if (ITEM.stack && !ITEM.placeholder && ITEM.stackPath == want) {
                ScrollOverview::Stacks::open(MONITOR, ITEM, ITEM.iconBox);
                ScrollOverview::Dock::damageIfChanged(MONITOR);
                g_pHyprRenderer->damageMonitor(MONITOR);
                return {};
            }
        }
        return {.success = false, .error = "no such stack on this dock"};
    }

    if (VERB == "scroll") {
        try {
            if (!ScrollOverview::Stacks::scrollBy(std::stof(rest)))
                return {.success = false, .error = "no fan is open"};
        } catch (...) { return {.success = false, .error = "expected a number of rows"}; }
        return {};
    }
    if (VERB == "filter") {
        if (!ScrollOverview::Stacks::setQuery(rest))
            return {.success = false, .error = "no stack is open"};
        return {};
    }

    if (VERB == "view" || VERB == "sort") {
        const auto CUT2 = rest.find(' ');
        if (CUT2 == std::string::npos)
            return {.success = false, .error = "expected a mode and a path"};

        const auto MODE = rest.substr(0, CUT2);
        auto       path = rest.substr(CUT2 + 1);
        while (!path.empty() && path.front() == ' ')
            path.erase(path.begin());

        if (VERB == "view") {
            if (MODE != "fan" && MODE != "grid" && MODE != "auto")
                return {.success = false, .error = "expected auto, fan or grid"};
            ScrollOverview::Stacks::setViewMode(path, ScrollOverview::StackModel::viewFromName(MODE));
            return {};
        }

        const auto SORT = MODE == "mtime" ? std::optional{ScrollOverview::StackModel::ESort::Modified} : ScrollOverview::StackModel::sortFromName(MODE);
        if (!SORT)
            return {.success = false, .error = "expected name, added, modified (mtime) or kind"};
        ScrollOverview::Stacks::setSort(path, *SORT);
        return {};
    }

    if (rest.empty())
        return {.success = false, .error = "expected a path"};

    if (VERB == "pin") {
        if (!ScrollOverview::Stacks::pin(rest))
            return {.success = false, .error = "not pinned: the path does not exist, or is already a stack"};
        return {};
    }

    if (VERB == "unpin") {
        if (!ScrollOverview::Stacks::unpin(rest))
            return {.success = false, .error = "not a stack"};
        return {};
    }

    if (VERB == "toggle") {
        if (!ScrollOverview::Stacks::unpin(rest) && !ScrollOverview::Stacks::pin(rest))
            return {.success = false, .error = "the path does not exist"};
        return {};
    }

    return {.success = false, .error = "expected pin, unpin, toggle, open, view, sort, scroll, filter or clear"};
}
} // namespace PluginDetail
