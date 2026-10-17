#pragma once

#include "globals.hpp"
#include <hyprland/src/helpers/time/Time.hpp>

#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/config/shared/complex/ComplexDataTypes.hpp>

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>

namespace ScrollOverview::Config {

using TDispatcher       = SDispatchResult (*)(std::string);
using TGestureKeyword   = Hyprlang::CParseResult (*)(const char* LHS, const char* RHS);
using TGestureRegistrar   = SDispatchResult (*)(size_t fingerCount, const std::string& direction, const std::string& action, const std::string& mods, float deltaScale,
                                                bool disableInhibit, const std::string& arg);

enum class ELayout {
    VERTICAL,
    HORIZONTAL,
};

enum class EScrollAction {
    WORKSPACE,
    COLUMN,
};

void registerDispatcher(const std::string& name, TDispatcher dispatcher);
void registerGesture(TGestureRegistrar gestureRegistrar, TGestureKeyword gestureKeyword);
void registerConfig();

// Keys are spelled as 25-45 character literals at every call site, and the map
// below is the one thing between a call site and the value. Taking the name as
// a std::string built a heap allocation per LOOKUP -- several hundred a frame
// per monitor, all of them for a string that already exists in .rodata. A
// transparent (heterogeneous) hash and equality let find() hash the characters
// where they lie; the only std::string that survives is the one a key is first
// stored under.
struct SKeyHash {
    using is_transparent = void;

    size_t operator()(const std::string_view key) const noexcept {
        return std::hash<std::string_view>{}(key);
    }
};

struct SKeyEqual {
    using is_transparent = void;

