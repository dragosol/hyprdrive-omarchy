#include "PluginParts.hpp"
using namespace PluginDetail;

namespace {
// EVERY TRACKPAD GESTURE STANDS DOWN WHILE LOCKED (marketplace review, #13890).
// Hyprland runs a registered gesture whatever the session's state, so a swipe
// at the lock screen closed windows, switched desktops and ran dispatchers.
// A gesture that begins while locked is dropped whole: its update and end
// never reach the gesture inside, so nothing is left half-begun.
class CLockedOutGesture : public ITrackpadGesture {
  public:
    explicit CLockedOutGesture(UP<ITrackpadGesture>&& inner) : m_inner(std::move(inner)) {}

    void begin(const STrackpadGestureBegin& e) override {
        m_live = !hyprdriveSessionLocked();
        if (m_live)
            m_inner->begin(e);
    }
    void update(const STrackpadGestureUpdate& e) override {
        if (m_live)
            m_inner->update(e);
    }
    void end(const STrackpadGestureEnd& e) override {
        if (m_live)
            m_inner->end(e);
        m_live = false;
    }
    bool isDirectionSensitive() override {
        return m_inner->isDirectionSensitive();
    }

  private:
    UP<ITrackpadGesture> m_inner;
    bool                 m_live = false;
};
}

