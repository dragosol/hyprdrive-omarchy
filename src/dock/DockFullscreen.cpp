// THE DOCK OVER A FULLSCREEN WINDOW (owner, 2026-11-12, issues #11 and #12:
// "setting for first option with a sub-setting for games and screensaver only",
// and "an option under dock context menu in get info for specific apps to not
// show dock in fulscreen, so it's buriede in Get Info").
//
// The dock keeps away -- it does not reveal, and a dock already out slides
// away with its own hide -- while the monitor's desktop shows a fullscreen
// window and
//   - that window's app is checked in its Get Info (hide-dock-fullscreen-apps),
//     whatever the two settings say; or
//   - dock:hide_over_fullscreen is on (off by default): any fullscreen window; or
//   - dock:hide_over_games is on (the default): the window is a game
//     (FullscreenGuard::isGame) or Omarchy's screensaver.
// So by default the dock keeps away from games and the screensaver and
// reveals over every other fullscreen app (owner, 2026-11-13).
// Maximized is not fullscreen; the overview's dock is its own (updateAutohide
// returns before asking). Hotkey numbers and a flight still bring it out: both
// are something the user just did.
//
// WHAT a monitor shows is settled on events -- a window going fullscreen or
// leaving it, opening, closing, moving, taking the focus, a desktop switch, a
// reload -- never per frame: the app's flag is a file stat and its identity a
// lookup. The two settings are hot reads (Config.hpp), so a live change from
// the settings panel shows on the next call.
#include "DockParts.hpp"
#include "FullscreenGuard.hpp"
#include "IdentityLive.hpp"

#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>

namespace Hyprdrive::Dock {
using namespace DockDetail;

namespace {
    // The screensaver Omarchy opens on idle (omarchy-launch-screensaver).
    constexpr const char* SCREENSAVER_CLASS = "org.omarchy.screensaver";

    struct SCover {
        PHLWINDOWREF window;
        bool         game    = false; // a game, or the screensaver
        bool         flagged = false; // its app is checked in Get Info
    };
    std::unordered_map<MONITORID, SCover> g_covers;
    uint32_t                              g_coverSeq = 0;

    PHLWINDOW fullscreenOn(PHLMONITOR monitor) {
        const auto WS = monitor ? monitor->m_activeWorkspace : nullptr;
        if (!WS)
            return nullptr;
        const auto W = ::Fullscreen::controller()->getFullscreenWindow(WS);
        if (!W || !W->m_isMapped || Minimize::isMinimized(W) || !::Fullscreen::controller()->isFullscreen(W, ::Fullscreen::FSMODE_FULLSCREEN))
            return nullptr;
        return W;
    }

    SCover coverOf(PHLMONITOR monitor) {
        const auto W = fullscreenOn(monitor);
        if (!W)
            return {};
        const bool SAVER = W->m_class == SCREENSAVER_CLASS || W->m_initialClass == SCREENSAVER_CLASS;
        const auto ID    = AppSearch::idForClass(Hyprdrive::AppIdentity::groupKey(W));
        return {.window = W, .game = SAVER || FullscreenGuard::isGame(W), .flagged = hidesDockInFullscreen(ID)};
    }

    bool hideOverAll() {
        if (!Config::g_pluginKeysRegistered)
            return false;
        static auto& VAL = Config::valueRef<Hyprlang::INT>(KEY_HIDE_OVER_FS);
        return *VAL != 0;
    }

    bool hideOverGames() {
        if (!Config::g_pluginKeysRegistered)
            return false;
        static auto& VAL = Config::valueRef<Hyprlang::INT>(KEY_HIDE_OVER_GAMES);
        return *VAL != 0;
    }
}

void noteFullscreenCover() {
    if (g_coverSeq || !g_pEventLoopManager)
        return;
    // Next turn of the loop: the event fires from inside the state change
    // (a fullscreen toggle mid-way, a map), and outside any frame.
    g_coverSeq = g_pEventLoopManager->doLater([] {
        g_coverSeq = 0;
        g_covers.clear();
        for (const auto& M : State::monitorState()->monitors()) {
            if (!M)
                continue;
            g_covers[M->m_id] = coverOf(M);
            damageIfChanged(M); // updateAutohide, and the frames its slide needs
        }
    });
}

void forgetFullscreenCovers() {
    if (g_pEventLoopManager && g_coverSeq)
        g_pEventLoopManager->removeDoLater(g_coverSeq);
    g_coverSeq = 0;
    g_covers.clear();
}

namespace DockDetail {
bool fullscreenKeepsDockAway(PHLMONITOR monitor) {
    if (!monitor)
        return false;
    const auto IT = g_covers.find(monitor->m_id);
    if (IT == g_covers.end())
        return false;
    // Still that window, still fullscreen on what this monitor shows: an
    // event that has not been settled yet must not keep the dock away from
    // a desktop that no longer has it.
    const auto W = IT->second.window.lock();
    if (!W || !W->m_isMapped || W->m_workspace != monitor->m_activeWorkspace || !::Fullscreen::controller()->isFullscreen(W, ::Fullscreen::FSMODE_FULLSCREEN))
        return false;
    if (IT->second.flagged)
        return true;
    return hideOverAll() || (IT->second.game && hideOverGames());
}
}
}