    bool operator()(const std::string_view a, const std::string_view b) const noexcept {
        return a == b;
    }
};

// CConfigValue is Hyprland's own caching handle: it holds the pointer-TO-pointer
// the config manager owns, so one of these survives a reload and costs a double
// indirection to read. They are created once per key and never erased, and the
// map stores them by pointer, so the reference handed back stays valid across a
// rehash -- which is what makes
//
//     static auto& VAL = valueRef<Hyprlang::INT>("plugin:hyprdrive:...");
//
// legal at a call site, and is how the per-frame helpers in Config.cpp skip even
// the hash. Do NOT cache what a handle POINTS AT that way: that pointer moves.
template <typename T>
CConfigValue<T>& valueRef(const std::string_view name) {
    static std::unordered_map<std::string, UP<CConfigValue<T>>, SKeyHash, SKeyEqual> values;

    if (const auto IT = values.find(name); IT != values.end())
        return *IT->second;

    const auto [IT, INSERTED] = values.try_emplace(std::string{name});
    IT->second                = makeUnique<CConfigValue<T>>(IT->first);

    return *IT->second;
}

// Set once registerConfigValues() has registered every plugin key. Before
// that, binding a plugin: key ABORTS Hyprland (CConfigValue::bind) -- and a
// plugin loaded at startup then aborts every login: 2026-09-29, a read of
// plugin:hyprdrive:smooth_frames in PLUGIN_INIT crash-looped the session. So an
// early read gets the type's empty value (and says so) instead of a crash.
inline bool g_pluginKeysRegistered = false;

inline bool earlyPluginKey(const std::string_view name) {
    if (g_pluginKeysRegistered || !name.starts_with("plugin:"))
        return false;
    std::fprintf(stderr, "[hyprdrive] BUG: config key %.*s read before registration\n", sc<int>(name.size()), name.data());
    return true;
}

template <typename T>
T getValue(const std::string_view name) {
    using TValue = std::decay_t<T>;

    if (earlyPluginKey(name))
        return TValue{};

	if constexpr (std::is_same_v<TValue, ::Config::CGradientValueData>) {
        auto& ref = valueRef<::Config::IComplexConfigValue>(name);
        if (!ref.good())
            return {};
        return *sc<::Config::CGradientValueData*>(ref.ptr());
    }

    if constexpr (std::is_same_v<TValue, bool>)
        return *valueRef<Hyprlang::INT>(name) != 0;
    else if constexpr (std::is_integral_v<TValue> && !std::is_same_v<TValue, bool>)
        return sc<TValue>(*valueRef<Hyprlang::INT>(name));
    else if constexpr (std::is_floating_point_v<TValue>)
        return sc<TValue>(*valueRef<Hyprlang::FLOAT>(name));
    else
        return *valueRef<TValue>(name);
}

// The live gradient, NOT a copy. CGradientValueData owns a colour vector, so
// getValue<CGradientValueData> is a heap allocation every call -- and the
// overview asks for the highlight and the shadow colour once per card and once
// per strip cell, per frame. nullptr when the key is not registered.
//
// The returned pointer is derived from the config manager on each call and must
// not outlive the expression that asked for it: a reload moves the value. Cache
// the HANDLE (the valueRef overload) at a call site, never this pointer.
inline const ::Config::CGradientValueData* gradientPtr(CConfigValue<::Config::IComplexConfigValue>& ref) {
    if (!ref.good())
        return nullptr;

    auto* const PTR = ref.ptr();
    return PTR ? sc<::Config::CGradientValueData*>(PTR) : nullptr;
}

inline const ::Config::CGradientValueData* gradientPtr(const std::string_view name) {
    return gradientPtr(valueRef<::Config::IComplexConfigValue>(name));
}

template <typename T>
T* getValuePtr(const std::string_view name) {
    return valueRef<T>(name).ptr();
}

template <typename T>
void setValue(const std::string_view name, const T& value) {
    using TValue = std::decay_t<T>;

    if (earlyPluginKey(name))
        return;

    if constexpr (std::is_same_v<TValue, bool>)
        *getValuePtr<Hyprlang::INT>(name) = value ? 1 : 0;
    else if constexpr (std::is_integral_v<TValue> && !std::is_same_v<TValue, bool>)
        *getValuePtr<Hyprlang::INT>(name) = sc<Hyprlang::INT>(value);
    else if constexpr (std::is_floating_point_v<TValue>)
        *getValuePtr<Hyprlang::FLOAT>(name) = sc<Hyprlang::FLOAT>(value);
    else
        *getValuePtr<TValue>(name) = value;
}

int           getGestureDistance();
float         getGestureDetent();
bool          getGestureDetentLabel();
std::string   getGestureDetentSound();

// Appearance is Hyprland's. Plugin keys that are unset (alpha 0 / -1) read the
// compositor's live values, so a theme that reloads Hyprland retints every
// surface. This is the one place those reads live; call sites do not parse a
// second palette.
//
//   highlightColor()        accent: col_highlight, else general:col.active_border
//   themeRounding()         decoration:rounding (0 is squared, and is valid)
//   themeRoundingPower()    decoration:rounding_power
//   dockIndicatorColor()    dock:indicator_color, else highlightColor()
//   dockBackgroundColor()   the terminal's glass: dock:background_color, else
//                           Ghostty's background (its config + includes +
//                           theme), else Omarchy colors.toml, else
//                           misc:background_color; opacity: dock:background_opacity,
//                           else Ghostty's background-opacity, else 0.85 x
//                           decoration:inactive_opacity
//   themeFontFamily()       misc:font_family if pinned, else Omarchy
//                           fontconfig monospace (omarchy font set)
//   dockCornerRadius()      dock:corner_radius (default 24; the pill is not a window)
//   dock blur/shadow -1     follow decoration:blur:* / decoration:shadow:*
//   search / results / strip  same plate as the dock (dockBackgroundColor,
//                             highlightColor border, frost if blur:enabled);
//                             rounding is themeRounding, not dockCornerRadius
//
// Pin a plugin key only to stop inheriting.
CHyprColor    highlightColor();
CHyprColor    restingBorderColor(); // general:col.inactive_border -- a tile at rest
CHyprColor    highlightForeground();
int           themeRounding();
float         themeRoundingPower();
const std::string& themeFontFamily();
CHyprColor    dockIndicatorColor();
CHyprColor    dockBackgroundColor();
int           dockCornerRadius();
bool          isLightTheme();
CHyprColor    themeForeground();
CHyprColor    themeMutedText();
CHyprColor    themeAccent();
CHyprColor    themeHeading();
CHyprColor    themeSelection();
CHyprColor    themeSelectedFill();
CHyprColor    themeSelectedText();
CHyprColor    themeUrgent();

// Everything above that reads a FILE -- the Omarchy palette (colors.toml) and
// the fontconfig monospace alias -- is cached behind a 500ms re-stat, because
// those sit under per-card colour reads and a stat() per colour per card per
// frame is thousands of syscalls a second. The poll alone would show a theme
// change up to half a second late; it never does, because `omarchy theme set`
// reloads Hyprland and the reload listener calls this first. The poll only
// covers a file edited underneath a theme that is already current.
void          invalidateThemeCache();
// The number invalidateThemeCache() bumps, for a cache that lives outside
// Config.cpp (BorderFade's `border` curve) to re-read on the same reload.
uint64_t      themeGeneration();

// A hash over the LIVE values of every plugin:hyprdrive:dock: key. Taken on
// config.preReload and again on config.reloaded, it answers the only question
// that reload listener actually has: did this reload move the dock's layout?
//
// Enumerated from the registration list, not from a hand-written key list -- a
// list is a thing to forget, and a dock key added later and left out of it
// would let a minimize flight chase an icon the new layout has moved. A key
// whose type is not one of the five the dock uses mixes in a counter instead,
// so the fingerprint cannot compare equal and the caller falls back to what it
// did before this existed.
uint64_t      dockKeyFingerprint();

float         getScale();
float         getOpenSpeed();
float         getCloseSpeed();
int           getWorkspaceGap();
ELayout       getLayout();
int           getScrollEventDelay();
bool          getLeftHanded();
int           getDragMode();
int           getDragThreshold();

// GLOBAL ANIMATION SPEED (user, 2026-08-06). One multiplier over every timed
// thing in the plugin. 1.0 is the shipped feel; 10.0 runs everything at a tenth
// speed so a single frame can be looked at properly. 0 snaps: every in-flight
// ease completes on the next sample (Hyprdrive motion toggle).
//
// Deliberately applied at the ONE place elapsed time is measured, not by editing
// durations: every constant in the codebase keeps its real, meaningful value
// (GNOME's 400ms minimize stays the number 400 and can still be checked against
// the host), and nothing has to be un-edited afterwards.
float         animScale();
bool          motionEnabled();

// Wall-clock ms. Input timing (double-click, drag-hold) must not ride animScale
// -- at 0 that clock jumps to infinity and would swallow clicks.
float         realElapsedMs(const Time::steady_tp& since);

// Milliseconds since `since`, DIVIDED by animScale -- so a clock that runs slow
// makes every duration comparison downstream stretch, with no other changes.
// Scale 0 returns a huge elapsed so every duration comparison is already done.
float         animElapsedMs(const Time::steady_tp& since);
float         getDragMiniScale();
int           getDragMiniMs();
int           getDragRecentreMs();
float         getTouchpadScrollFactor();
EScrollAction getVerticalScrollAction(ELayout layout);
EScrollAction getHorizontalScrollAction(ELayout layout);
int           getWallpaperMode();
bool          getBlur();
std::string   getTerminal();
bool          getMissionControl();
int           getMissionControlGap();
int           getMissionControlReflowMs();
bool          getStrip();
int           getStripHeight();
int           getStripMaxScreenPct();
bool          getSearchDictionary();
bool          getSearchTranslate();
std::string   getSearchTranslateEndpoint();
std::string   getSearchTranslateTarget();
int           getStripGap();
bool          getTapSuper();
int           getTapSuperTimeoutMs();
::Config::CCssGapData getCssGapData(const std::string& name);
int          getShadowEnabled();
int          getShadowRange();
int          getShadowRenderPower();
const ::Config::CGradientValueData* getShadowColor();

}
