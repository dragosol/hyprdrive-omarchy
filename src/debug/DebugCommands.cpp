#include "PluginParts.hpp"
#include "FunctionHooks.hpp"
using namespace PluginDetail;

namespace PluginDetail {
// `hyprctl dispatch 'hl.plugin.hyprdrive.debug("frames")'` -- why is each
// monitor being asked to repaint right now?
//
// Why this is permanent rather than another probe build: a monitor that
// repaints when nothing moves is the most expensive bug class in this plugin
// (measured 2026-09-15: DP-1 held near 100 fps by a settled overview), and
// every hunt for one so far has cost a stress rig, a custom build and -- twice
// -- the user's session. Reading the answer out of the live compositor costs
// nothing when nobody asks.
//
// The report comes back as `.error` so plain `hyprctl dispatch` prints it; a
// dispatcher has no other way to say anything back. It is also written to the
// Hyprland log for whoever reads that instead.
SDispatchResult onDebugDispatcher(std::string arg) {
    CConstVarList     data(arg, 0, 's', true);
    const std::string TOPIC = std::string{data[0]};
    // `activate <class>`: exactly a left click on that app's dock icon, for the
    // nested test rig (whose dock no real pointer can reach).
    if (TOPIC == "activate") {
        const std::string CLASS = data.size() > 1 ? std::string{data[1]} : std::string{};
        if (!Hyprdrive::Dock::debugActivate(Desktop::focusState()->monitor(), CLASS))
            return {.success = false, .error = std::format("hyprdrive:debug activate: no dock item for class {}", CLASS)};
        return {};
    }
    // `movehere <class>`: the dock menu's "Move to this workspace" on that
    // app's window, to the focused monitor's desktop.
    if (TOPIC == "movehere") {
        const std::string CLASS = data.size() > 1 ? std::string{data[1]} : std::string{};
        if (!Hyprdrive::Dock::debugMoveHere(Desktop::focusState()->monitor(), CLASS))
            return {.success = false, .error = std::format("hyprdrive:debug movehere: no window of class {}", CLASS)};
        return {};
    }
    // `tile <class>`: where that app's window is drawn in the focused
    // monitor's open overview (monitor-local render px), so a test clicks it
    // with a real pointer instead of guessing at a screenshot.
    if (TOPIC == "tile") {
        const std::string CLASS   = data.size() > 1 ? std::string{data[1]} : std::string{};
        const auto        MONITOR = Desktop::focusState()->monitor();
        const auto        OV      = MONITOR ? openScrollOverviewForMonitor(MONITOR) : nullptr;
        if (!OV)
            return {.success = false, .error = "hyprdrive:debug tile: no open overview"};
        for (const auto& W : Desktop::windowState()->windows()) {
            if (!W || !W->m_isMapped || W->m_class != CLASS)
                continue;
            const auto B = OV->tileBox(W);
            if (B.width < 1 || B.height < 1)
                continue;
            return {.success = false, .error = std::format("tile {} {} {} {} {}", CLASS, sc<int>(B.x), sc<int>(B.y), sc<int>(B.width), sc<int>(B.height))};
        }
        return {.success = false, .error = std::format("hyprdrive:debug tile: no tile for class {}", CLASS)};
    }
    // `hooks`: every Hyprland function the plugin hooked, in creation order
    // (FunctionHooks.hpp) -- owner, name, address, demangled signature.
    if (TOPIC == "hooks") {
        const auto  LINES  = Hyprdrive::FunctionHooks::dump();
        std::string report = std::format("hyprdrive hooks: {}", LINES.size());
        for (const auto& L : LINES)
            report += "\n" + L;
        return {.success = false, .error = report};
    }
    // `xgeom`: XGeometry's view of the focused window right now.
    if (TOPIC == "xgeom")
        return {.success = false, .error = "xgeom " + Hyprdrive::XGeometry::status()};
    // `ripple [x y]`: the hot corner's ripple on the focused monitor, from its
    // top-left corner or from global logical (x, y) -- the rig has no pointer
    // that can reach a corner.
    if (TOPIC == "ripple") {
        auto MONITOR = Desktop::focusState()->monitor();
        if (!MONITOR)
            return {.success = false, .error = "hyprdrive:debug ripple: no monitor"};
        Vector2D origin = MONITOR->logicalBox().pos();
        if (data.size() > 2) {
            try {
                origin = Vector2D{std::stod(std::string{data[1]}), std::stod(std::string{data[2]})};
            } catch (...) { return {.success = false, .error = "hyprdrive:debug ripple: x y must be numbers"}; }
            // The monitor the point is on (a corner sits on the box's edge).
            for (const auto& M : State::monitorState()->monitors())
                if (M && M->m_enabled && M->logicalBox().copy().expand(1).containsPoint(origin))
                    MONITOR = M;
        }
        Hyprdrive::Ripple::trigger(MONITOR, origin);
        return {};
    }
    // `scratchpad`: exactly a click on the overview's Scratchpad cell -- show
    // the default scratchpad, then close the overview (the test rig's trigger).
    if (TOPIC == "scratchpad") {
        const auto MONITOR = Desktop::focusState()->monitor();
        Hyprdrive::Scratchpad::show(Hyprdrive::Scratchpad::ensureDefault(MONITOR));
        if (const auto OV = scrollOverviewForMonitor(MONITOR))
            OV->close();
        return {};
    }
    // `type <text>` / `type`: set the open overview's search query as if typed
    // (empty clears it) -- the rig has no keyboard that reaches the overview.
    // `motion`: which animation:curve preset the engine is ACTUALLY using, and
    // the three curves at t = 0.25 / 0.5 -- a preset that reads as the house
    // one here is not reaching the animations.
    // `dockmenu`: the dock's hover pane cells and its open menu's row boxes.
    // `stripclean`: the overview strip's Clean Up button (box, state, bump).
    if (TOPIC == "stripclean")
        return {.success = false, .error = overviewStripCleanupReport(nullptr)};
    if (TOPIC == "dockmenu")
        return {.success = false, .error = Hyprdrive::Dock::debugMenuReport(Desktop::focusState()->monitor())};
    // `rows`: each monitor's row of desktops -- Omarchy's workspace blocks on
    // or off, the block file as read, guests and stale records, members and visitors.
    if (TOPIC == "rows")
        return {.success = false, .error = Hyprdrive::Rows::debugReport()};
    if (TOPIC == "glass") {
        const auto C = Hyprdrive::Config::dockBackgroundColor();
        return {.success = false, .error = std::format("hyprdrive glass: tint #{:02x}{:02x}{:02x} alpha {:.3f}", sc<int>(std::round(C.r * 255)), sc<int>(std::round(C.g * 255)),
                                                       sc<int>(std::round(C.b * 255)), C.a)};
    }
    if (TOPIC == "windows")
        return {.success = false, .error = Hyprdrive::SizeAck::report()};
    // `desktop`: the desktop strip -- moving or idle, its row, where it is.
    if (TOPIC == "desktop")
        return {.success = false, .error = "hyprdrive " + Hyprdrive::DesktopSwitch::debugReport()};
    if (TOPIC == "motion") {
        using Hyprdrive::Motion::ERole;
        const auto E = [](ERole r, float t) { return Hyprdrive::Motion::ease(r, t); };
        return {.success = false,
                .error   = std::format("hyprdrive motion: house={} decel(.25)={:.3f} decel(.5)={:.3f} accel(.5)={:.3f} pop(.5)={:.3f} | blur minimize {:.3f}@.25 swipe {:.3f}@.25",
                                       Hyprdrive::Motion::housePreset(), E(ERole::DECEL, 0.25F), E(ERole::DECEL, 0.5F), E(ERole::ACCEL, 0.5F), E(ERole::POP, 0.5F),
                                       Hyprdrive::FlightBlur::amount(Hyprdrive::FlightBlur::ESite::MINIMIZE, 0.25F),
                                       Hyprdrive::FlightBlur::amount(Hyprdrive::FlightBlur::ESite::SWIPE, 0.25F)) +
                       // Per blur site since the last `motion` read (which resets them):
                       // asked / smeared / drawn, trail length, shutter (FlightBlur.hpp).
                       " || " + Hyprdrive::FlightBlur::debugReport(true)};
    }
    // `mem`: what hyprdrive holds on the GPU, as far as it can count it.
    if (TOPIC == "mem") {
        const auto [N, BYTES] = OverviewWindow::texturePrefilterCacheBytes();
        std::string report    = std::format("hyprdrive mem: prefilter {} entries {:.1f} MiB", N, BYTES / 1048576.0);
        for (const auto& M : State::monitorState()->monitors())
            if (M)
                report += std::format("  | {} {}x{} full-screen RGBA8 = {:.1f} MiB", M->m_name, (int)M->m_transformedSize.x, (int)M->m_transformedSize.y,
                                      M->m_transformedSize.x * M->m_transformedSize.y * 4.0 / 1048576.0);
        report += Hyprdrive::wallpaperStashReport();
        return {.success = false, .error = report};
    }
    // `lockstate`: the lock and the overview search, for the lock gate
    // (tests/rig/headless/lock_gate.sh). The query's LENGTH, never its text.
    if (TOPIC == "lockstate")
        return {.success = false,
                .error   = std::format("hyprdrive lockstate: locked={} overviews={} accepting={} qlen={}", hyprdriveSessionLocked() ? 1 : 0, scrollOverviews().size(),
                                       overviewAcceptsSearch() ? 1 : 0, g_search.query.text.size())};
    // `gesture state`: how many overviews are open, in the native tree's
    // words (its gesture topic also plays synthetic gestures; this tree has
    // only the report), so the anim_off and overview gates can ask it.
    if (TOPIC == "gesture") {
        if (data.size() < 2 || data[1] != "state")
            return {.success = false, .error = "hyprdrive:debug gesture: only `gesture state` in this build"};
        size_t open = 0;
        for (const auto& O : scrollOverviews())
            if (O && !O->isClosing())
                ++open;
        return {.success = false, .error = std::format("hyprdrive gesture: {} overview(s) open", open)};
    }
    if (TOPIC == "type") {
        const std::string Q = data.size() > 1 ? std::string{data[1]} : std::string{};
        g_search.query.text   = Q;
        g_search.query.active = !Q.empty();
        g_search.nav.zone     = EOverviewNavZone::Search;
        g_search.nav.intent   = ESearchIntent::Typing;
        afterQueryEdit();
        for (const auto& overview : scrollOverviews())
            if (overview)
                overview->damage();
        return {};
    }
    // `press <evdev keycode>`: one key through the overview search's own press
    // handler, as if typed -- the path `type` skips. For gates (the easter
    // egg's trigger counts presses, not queries).
    if (TOPIC == "press") {
        if (data.size() < 2)
            return {.success = false, .error = "press needs an evdev keycode"};
        try {
            const auto CODE = sc<uint32_t>(std::stoul(std::string{data[1]}));
            if (Hyprdrive::KeyboardCat::active())
                Hyprdrive::KeyboardCat::handleKey(CODE);
            else
                handleSearchPress(CODE);
        } catch (...) { return {.success = false, .error = "press needs an evdev keycode"}; }
        for (const auto& overview : scrollOverviews())
            if (overview)
                overview->damage();
        return {};
    }
    if (TOPIC != "frames")
        return {.success = false, .error = "hyprdrive:debug frames|mem|activate <class>|movehere <class>|scratchpad|type [text]|press <keycode>"};

    std::string report = "hyprdrive frames:";
    for (const auto& monitor : State::monitorState()->monitors()) {
        if (!monitor)
            continue;

        report += "\n  " + monitor->m_name + std::format(" @{:.2f}Hz: ", monitor->m_refreshRate) + COverview::frameCadence(monitor->m_id);

        if (const auto OVERVIEW = scrollOverviewForMonitor(monitor)) {
            report += "\n  " + OVERVIEW->frameDebugLine();
            continue;
        }

        std::string line = "\n  " + monitor->m_name + " [desktop]";
        if (const auto DOCK = Hyprdrive::Dock::frameReason(monitor))
            line += std::string(" dock:") + DOCK;
        else
            line += " idle";
        report += line;
    }

    // Also to a file: the report is multi-line and a Lua error goes through
    // hyprctl's own formatting, so this is the copy that is always readable.
    const std::string STATEDIR = std::string{std::getenv("HOME") ? std::getenv("HOME") : "/tmp"} + "/.local/state/hyprdrive";
    std::filesystem::create_directories(STATEDIR);
    if (std::ofstream out(STATEDIR + "/frames.txt", std::ios::trunc); out.is_open())
        out << report << "\n";

    return {.success = false, .error = report};
}
} // namespace PluginDetail