namespace PluginDetail {
// Register / unregister a trackpad gesture. Used by the hyprlang keyword, the
// Lua `hyprdrive.gesture` function, and the `hyprdrive:gesture` dispatcher.
std::expected<void, std::string> applyTrackpadGesture(size_t fingerCount, eTrackpadGestureDirection direction, const std::string& action, uint32_t modMask, float deltaScale,
                                                             bool disableInhibit, const std::string& arg) {
    if (fingerCount <= 1 || fingerCount >= 10)
        return std::unexpected(std::format("Invalid value {} for finger count", fingerCount));

    if (direction == TRACKPAD_GESTURE_DIR_NONE)
        return std::unexpected("Invalid direction");

    const auto add = [&](UP<ITrackpadGesture>&& gesture) {
        return g_pTrackpadGestures->addGesture(makeUnique<CLockedOutGesture>(std::move(gesture)), fingerCount, direction, modMask, deltaScale, disableInhibit);
    };

    // The desktop swipe's Super twin (below) goes with it.
    const bool SUPERTWIN = modMask == 0 && (direction == TRACKPAD_GESTURE_DIR_HORIZONTAL || direction == TRACKPAD_GESTURE_DIR_SWIPE);

    if (action == "unset") {
        if (SUPERTWIN)
            (void)g_pTrackpadGestures->removeGesture(fingerCount, TRACKPAD_GESTURE_DIR_HORIZONTAL, HL_MODIFIER_META, deltaScale, disableInhibit);
        return g_pTrackpadGestures->removeGesture(fingerCount, direction, modMask, deltaScale, disableInhibit);
    }

    if (action == "overview")
        return add(makeUnique<COverviewGesture>(direction));

    if (action == "canvas_zoom")
        return add(makeUnique<CCanvasZoomGesture>(direction));

    if (action == "workspace") {
        const auto RES = add(makeUnique<COverviewWorkspaceGesture>());
        // SUPER TOO (user, 2026-11-01: "I want for super+3 finger swipe to ALSO
        // do that ... when I'm zoomed out in infinite canvas" -- the zoomed-out
        // canvas is Super held, and Hyprland matches a gesture's modifiers
        // exactly). The same swipe with Super down switches desktops too,
        // unless the config binds that one itself (then this add is refused).
        if (RES && SUPERTWIN)
            (void)g_pTrackpadGestures->addGesture(makeUnique<CLockedOutGesture>(makeUnique<COverviewWorkspaceGesture>()), fingerCount, TRACKPAD_GESTURE_DIR_HORIZONTAL, HL_MODIFIER_META, deltaScale,
                                                  disableInhibit);
        return RES;
    }

    if (action == "workspace_all")
        return add(makeUnique<COverviewWorkspaceGesture>(true));

    if (action == "scroll")
        return add(makeUnique<COverviewScrollGesture>());

    if (action == "scratchpad_show" || action == "scratchpad_hide")
        return add(makeUnique<CScratchpadGesture>(action == "scratchpad_show"));

    if (action == "fullscreen_on" || action == "unfullscreen_or_close")
        return add(makeUnique<CFullscreenOrCloseGesture>(action == "fullscreen_on"));

    if (action == "close")
        return add(makeUnique<CCloseTrackpadGesture>());

    if (action == "fullscreen" || action == "maximize")
        return add(makeUnique<CFullscreenTrackpadGesture>(action == "maximize" ? "maximize" : (arg.empty() ? "fullscreen" : arg)));

    if (action == "float" || action == "tile")
        return add(makeUnique<CFloatTrackpadGesture>(action == "tile" ? "tile" : (arg.empty() ? "toggle" : arg)));

    if (action == "move")
        return add(makeUnique<CMoveTrackpadGesture>());

    if (action == "resize")
        return add(makeUnique<CResizeTrackpadGesture>());

    if (action == "special")
        return add(makeUnique<CSpecialWorkspaceGesture>(arg.empty() ? "special" : arg));

    if (action == "zoom") {
        std::string level = "2";
        std::string mode  = arg.empty() ? "live" : arg;
        if (const auto SP = mode.find(' '); SP != std::string::npos) {
            level = mode.substr(0, SP);
            mode  = mode.substr(SP + 1);
        } else if (mode != "live" && mode != "toggle" && mode != "mult") {
            level = mode;
            mode  = "live";
        }
        return add(makeUnique<CCursorZoomTrackpadGesture>(level, mode));
    }

    if (action == "dispatcher" || action == "dispatch") {
        if (arg.empty())
            return std::unexpected("dispatcher gesture needs an arg, e.g. exec,notify-send hi");
        std::string dispatcher = arg;
        std::string data;
        const auto  SEP = arg.find_first_of(", ");
        if (SEP != std::string::npos) {
            dispatcher = arg.substr(0, SEP);
            data       = arg.substr(SEP + 1);
            while (!data.empty() && (data.front() == ' ' || data.front() == ','))
                data.erase(data.begin());
        }
        return add(makeUnique<CDispatcherTrackpadGesture>(dispatcher, data));
    }

    if (action == "keys" || action == "key" || action == "shortcut" || action == "chord") {
        if (arg.empty())
            return std::unexpected("keys gesture needs a chord, e.g. CTRL+T or Print");
        return add(makeUnique<CKeysTrackpadGesture>(arg));
    }

    return std::unexpected(std::format("Invalid gesture: {}", action));
}

// Lua-facing registrar (hyprdrive.gesture). takes the direction/mods as strings and resolves
// them the same way the keyword does, then defers to applyTrackpadGesture.
SDispatchResult onRegisterOverviewGesture(size_t fingerCount, const std::string& directionStr, const std::string& action, const std::string& mods, float deltaScale,
                                                 bool disableInhibit, const std::string& arg) {
    if (g_unloading)
        return {};

    std::string directionNorm = directionStr;
    if (directionNorm == "pinch_in" || directionNorm == "pinch-in")
        directionNorm = "pinchin";
    else if (directionNorm == "pinch_out" || directionNorm == "pinch-out")
        directionNorm = "pinchout";

    uint32_t clickMods = 0;
    if (!mods.empty() && g_pKeybindManager)
        clickMods = g_pKeybindManager->stringToModMask(mods);

    if (directionNorm == "click" || directionNorm == "tap") {
        if (action == "unset")
            removeClickGesture(fingerCount, clickMods);
        else
            addClickGesture(fingerCount, action, clickMods, disableInhibit, arg);
        return {};
    }

    const auto direction = g_pTrackpadGestures->dirForString(directionNorm);
    if (direction == TRACKPAD_GESTURE_DIR_NONE)
        return {.success = false, .error = std::format("Invalid direction: {}", directionStr)};

    const uint32_t modMask = clickMods;

    const auto res = applyTrackpadGesture(fingerCount, direction, action, modMask, std::clamp(deltaScale, 0.1F, 10.F), disableInhibit, arg);
    if (!res)
        return {.success = false, .error = res.error()};

    return {};
}

Hyprlang::CParseResult overviewGestureKeyword(const char* LHS, const char* RHS) {
    Hyprlang::CParseResult result;

    if (g_unloading)
        return result;

    CConstVarList             data(RHS);

    size_t                    fingerCount = 0;

    try {
        fingerCount = std::stoul(std::string{data[0]});
    } catch (...) {
        result.setError(std::format("Invalid value {} for finger count", data[0]).c_str());
        return result;
    }

    const std::string DIRSTR = std::string{data[1]};
    const bool        ISCLICK = DIRSTR == "click" || DIRSTR == "tap";
    const auto direction = ISCLICK ? TRACKPAD_GESTURE_DIR_NONE : g_pTrackpadGestures->dirForString(data[1]);

    if (!ISCLICK && direction == TRACKPAD_GESTURE_DIR_NONE) {
        result.setError(std::format("Invalid direction: {}", data[1]).c_str());
        return result;
    }

    int      startDataIdx   = 2;
    uint32_t modMask        = 0;
    float    deltaScale     = 1.F;
    bool     disableInhibit = false;

    while (true) {

        if (data[startDataIdx].starts_with("mod:")) {
            modMask = g_pKeybindManager->stringToModMask(std::string{data[startDataIdx].substr(4)});
            startDataIdx++;
            continue;
        } else if (data[startDataIdx].starts_with("scale:")) {
            try {
                deltaScale = std::clamp(std::stof(std::string{data[startDataIdx].substr(6)}), 0.1F, 10.F);
                startDataIdx++;
                continue;
            } catch (...) {
                result.setError(std::format("Invalid delta scale: {}", std::string{data[startDataIdx].substr(6)}).c_str());
                return result;
            }
        } else if (data[startDataIdx] == "disable_inhibit") {
            disableInhibit = true;
            startDataIdx++;
            continue;
        }

        break;
    }

    const std::string ACTION = std::string{data[startDataIdx]};
    std::string       ARG;
    for (int i = startDataIdx + 1; !data[i].empty(); ++i) {
        if (!ARG.empty())
            ARG += ",";
        ARG += std::string{data[i]};
    }

    if (ISCLICK) {
        if (ACTION == "unset")
            removeClickGesture(fingerCount, modMask);
        else
            addClickGesture(fingerCount, ACTION, modMask, disableInhibit, ARG);
        return result;
    }

    const auto resultFromGesture = applyTrackpadGesture(fingerCount, direction, ACTION, modMask, deltaScale, disableInhibit, ARG);

    if (!resultFromGesture)
        result.setError(resultFromGesture.error().c_str());

    return result;
}
} // namespace PluginDetail
