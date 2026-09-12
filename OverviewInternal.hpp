#pragma once

// Internal helpers shared by the scrollOverview translation units. Not installed,
// not an API -- this is the ~1900 lines of file-static helpers that used to sit on
// top of scrollOverview.cpp and pinned every method to that one file.
//
// ponytail: header-only, and it re-exports itself with a trailing using-directive so
// the moved code needed no edits. The namespace is not decoration -- these compile to
// exported symbols now, and `hyprlerp` / `axisValue` would otherwise collide with
// Hyprland's own. It is deliberately NOT nested under ScrollOverview:: -- that made
// unqualified Config:: resolve to ScrollOverview::Config. Split into real .cpp files
// when compile time starts to hurt.

#include "scrollOverview.hpp"

#include <cctype>
#include <cstdio>

#include "AppSearch.hpp"
#include "OmarchySearch.hpp"
#include "SearchSession.hpp"
#include "TileChrome.hpp"
#include "WindowPill.hpp"

namespace TileChrome = ScrollOverview::TileChrome;
namespace WindowPill = ScrollOverview::WindowPill;

#include <drm_fourcc.h>
#include <hyprgraphics/image/Image.hpp>
#include <hyprland/src/render/gl/GLTexture.hpp>
#include <unordered_set>
#include <algorithm>
#include <any>
#include <array>
#include <chrono>
#include <dlfcn.h>
#include <functional>
#include <limits>
#include <optional>
#include <unordered_map>
#include <linux/input-event-codes.h>
#include <state/WorkspaceState.hpp>
#define private public
#define protected public
#include <hyprland/src/render/gl/GLTexture.hpp>
#include <drm_fourcc.h>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/config/ConfigValue.hpp>
#include <hyprland/src/config/ConfigManager.hpp>
#include <hyprland/src/config/lua/ConfigManager.hpp>
#include <hyprland/src/config/shared/actions/ConfigActions.hpp>
#include <hyprland/src/config/shared/animation/AnimationTree.hpp>
#include <hyprland/src/config/shared/complex/ComplexDataTypes.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/animation/AnimationManager.hpp>
#include <hyprland/src/managers/EventManager.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/managers/KeybindManager.hpp>
#include <hyprland/src/managers/SeatManager.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/layout/algorithm/Algorithm.hpp>
#include <hyprland/src/layout/space/Space.hpp>
#include <hyprland/src/layout/target/Target.hpp>
#include <hyprland/src/layout/algorithm/tiled/scrolling/ScrollingAlgorithm.hpp>
#include <hyprland/src/desktop/state/GlobalWindowController.hpp>
#include <hyprland/src/pointer/cursor/CursorShapeOverrideController.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/view/Group.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/desktop/view/WLSurface.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/desktop/view/Popup.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/state/ViewState.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/protocols/LayerShell.hpp>
#include <hyprland/src/devices/IKeyboard.hpp>
#include <hyprland/src/helpers/math/Math.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/plugins/PluginSystem.hpp>
#include <hyprland/src/render/pass/BorderPassElement.hpp>
#include <hyprland/src/render/pass/Pass.hpp>
#include <hyprland/src/render/pass/ClearPassElement.hpp>
#include <hyprland/src/render/pass/PreBlurElement.hpp>
#include <hyprland/src/render/pass/RectPassElement.hpp>
#include <hyprland/src/render/pass/RendererHintsPassElement.hpp>
#include <hyprland/src/render/pass/SurfacePassElement.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>
#include <hyprland/src/render/types.hpp>
#include <hyprland/src/render/decorations/CHyprGroupBarDecoration.hpp>
#include <hyprland/src/render/decorations/DecorationPositioner.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>
#include <hyprutils/string/Numeric.hpp>
#undef protected
#undef private
#include "Config.hpp"
#include "Dock.hpp"
#include "DropIndicator.hpp"
#include "OverviewPassElement.hpp"
#include "Minimize.hpp"
#include "OverviewRender.hpp"
#include "Window.hpp"
#include "SearchProviders.hpp"

// Anything that eases by hand -- tile hover, tile press, the grid reflow -- goes
// through here, so the whole plugin is driven by the same two curve objects the
// open and close animations use. Global scope on purpose: TileChrome.cpp links
// to this exact symbol (_Z9houseEasebf) via its own extern declaration, so it
// must NOT move into OvInternal. Defined in scrollOverview.cpp.
float houseEase(bool decel, float t);

// Card chrome helpers (shadow + rounding) live in their own header so the dock
// can reach them without this everything-header. Included before the
// namespace opens because it opens OvInternal itself.
#include "OverviewChrome.hpp"

// The pure transforms (getOverviewBox family, clamps, lerps) and the grid.
// Both open OvInternal themselves, hence the same placement as the chrome
// header. OverviewGeometry.hpp deliberately includes NO Hyprland header --
// that is what lets scripts/geometry_test.cpp compile it on its own.
#include "OverviewGeometry.hpp"
#include "GridLayout.hpp"

namespace OvInternal {

// The two enums OverviewGeometry mirrors so it can stay free of Hyprland. A
// value that stops matching turns every transform into nonsense in silence, so
// it is a compile error instead.
namespace OvGeom = ScrollOverview::Geometry;
static_assert(sc<int>(ScrollOverview::Config::ELayout::VERTICAL) == sc<int>(OvGeom::EAxis::VERTICAL));
static_assert(sc<int>(ScrollOverview::Config::ELayout::HORIZONTAL) == sc<int>(OvGeom::EAxis::HORIZONTAL));
static_assert(sc<int>(Layout::CORNER_TOPLEFT) == sc<int>(OvGeom::CORNER_TOPLEFT));
static_assert(sc<int>(Layout::CORNER_TOPRIGHT) == sc<int>(OvGeom::CORNER_TOPRIGHT));
static_assert(sc<int>(Layout::CORNER_BOTTOMLEFT) == sc<int>(OvGeom::CORNER_BOTTOMLEFT));
static_assert(sc<int>(Layout::CORNER_BOTTOMRIGHT) == sc<int>(OvGeom::CORNER_BOTTOMRIGHT));

inline float overviewTopChromeBottomL(PHLMONITOR monitor);

// Sit the scaled card in the band between the Omarchy top bar and the dock
// so the padding above the card equals the padding below it. The shift fades
// with open progress: at scale 1 the desktop still fills the monitor.
inline Vector2D overviewCardOriginOffset(PHLMONITOR monitor, float overviewScale) {
    if (!monitor)
        return {};

    const float MS    = std::max(monitor->m_scale, 0.01F);
    const float H     = sc<float>(monitor->m_size.y) * MS;
    const float MONCY = H * 0.5F;

    const float TOP = std::max(0.F, (overviewTopChromeBottomL(monitor) - sc<float>(monitor->m_position.y)) * MS);

    float bottom = H;
    if (ScrollOverview::Config::getValue<bool>("plugin:hyprdrive:dock:enabled")) {
        const float ICON = sc<float>(std::clamp(ScrollOverview::Config::getValue<int>("plugin:hyprdrive:dock:icon_size"), 8, 512));
        // Dock.cpp: ICON_PAD 14 on top and bottom, EDGE_MARGIN 4 under the pill.
        const float DOCKH = ICON + 14.F + 14.F + 4.F;
        bottom            = std::max(TOP + 1.F, H - DOCKH * MS);
    }

    if (bottom <= TOP + 1.F)
        return {};

    const float OPEN  = ScrollOverview::Config::getScale();
    const float RANGE = std::max(0.01F, 1.F - OPEN);
    const float T     = std::clamp((1.F - overviewScale) / RANGE, 0.F, 1.F);
    return {0.F, ((TOP + bottom) * 0.5F - MONCY) * T};
}

// Everything the pure transforms need from an output, read once at the boundary.
inline OvGeom::SViewport viewportOf(PHLMONITOR monitor, float overviewScale = 1.F) {
    OvGeom::SViewport vp{monitor->m_position, monitor->m_size, monitor->m_scale};
    if (monitor && overviewScale < 0.999F)
        vp.originOffset = overviewCardOriginOffset(monitor, overviewScale);
    return vp;
}

inline OvGeom::EAxis axisOf(ScrollOverview::Config::ELayout layout) {
    return sc<OvGeom::EAxis>(layout);
}

inline PHLWINDOW getOverviewFullscreenVisibilityWindow(const PHLWORKSPACE& workspace, const PHLWINDOW& fallback = {});
inline constexpr const char* OVERVIEW_SUBMAP = "hyprdrive";
// ONE RELEASE. The submap is defined by the USER's config, not by the plugin,
// so a config written before the rename binds its clicks under the old name and
// would simply stop working the day the plugin stopped answering to it.
inline constexpr const char* OVERVIEW_SUBMAP_LEGACY = "scrolloverview";

inline bool isOverviewSubmapName(const std::string& name) {
    return name == OVERVIEW_SUBMAP || name == OVERVIEW_SUBMAP_LEGACY;
}

// The name this config actually binds, preferring the new one when both exist.
inline const char* boundOverviewSubmap() {
    if (g_pKeybindManager && std::ranges::any_of(g_pKeybindManager->m_keybinds, [](const auto& keybind) { return keybind && keybind->submap.name == OVERVIEW_SUBMAP; }))
        return OVERVIEW_SUBMAP;
    return OVERVIEW_SUBMAP_LEGACY;
}
inline CScrollOverview*      g_pointerGrabOverview = nullptr;

inline void removeOverview(CScrollOverview* overview) {
    const auto PMONITOR = overview ? overview->pMonitor.lock() : PHLMONITOR{};
    unregisterScrollOverview(overview);
    if (scrollOverviews().empty()) {
        disableScrollOverviewHooks();
        // Click-to-close and swipe-down never went through the dispatcher, so
        // the query lived until the next Super-tap. A fully gone overview
        // always starts the next one empty.
        resetSearchSession();
    }

    if (PMONITOR) {
        PMONITOR->recheckSolitary();
        g_pHyprRenderer->damageMonitor(PMONITOR);
    }
}

inline xkb_keysym_t getOverviewKeysym(const IKeyboard::SKeyEvent& event) {
    const auto PKEYBOARD = g_pSeatManager->m_keyboard.lock();

    if (!PKEYBOARD)
        return XKB_KEY_NoSymbol;

    xkb_state* const STATE = PKEYBOARD->m_resolveBindsBySym && PKEYBOARD->m_xkbSymState ? PKEYBOARD->m_xkbSymState : PKEYBOARD->m_xkbState;

    if (!STATE)
        return XKB_KEY_NoSymbol;

    return xkb_state_key_get_one_sym(STATE, event.keycode + 8);
}

inline bool hasOverviewSubmap() {
    return g_pKeybindManager && std::ranges::any_of(g_pKeybindManager->m_keybinds, [](const auto& keybind) { return keybind && isOverviewSubmapName(keybind->submap.name); });
}

// Whether the submap binds any KEY, as opposed to only mouse buttons.
// Tested on the key STRING, not SKeybind::mouse: a plain `bind` whose key is
// "mouse:272" leaves that flag false (it marks bindm-style drag binds), so the
// flag alone counted this rice's two click binds as keyboard binds and kept the
// keyboard hook uninstalled -- measured 2026-08-01, arrows and Return dead.
inline bool hasOverviewSubmapKeyBinds() {
    return g_pKeybindManager && std::ranges::any_of(g_pKeybindManager->m_keybinds, [](const auto& keybind) {
        return keybind && isOverviewSubmapName(keybind->submap.name) && !keybind->mouse && !keybind->key.starts_with("mouse");
    });
}

inline bool isOverviewSubmapActive() {
    return g_pKeybindManager && isOverviewSubmapName(g_pKeybindManager->getCurrentSubmap().name);
}

// The overview is not a Wayland client. Clear seat keyboard focus so keys
// cannot reach the window (or a layer-shell bar) underneath; the raw-key
// hook builds the search query from the same stream.
inline void stealOverviewKeyboard() {
    if (g_pSeatManager)
        g_pSeatManager->setKeyboardFocus(nullptr);
}

// Give the keyboard back to the window overview stole it from. Seat focus is
// a different thing from "which window is active": stealOverviewKeyboard
// nulls the seat without changing Desktop::focusState, so close() used to
// skip fullWindowFocus when the same terminal was still "the" window -- and
// Hyprland never sent wl_keyboard.enter. Typing stayed dead until a workspace
// switch forced a real refocus. Passing the surface, then forcing the seat,
// is the click-path that actually delivers enter.
inline void restoreOverviewKeyboard(const PHLWINDOW& window) {
    for (const auto& overview : scrollOverviews()) {
        if (overview && !overview->isClosing())
            return;
    }

    PHLWINDOW target = window;
    if (!Desktop::View::validMapped(target))
        target = Desktop::focusState()->window();
    if (!Desktop::View::validMapped(target))
        return;

    SP<CWLSurfaceResource> surf;
    if (target->wlSurface())
        surf = target->wlSurface()->resource();

    Desktop::focusState()->fullWindowFocus(target, Desktop::FOCUS_REASON_SWITCH_TO_WINDOW_HARD, surf);
    if (g_pSeatManager && surf)
        g_pSeatManager->setKeyboardFocus(surf);
}

inline bool isTopLayerFocused(PHLMONITOR monitor) {
    const auto FOCUSEDSURFACE = g_pSeatManager->m_state.keyboardFocus.lock();

    if (!FOCUSEDSURFACE)
        return false;

    const auto HLSURFACE = Desktop::View::CWLSurface::fromResource(FOCUSEDSURFACE);
    if (!HLSURFACE)
        return false;

    const auto VIEW = HLSURFACE->view();
    if (!VIEW)
        return false;

    auto layerOwner = Desktop::View::CLayerSurface::fromView(VIEW);

    if (!layerOwner) {
        const auto POPUP = Desktop::View::CPopup::fromView(VIEW);
        if (POPUP) {
            const auto T1OWNER = POPUP->getT1Owner();
            if (T1OWNER)
                layerOwner = Desktop::View::CLayerSurface::fromView(T1OWNER->view());
        }
    }

    return layerOwner && layerOwner->m_monitor == monitor && layerOwner->m_layer >= ZWLR_LAYER_SHELL_V1_LAYER_TOP;
}

inline constexpr const char* OVERVIEW_INSERT_FADE_BEZIER = "hyprdriveWorkspaceInsertFade";
inline constexpr const char* OVERVIEW_REMOVE_FADE_BEZIER = "hyprdriveWorkspaceRemoveFade";

// The overview's own entrance and exit curves. Both end flat rather than
// overshooting -- an overview that bounces reads as a toy, and a bounce on the
// exit fights the reversal, because a reversed close would have to unwind an
// overshoot it had already started.
//
// These are the rice's two shared curves -- `appleDecel` / `appleAccel` in
// hyprland/general.lua and in the quickshell Appearance.qml. The plugin cannot
// reference a config curve by name, so the control points are duplicated here:
// change all three together or the rice stops speaking one motion language.
inline constexpr const char* OVERVIEW_OPEN_BEZIER  = HOUSE_DECEL_BEZIER;
inline constexpr const char* OVERVIEW_CLOSE_BEZIER = HOUSE_ACCEL_BEZIER;

// houseEase lives at GLOBAL scope, declared above the namespace and defined in
// scrollOverview.cpp. It is the one helper other translation units link to by
// symbol: TileChrome.cpp extern-declares `float houseEase(bool, float)` and the
// original split swept the definition into OvInternal:: with everything else,
// which renamed the exported symbol and left _Z9houseEasebf undefined. A shared
// object links fine with undefined symbols and dlopen resolves lazily, so the
// compositor died at the first mid-close tile ease, not at load: ld.so's error
// path exits 127, and a garbage PLT jump gives "segfault at X ip X".
// scripts/symcheck.sh now diffs against the host's exports instead of grepping
// for a namespace, precisely so the next global-scope symbol cannot hide.

inline bool isPointerOnTopLayer(PHLMONITOR monitor) {
    if (!monitor)
        return false;

    const auto MOUSECOORDS = g_pInputManager->getMouseCoordsInternal();
    Vector2D   surfaceCoords;
    PHLLS      layerSurface;

    if (Desktop::viewState()->hitTest().layerSurfaceAt(MOUSECOORDS, &monitor->m_layerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY], &surfaceCoords, &layerSurface))
        return true;

    return !!Desktop::viewState()->hitTest().layerSurfaceAt(MOUSECOORDS, &monitor->m_layerSurfaceLayers[ZWLR_LAYER_SHELL_V1_LAYER_TOP], &surfaceCoords, &layerSurface);
}

inline PHLWINDOW getOverviewWindowToShow(const PHLWINDOW& window) {
    if (!window)
        return nullptr;

    if (window->m_group)
        return window->m_group->current();

    return window;
}

// OVERVIEW_CPP_SPEC §13.4 G5. TWO LAYOUTS, TWO CAPS, and the caps are PER
// PROVIDER. Apps are a grid and cap at 6; every other provider is a list and
// caps at 5 ROWS EACH, not 5 shared. A shared cap is what made X49's section
// cards invisible: a query matching 5 apps starved every provider, so there was
// only ever one card to categorise.
inline constexpr size_t OVERVIEW_APP_RESULT_LIMIT      = 6;
inline constexpr size_t OVERVIEW_PROVIDER_ROW_LIMIT    = 5;

// X19: the panel shows ONE ordered list. Local app entries first -- they are an
// in-memory scan and always instant -- then whatever the SearchProvider2
// providers have answered with so far. Providers arrive asynchronously, so this
// is deliberately a snapshot: the list grows across frames as replies land, and
// a provider that never answers simply never appears.
struct SSearchRow {
    std::string                  name;
    std::string                  description;
    std::string                  icon;
    std::string                  iconFont;
    const AppSearch::SEntry*     app = nullptr;
    SearchProviders::SResult     provider;
    std::optional<OmarchySearch::SSearchItem> omarchy;
    int                          column = 0; // 0 = Left (Apps, Actions, Calc, Agents), 1 = Right (Files, Settings)
};

inline PHLWINDOW overviewWindowForApp(const AppSearch::SEntry& entry);

inline std::vector<SSearchRow> overviewSearchRows() {
    std::vector<SSearchRow> rows;
    Prof::Timer PROF{Prof::rowsNs, Prof::rowsCalls};

    const std::string& Q           = g_search.query.text;
    const bool         MENU_BROWSE = OmarchySearch::canNavigateMenuBack();

    // ------------------------------------------------------------------------
    // COLUMN 0: OMARCHY MENU & APPS (Super+Space launcher)
    // APPS ALWAYS LEAD. Running ones first, so Enter switches to an open window
    // even when an Omarchy menu row also matches; then apps that are not open
    // yet; then the menu.
    //
    // User: typing "zen" with Zen closed used to put "Zen setup -> Defaults ->
    // Browser" on top, because menu rows were spliced BETWEEN running and
    // unopened apps. Searching an app's name is a request for the app, not for
    // a setting that mentions it -- the setting is still there, one row down.
    // ------------------------------------------------------------------------
    std::vector<SSearchRow> runningApps, menuRows, otherApps;

    if (!Q.empty() && !MENU_BROWSE) {
        for (const auto* E : AppSearch::search(Q, 6)) {
            SSearchRow row{E->name, "", E->icon, "", E, {}, std::nullopt, 0};
            if (overviewWindowForApp(*E))
                runningApps.push_back(std::move(row));
            else
                otherApps.push_back(std::move(row));
        }
    }

    const size_t MENU_LIMIT = (Q.empty() || MENU_BROWSE) ? 24 : 8;
    for (const auto& item : OmarchySearch::searchMenu(Q, MENU_LIMIT))
        menuRows.push_back({item.name, item.description, item.icon, item.iconFont, nullptr, {}, item, 0});

    rows.insert(rows.end(), runningApps.begin(), runningApps.end());
    rows.insert(rows.end(), otherApps.begin(), otherApps.end());
    rows.insert(rows.end(), menuRows.begin(), menuRows.end());

    // ------------------------------------------------------------------------
    // COLUMN 1: FILES & FOLDERS (Omarchy Find Index)
    // ------------------------------------------------------------------------
    if (!MENU_BROWSE) {
        if (Q.empty()) {
            for (const auto& f : OmarchySearch::recentFiles()) {
                rows.push_back({f.name, f.description, f.icon, "", nullptr, {}, f, 1});
            }
        } else {
            for (const auto& f : OmarchySearch::fileResults()) {
                rows.push_back({f.name, f.description, f.icon, "", nullptr, {}, f, 1});
            }
        }
    }

    // ------------------------------------------------------------------------
    // COLUMN 2: AI AGENTS & TOOLS
    // ------------------------------------------------------------------------
    if (!Q.empty()) {
        if (const auto calc = OmarchySearch::evaluateCalculator(Q); calc) {
            rows.push_back({calc->name, calc->description, calc->icon, "", nullptr, {}, *calc, 2});
        }
    }

    if (OmarchySearch::isAgentQuery(Q)) {
        for (const auto& item : OmarchySearch::searchAgents(Q))
            rows.push_back({item.name, item.description, item.icon, item.iconFont, nullptr, {}, item, 2});
    } else if (!Q.empty() && !MENU_BROWSE) {
        for (const auto& item : OmarchySearch::promptAllAgents(Q))
            rows.push_back({item.name, item.description, item.icon, item.iconFont, nullptr, {}, item, 2});

        OmarchySearch::SSearchItem webItem;
        webItem.kind          = OmarchySearch::EItemKind::Action;
        webItem.id            = "web:google";
        webItem.name          = "Search Google";
        webItem.description   = Q;
        webItem.icon          = "web-browser";
        webItem.category      = "Web";
        std::string qenc;
        for (unsigned char c : Q) {
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
                qenc += static_cast<char>(c);
            else if (c == ' ')
                qenc += '+';
            else {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "%%%02X", c);
                qenc += buf;
            }
        }
        webItem.actionCommand = "xdg-open 'https://www.google.com/search?q=" + qenc + "'";
        rows.push_back({webItem.name, webItem.description, webItem.icon, "", nullptr, {}, webItem, 2});
    } else if (!MENU_BROWSE) {
        for (const auto& item : OmarchySearch::agentHelpItems())
            rows.push_back({item.name, item.description, item.icon, item.iconFont, nullptr, {}, item, 2});
    }

    return rows;
}

// X49. One card per column: Omarchy Menu (0), Files & Folders (1), AI & Tools (2).
struct SSearchSection {
    std::string         title;
    std::string         icon;
    std::vector<size_t> rows;  // indices into the overviewSearchRows() vector
    size_t              total = 0;
    bool                grid = false;
    int                 column = 0; // 0, 1, or 2
};

inline std::vector<SSearchSection> overviewSearchSections(const std::vector<SSearchRow>& rows) {
    std::vector<SSearchSection> out;
    std::string menuTitle = OmarchySearch::menuCurrentRouteTitle();
    out.push_back({menuTitle.empty() ? "Omarchy Menu" : menuTitle, "system-run", {}, 0, false, 0});
    out.push_back({"Files & Folders", "folder", {}, 0, false, 1});
    out.push_back({"AI & Tools", "utilities-terminal", {}, 0, false, 2});

    for (size_t i = 0; i < rows.size(); ++i) {
        int col = std::clamp(rows[i].column, 0, 2);
        out[sc<size_t>(col)].rows.push_back(i);
    }

    for (auto& sec : out) {
        sec.total = sec.rows.size();
    }

    return out;
}

// Mapped window of this desktop entry, if any. Prefer urgent, then a
// non-minimized instance, so Enter switches instead of spawning a duplicate.
inline PHLWINDOW overviewWindowForApp(const AppSearch::SEntry& entry) {
    PHLWINDOW best;
    for (const auto& WINDOW : Desktop::windowState()->windows()) {
        if (!WINDOW || !WINDOW->m_isMapped)
            continue;
        if (WINDOW->isHidden())
            continue;
        if (OmarchySearch::isOverviewSushiWindow(WINDOW->m_class, WINDOW->getPID()))
            continue;

        bool match = false;
        if (!entry.wmClass.empty() && WINDOW->m_class == entry.wmClass)
            match = true;
        else if (AppSearch::idForClass(WINDOW->m_class) == entry.id)
            match = true;
        else if (WINDOW->m_class == entry.id)
            match = true;
        if (!match)
            continue;

        if (WINDOW->m_isUrgent)
            return WINDOW;
        if (!best)
            best = WINDOW;
        else if (ScrollOverview::Minimize::isMinimized(best) && !ScrollOverview::Minimize::isMinimized(WINDOW))
            best = WINDOW;
    }
    return best;
}

// Activating a result runs the corresponding app, Omarchy action, agent prompt,
// calculator copy, or file open. Submenu navigation returns false so the overview
// stays open while drilling down into the hierarchy. For an already-open app,
// Enter focuses that window unless newInstance is set (double-tap Shift).
inline bool activateSearchResult(int index, bool newInstance = false) {
    const auto RESULTS = overviewSearchRows();
    if (index < 0 || index >= sc<int>(RESULTS.size()))
        return false;

    const auto& ROW = RESULTS[index];
    if (ROW.omarchy) {
        if (ROW.omarchy->kind == OmarchySearch::EItemKind::Menu || ROW.omarchy->kind == OmarchySearch::EItemKind::Back) {
            const bool DRILL = ROW.omarchy->kind == OmarchySearch::EItemKind::Menu;
            OmarchySearch::activate(*ROW.omarchy);
            if (DRILL) {
                g_search.query.text.clear();
                g_search.query.active          = false;
                g_search.nav.zone              = EOverviewNavZone::Result;
                g_search.nav.explicitSelection = true;
                g_search.nav.selectFirstPending = false;
                g_search.nav.intent            = ESearchIntent::Browsing;
                g_search.nav.hoverIndex        = -1;
            }
            // Skip the ‹ Back row so a second Enter launches the first child.
            const auto NEXT = overviewSearchRows();
            int        sel  = 0;
            for (int i = 0; i < sc<int>(NEXT.size()); ++i) {
                if (NEXT[sc<size_t>(i)].column != 0)
                    continue;
                if (NEXT[sc<size_t>(i)].omarchy && NEXT[sc<size_t>(i)].omarchy->kind == OmarchySearch::EItemKind::Back)
                    continue;
                sel = i;
                break;
            }
            g_search.nav.selection = sel;
            return false;
        }
        return OmarchySearch::activate(*ROW.omarchy);
    } else if (ROW.app) {
        if (!newInstance) {
            if (const auto W = overviewWindowForApp(*ROW.app)) {
                g_search.nav.pendingFocus = W;
                return true;
            }
        }
        AppSearch::launch(*ROW.app, ScrollOverview::Config::getTerminal());
        return true;
    } else {
        SearchProviders::activate(ROW.provider, g_search.query.text);
        return true;
    }
}

// OVERVIEW_CPP_SPEC §5 F2, and §13.7 X18's open-window half.
//
// Open windows ARE the window search result: they are already the grid's tiles,
// already promoted from other desktops by B2 and already animated in by F3-b,
// so a second result ROW for a window the user can see would be the same window
// twice in one view. What X18 buys is PARITY OF MATCHING with
// windows-search-provider@G-dH, and the host's own settings were read rather
// than its defaults: `search-method` is 0 = Strict, so the fuzzy app scorer is
// the WRONG algorithm here. Strict is every space-separated term appearing
// somewhere, in any order, case- and accent-insensitively.
//
// Haystack is the extension's own, `${wsIndex + 1}: ${title} ${appName}`, plus
// the window class -- dropping the class would regress the F2 behaviour that
// shipped long before X18.
// Counts RENDERED frames, which is the only clock this code actually advances
// on: an idle overview renders nothing, so a wall-clock "last touched" measures
// how long the user sat still rather than whether the tile was drawn.
// ponytail: one counter for all monitors, so a second output would advance it
// twice per frame and every tile would read as skipped; make it per-monitor if
// this ever runs on more than one.
inline uint64_t g_overviewFrame = 0;

inline bool overviewSearchMatches(const PHLWINDOW& window) {
    if (g_search.query.text.empty())
        return true;

    Prof::Timer PROF{Prof::matchNs, Prof::matchCalls};

    // X52 secondary. ~40 call sites consult this gate, so one frame asks the
    // same question about the same window dozens of times -- measured 617-1041
    // calls per frame over a 16-window fixture on the broad query `x`, each one
    // rebuilding the haystack from scratch. The answer cannot change within a
    // frame, so cache it for the frame.
    //
    // The key carries the QUERY as well as the frame counter, not a generation
    // number someone has to remember to bump: a keystroke lands between frames,
    // and the query is 1-3 characters, so comparing it is cheaper than the
    // allocation this skips. Liveness (validMapped) stays OUTSIDE the memo, in
    // shouldShowOverviewWindow, so an unmapped window is never served from here.
    //
    // The three cache fields were function-local statics of this inline
    // function. They are session state -- they are keyed on the query -- so they
    // live with it now; the single-instance property is unchanged, and a header
    // that grows a second definition of this function can no longer grow a
    // second cache with it.
    auto& CACHE = g_search.query;

    if (CACHE.memoFrame != g_overviewFrame || CACHE.memoQuery != CACHE.text) {
        CACHE.memo.clear();
        CACHE.memoFrame = g_overviewFrame;
        CACHE.memoQuery = CACHE.text;
    } else if (const auto IT = CACHE.memo.find(window.get()); IT != CACHE.memo.end())
        return IT->second;

    // Hyprland numbers regular workspaces from 1, which is what the extension
    // renders as `wsIndex + 1`, so typing "2:" finds desktop 2 on both.
    std::string haystack = window->m_workspace ? std::format("{}: ", window->m_workspace->m_id) : "";
    haystack += window->m_title + " " + window->m_class;

    if (const auto APPNAME = AppSearch::appNameForClass(window->m_class); !APPNAME.empty())
        haystack += " " + APPNAME;

    const bool MATCHED       = AppSearch::strictMatch(CACHE.text, haystack);
    CACHE.memo[window.get()] = MATCHED;
    return MATCHED;
}

// P6i1, ROUND TWO: THE GATE IS SPLIT BY COST, and only the expensive half is
// shadowed -- and, at cutover, cached.
//
// Round one shadowed the whole gate and found 15 divergences over a full suite,
// every one of them `validMapped` flipping inside a single generation:
// `window.close` fires when the close is REQUESTED, the unmap happens after it,
// and `window.destroy` fires later still. Nothing announces the unmap, so no
// bump set can ever cover it. But the properties with that shape -- isHidden,
// validMapped, the pinned+floating pair -- are also the CHEAPEST part of the
// gate, four bool reads off the window, so they are simply re-read on every
// call, forever. Only what actually costs gets a generation key.
//
// This half is that cost: a string match over workspace+title+class+appname,
// and a linear scan of the minimize stash. Both change only through mutators
// that already bump (query edit, session reset, minimize, restore, title,
// class, moveToWorkspace), which is exactly the property a generation key needs.
inline bool overviewMembershipMatchesLive(const PHLWINDOW& WINDOW) {
    // Y9 REVISED (user, 2026-08-06): a minimized window belongs in the overview
    // ONLY while a query is active. With an empty query it stays put away --
    // which is what minimizing it meant -- and this is X58(a)'s original wording
    // restored: "filtering needs to work even on minimized windows".
    //
    // The gate goes HERE and nowhere else on purpose. shouldShowOverviewWindow
    // is the one thing every overview path consults (see the note on it), so
    // putting the rule under it keeps render, hit-test, drag and grid from ever
    // disagreeing about whether a stashed window exists. The workspace-home
    // plumbing in overviewGridWindows and addOverviewWindow stays: it is what
    // puts the window on the RIGHT desktop once a query does bring it back.
    if (g_search.query.text.empty() && ScrollOverview::Minimize::isMinimized(WINDOW))
        return false;

    return overviewSearchMatches(WINDOW);
}

// THE CACHE READ, and the only one. Keyed on WINDOW -- the group's current --
// rather than on the window handed in: every member of a group asks this
// question about the same representative and has to get the same answer.
//
// With the shadow off (the shipping path) a hit returns immediately and nothing
// else in this function runs. With it on, the live answer is computed anyway and
// compared, which is how the bump set gets re-proved after someone adds a
// mutator -- and is why the comparison lives here rather than in a test that
// would have to reconstruct sixty callers to mean anything.
inline bool overviewMembershipMatches(const PHLWINDOW& WINDOW) {
    bool       cached = false;
    const bool HIT    = Membership::lookup(WINDOW.get(), cached);

    if (HIT && !Membership::shadowEnabled())
        return cached;

    const bool LIVE = overviewMembershipMatchesLive(WINDOW);

    if (HIT && cached != LIVE)
        Membership::divergence(WINDOW->m_class, WINDOW->m_title, LIVE);

    Membership::store(WINDOW.get(), LIVE);
    return LIVE;
}

// The search filter hangs off this gate rather than off the grid builder: every
// overview path (render, hit-test, drag, grid) already consults it, so they
// cannot disagree about which windows exist. Dropping a window from the gate
// removes it from the grid too, and the survivors re-lay-out through the U4
// reflow animation at no extra cost.
// Omarchy's screensaver is a real fullscreen terminal (class
// org.omarchy.screensaver). The overview owns the monitor's render pass, so
// that window cannot cover the output while an overview is up. Drop the
// overview immediately; do not wait for the close animation.
inline bool isScreensaverWindow(const PHLWINDOW& window) {
    if (!window)
        return false;
    return window->m_class == "org.omarchy.screensaver" || window->m_initialClass == "org.omarchy.screensaver";
}

inline bool shouldShowOverviewWindow(const PHLWINDOW& window) {
    // Z9r5: a window hidden for a dock flight is the proxy's alone -- drawing
    // its tile too would be the double image the spec forbids.
    if (window && window->isHidden())
        return false;

    const auto WINDOW = getOverviewWindowToShow(window);

    if (!WINDOW)
        return false;

    if (OmarchySearch::isOverviewSushiWindow(WINDOW->m_class, WINDOW->getPID()))
        return false;

    if (!validMapped(WINDOW))
        return false;

    if (WINDOW->m_pinned && WINDOW->m_isFloating)
        return false;

    return overviewMembershipMatches(WINDOW);
}

// THE TILE PREDICATE. A tile is on screen if the membership gate admits it OR
// the grid has promoted it, and BOTH the render and the hit-test ask exactly
// this question -- which is the whole point of the function existing.
//
// They used to ask different ones. The render drew a window when
// `PROMOTED || shouldShowOverviewWindow(window)`; windowAtOverviewPoint asked
// the gate alone. So for as long as a promoted or releasing tile was drawn it
// took no press: the click fell THROUGH it to the workspace CARD underneath and
// switched desktops, which is the user's "the click took me to an empty
// desktop". Measured with the render-list shadow -- 48 RENDER-UNCLICK lines in
// one soak, a drawn tile's own centre hit-testing to another window or to
// nothing at all. B1 says render, hit-test and drag all place a tile through one
// getOverviewWindowBox; this is the same rule applied one step earlier, to
// whether the tile is there at all.
//
// `promoted` hands back WHICH half admitted it, because the render needs that
// anyway (it decides the clip box and suppresses the per-card offset) and asking
// twice is how the two answers drift apart again.
//
// On side effects: overviewGridPromoted arms and clocks the release, and the
// hit-test reaches it now. That is not a new class of call -- the very next line
// of every one of these loops calls getOverviewWindowBox, which has always
// called overviewGridReleasing unconditionally -- and the release clock is
// idempotent within a frame by construction (GridLayout.hpp).
inline bool overviewTileShown(const PHLWINDOW& window, PHLMONITOR monitor, bool* promoted = nullptr) {
    const bool PROMOTED = overviewGridPromoted(window, monitor);
    if (promoted)
        *promoted = PROMOTED;

    return PROMOTED || shouldShowOverviewWindow(window);
}

// THE MINI PREDICATE. A strip mini answers a DIFFERENT question from a tile:
// not "is this window drawn on this card" but "does this window LIVE on this
// desktop". The two were the same question for as long as a card's render list
// only ever held its own windows, and X58(a)'s homeless promoted window
// (redrawAll) is what breaks the tie: it is listed under the ACTIVE card
// because that is where its TILE is laid out, and drawing its mini there too
// would put a window on a desktop it is not on -- the user report 46bccac
// closed. HOME is the same rule that decides its slot, asked one more time.
inline bool overviewStripMiniShown(const PHLWINDOW& window, const PHLWORKSPACE& workspace) {
    if (!workspace || !shouldShowOverviewWindow(window))
        return false;

    return overviewGridHomeWorkspace(window) == workspace->m_id;
}

inline bool shouldShowPinnedFloatingOverviewWindow(const PHLWINDOW& window) {
    const auto WINDOW = getOverviewWindowToShow(window);

    if (!validMapped(WINDOW))
        return false;

    if (!WINDOW->m_pinned || !WINDOW->m_isFloating)
        return false;

    return true;
}

inline bool surfaceTreeHasFrameCallbacks(SP<CWLSurfaceResource> surface) {
    if (!surface)
        return false;

    bool hasCallbacks = false;
    surface->breadthfirst(
        [&hasCallbacks](SP<CWLSurfaceResource> child, const Vector2D&, void*) {
            if (!child || child->m_current.callbacks.empty())
                return;

            hasCallbacks = true;
        },
        nullptr);

    return hasCallbacks;
}

inline void surfaceTreePresent(SP<CWLSurfaceResource> surface, PHLMONITOR monitor, const Time::steady_tp& now) {
    if (!surface)
        return;

    std::pair<PHLMONITOR, Time::steady_tp> data = {monitor, now};
    surface->breadthfirst([](SP<CWLSurfaceResource> child, const Vector2D&, void* data) {
        if (!child)
            return;

        const auto [MONITOR, NOW] = *sc<std::pair<PHLMONITOR, Time::steady_tp>*>(data);
        child->presentFeedback(NOW, MONITOR, false);
    }, &data);
}

inline bool windowHasOverviewAnimation(const PHLWINDOW& window) {
    if (!window)
        return false;

    return window->positionAnimation()->isBeingAnimated() || window->sizeAnimation()->isBeingAnimated() || window->m_alpha.isBeingAnimated() ||
        window->m_borderFadeAnimationProgress->isBeingAnimated() || window->m_borderAngleAnimationProgress->isBeingAnimated() || window->m_dimPercent->isBeingAnimated() ||
        window->m_shadowFadeAnimationProgress->isBeingAnimated();
}

inline bool layerHasOverviewAnimation(const PHLLS& layer) {
    if (!Desktop::View::validMapped(layer))
        return false;

    return layer->positionAnimation()->isBeingAnimated() || layer->sizeAnimation()->isBeingAnimated() || layer->m_alpha.isBeingAnimated();
}

// Workspace thumbnail strip, OVERVIEW_CPP_SPEC §5 D'4 + E. A row of mini
// workspace previews along the top of the focused workspace card. It exists
// because at scale 0.82 the neighbour cards are ~180px slivers: there was
// nothing on screen to aim a drag at, and nowhere for D'4's post-drop preview
// to appear. Minis are real windows rendered small; spec §6 exempts them from
// frost, so this does not reopen section C.
inline float overviewStripLogicalHeight(PHLMONITOR monitor) {
    if (!ScrollOverview::Config::getStrip() || !monitor)
        return 0.F;

    const float H = sc<float>(ScrollOverview::Config::getStripHeight());
    return H > 1.F && H < sc<float>(monitor->m_size.y) / 2.F ? H : 0.F;
}

// Strip cells sit in the reserved band with this pad on every side. The panel
// around them is a fixed 10px (same as the reference). The empty space between
// the search pill and that panel is the chrome gap the grid must match below
// the strip -- otherwise the leftover band + mission-control gap reads as a
// hole between the strip and the windows.
inline constexpr float OVERVIEW_STRIP_BAND_PAD_FRAC = 0.18F;
inline constexpr float OVERVIEW_STRIP_PANELPADL     = 10.F;

inline float overviewStripPadL(PHLMONITOR monitor) {
    return overviewStripLogicalHeight(monitor) * OVERVIEW_STRIP_BAND_PAD_FRAC;
}

inline float overviewChromeGapL(PHLMONITOR monitor) {
    const float PAD = overviewStripPadL(monitor);
    return std::max(0.F, PAD - OVERVIEW_STRIP_PANELPADL + OVERVIEW_SEARCH_DOCKPADL);
}

// Search/results, then the drawn strip panel, then one chrome gap. The grid
// starts immediately after this; it must not also add mission_control_gap.
inline float overviewStripStackL(PHLMONITOR monitor) {
    const float BAND = overviewStripLogicalHeight(monitor);
    if (BAND <= 0.F)
        return 0.F;

    const float PAD   = overviewStripPadL(monitor);
    const float CELLH = std::max(0.F, BAND - 2.F * PAD);
    return PAD + CELLH + OVERVIEW_STRIP_PANELPADL + overviewChromeGapL(monitor);
}

// X10 + the pill row's published height. OVERVIEW_SEARCH_ROWHL and the value
// itself live in SearchSession.hpp: the row is written by the search render step
// from the pill's ACTUAL landed foot, so it is part of the published layout and
// not a free-standing global.
inline float overviewSearchLogicalHeight(PHLMONITOR monitor) {
    return monitor ? g_search.layout.rowL : 0.F;
}

// X50: how far down the monitor the top chrome reaches, in logical global px.
// The WORK AREA is no help: measured 2026-08-06, the rice's bar sets no
// exclusive zone -- `hyprctl monitors` reports reserved [0,0,0,0] while
// `quickshell:bar` occupies the top 63px -- so a dock target read from
// workArea() puts the pill UNDER the bar, which this pass draws after the whole
// overview. That is X10's occlusion finding reached from the other side, and it
// is what the first X50 build did.
// Only WIDE surfaces pinned to the monitor's top edge count: the same rice puts
// 250px screenCorners up there, and docking below one of those is a 23px move.
inline float overviewTopChromeBottomL(PHLMONITOR monitor) {
    if (!monitor)
        return 0.F;

    float bottom = sc<float>(monitor->m_position.y);
    for (const auto LEVEL : {ZWLR_LAYER_SHELL_V1_LAYER_TOP, ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY}) {
        for (auto const& lsRef : monitor->m_layerSurfaceLayers[LEVEL]) {
            const auto LS = lsRef.lock();
            if (!Desktop::View::validMapped(LS))
                continue;

            const auto& GEOM = LS->m_geometry;
            if (sc<float>(GEOM.w) < sc<float>(monitor->m_size.x) * 0.5F || sc<float>(GEOM.h) > sc<float>(monitor->m_size.y) / 3.F)
                continue;
            if (sc<float>(GEOM.y) > sc<float>(monitor->m_position.y) + 1.F)
                continue;

            bottom = std::max(bottom, sc<float>(GEOM.y + GEOM.h));
        }
    }

    return bottom;
}

// X21: the results panel is part of the LAYOUT STACK, not an overlay. Its band
// sits between the search row and the strip, so a growing list pushes the strip
// down and shrinks the grid instead of covering them. The EASED value (X23) is
// mirrored out of CScrollOverview::resultsBand into the published layout -- the
// free layout helpers below and the hit-test paths all read it, and none of them
// can reach a member.
inline constexpr float OVERVIEW_RESULT_ROWHL = 34.F, OVERVIEW_RESULT_ROWGAPL = 6.F, OVERVIEW_RESULT_PANELPADL = 8.F, OVERVIEW_RESULT_PANELGAPL = 10.F;
// X49. The results are WIDE and CATEGORISED: one rounded card per provider, the
// provider's icon and name on the card's leading edge, its rows across the rest
// of the width. HEADERWL is that leading column; HEADERHL is the floor a card
// gets, so a one-row section still has room for a 32px provider icon.
inline constexpr float OVERVIEW_RESULT_SECTIONGAPL = 8.F, OVERVIEW_RESULT_HEADERWL = 168.F, OVERVIEW_RESULT_HEADERHL = 44.F;
// G5's OTHER layout. Apps are a grid of icon cells, not rows: a 48px icon with
// its name under it. CELLWL is the widest a cell gets -- the grid caps at 6 AND
// at whatever fits, so a narrow monitor wraps instead of overflowing.
inline constexpr float OVERVIEW_RESULT_CELLWL = 132.F, OVERVIEW_RESULT_CELLHL = 78.F, OVERVIEW_RESULT_CELLICONL = 48.F;

// The rest of the card's metrics, which the draw pass used to spell out as bare
// literals eight scroll-screens away from the family above. Every one of these
// is multiplied by the card transform's SCALE at the point of use -- they are
// LOGICAL px like the OVERVIEW_RESULT_* family, not rendered px.
//
// The repeats are named apart on purpose: the icon-to-title gap, the icon-to-row
// -text gap and the name-to-description gap were all the literal 10, and reading
// them as one number is how "nudge the description over" quietly moves the G6
// header too.
inline constexpr float OVERVIEW_CARD_PADL         = 12.F; // inner pad: card edge -> icon, row edge -> icon
inline constexpr float OVERVIEW_CARD_HEADICONL    = 32.F; // G6 provider icon on the leading edge
inline constexpr float OVERVIEW_CARD_HEADGAPL     = 10.F; // that icon -> the provider name
inline constexpr float OVERVIEW_CARD_HEADPX       = 14.F; // the provider name
inline constexpr float OVERVIEW_CARD_MOREPX       = 12.F; // G6's "%d more", one step down
inline constexpr float OVERVIEW_CARD_HEADLINEGAPL = 2.F;  // name -> "%d more", so the pair centres as a block
inline constexpr float OVERVIEW_CARD_ROWPX        = 14.F; // a list row's name and description
inline constexpr float OVERVIEW_CARD_ROWICONL     = 22.F; // the app icon on a list row
inline constexpr float OVERVIEW_CARD_ROWICONGAPL  = 10.F; // that icon -> the name
inline constexpr float OVERVIEW_CARD_DESCGAPL     = 10.F; // the name -> the dimmer description beside it
inline constexpr float OVERVIEW_CARD_DESCMINL     = 24.F; // below this much room the description is dropped, not squeezed
inline constexpr float OVERVIEW_CARD_CELLPADL     = 6.F;  // G5 grid cell: its own inset
inline constexpr float OVERVIEW_CARD_CELLGAPL     = 4.F;  // that cell's icon -> its label
inline constexpr float OVERVIEW_CARD_CELLPX       = 13.F; // that label

inline float overviewResultsLogicalHeight(PHLMONITOR monitor) {
    return monitor ? std::max(0.F, g_search.layout.bandH) : 0.F;
}

// The width the result cards span: the monitor inset by the mission-control
// gap, which is the same span the grid tiles get (X49). The band maths and the
// render pass both need it, and a grid's height depends on it.
inline float overviewResultsSpanW(PHLMONITOR monitor) {
    if (!monitor)
        return 1.F;

    const float GAPL = sc<float>(ScrollOverview::Config::getMissionControlGap());
    return std::clamp(sc<float>(monitor->m_size.x) * 0.84F, 880.F, std::max(1.F, sc<float>(monitor->m_size.x) - (GAPL * 2.F)));
}

// Unified search panel: visible columns + inspector fill the container.
// Empty columns ease to width 0; leftover is split by flex (files take more
// when present so paths fit).
inline constexpr float SEARCH_PAD_L        = 14.F;
inline constexpr float SEARCH_DIV_GAPL     = 14.F;
inline constexpr float SEARCH_INSP_WL      = 280.F;
inline constexpr float SEARCH_INSP_MID_WL  = 340.F; // 2 result columns
inline constexpr float SEARCH_INSP_WIDE_WL = 400.F; // 1 result column
inline constexpr float SEARCH_COL_FLEX[3]  = {1.F, 1.45F, 1.F};
inline constexpr float SEARCH_COL_HIDE_WL  = 8.F;

struct SSearchColWidths {
    float col[3]    = {};
    float inspector = SEARCH_INSP_WL;
    float container = 0.F;
    int   visible   = 0;
};

inline uint8_t overviewSearchColMask(const std::vector<SSearchSection>& sections) {
    uint8_t mask = 0;
    for (const auto& sec : sections) {
        if (!sec.rows.empty() && sec.column >= 0 && sec.column < 3)
            mask |= static_cast<uint8_t>(1u << sec.column);
    }
    return mask;
}

inline SSearchColWidths overviewSearchColWidths(float containerWL, uint8_t mask) {
    SSearchColWidths w;
    w.container = containerWL;
    int   n    = 0;
    float flex = 0.F;
    for (int i = 0; i < 3; ++i) {
        if (mask & (1u << i)) {
            ++n;
            flex += SEARCH_COL_FLEX[i];
        }
    }
    w.visible = n;
    if (n == 0 || containerWL < SEARCH_COL_HIDE_WL)
        return w;

    const float CHROME = (SEARCH_PAD_L * 2.F) + (SEARCH_DIV_GAPL * sc<float>(n));
    float       insp   = SEARCH_INSP_WL;
    if (n == 1)
        insp = SEARCH_INSP_WIDE_WL;
    else if (n == 2)
        insp = SEARCH_INSP_MID_WL;
    const float MIN_COLS = 160.F * sc<float>(n);
    insp                 = std::min(insp, std::max(200.F, containerWL - CHROME - MIN_COLS));
    float inner          = containerWL - CHROME - insp;
    if (inner < 120.F) {
        insp  = std::clamp(containerWL * 0.28F, 160.F, insp);
        inner = containerWL - CHROME - insp;
    }
    inner         = std::max(0.F, inner);
    w.inspector   = insp;
    if (flex <= 0.F)
        return w;
    for (int i = 0; i < 3; ++i) {
        if (mask & (1u << i))
            w.col[i] = inner * (SEARCH_COL_FLEX[i] / flex);
    }
    return w;
}

// G5: the grid "caps at 6 and at whatever fits the width". The 6 is applied
// when the rows are gathered; this is the width half.
inline size_t overviewResultsGridCols(size_t cells, float spanWL) {
    const float INNER = std::max(1.F, spanWL - (OVERVIEW_RESULT_PANELPADL * 2.F));
    const auto  FITS  = sc<size_t>(std::max(1.F, std::floor(INNER / OVERVIEW_RESULT_CELLWL)));
    return std::max(sc<size_t>(1), std::min(cells, FITS));
}

// The logical height one card needs. A LIST card is its rows, its padding, and
// a floor so the G6 provider icon on the leading edge always fits a one-row
// section. A GRID card is however many cell rows the width forced it into.
inline float overviewResultsSectionHeight(const SSearchSection& section, float spanWL) {
    if (section.grid) {
        const float ROWS = std::ceil(sc<float>(section.rows.size()) / sc<float>(overviewResultsGridCols(section.rows.size(), spanWL)));
        return (OVERVIEW_RESULT_PANELPADL * 2.F) + (ROWS * OVERVIEW_RESULT_CELLHL) + ((ROWS - 1.F) * OVERVIEW_RESULT_ROWGAPL);
    }

    const float N    = sc<float>(section.rows.size());
    const float BODY = (N * OVERVIEW_RESULT_ROWHL) + ((N - 1.F) * OVERVIEW_RESULT_ROWGAPL);
    return (OVERVIEW_RESULT_PANELPADL * 2.F) + std::max(BODY, OVERVIEW_RESULT_HEADERHL);
}

// The height the band is easing TOWARD -- X49 sums the SECTION CARDS rather
// than a flat row count -- still capped by a BUDGET so that however many rows
// the query matches, the desktop strip and the window tiles stay on screen
// (DoD clause 23, whose PLACEMENT half X60 rewrote and whose "the overview
// stays visible" half is unchanged).
// X64. USER-REPORTED: "it should show just like gnome with results in
// categories" -- against a build that showed the apps icons and nothing else.
//
// renderSearch() drops any card that will not fit the band, and the apps grid is
// drawn FIRST, so an overflowing stack loses the PROVIDER categories entirely.
// G5 turned that from a latent ceiling into a daily one by raising the caps from
// 5 SHARED to 6 apps + 5 rows PER PROVIDER.
//
// So the stack is made to FIT before anything measures it: rows are given up
// from the tallest section first, and no section is ever taken below one row.
// Every category survives; the rows given up are what G6's "%d more" reports.
inline void overviewTrimSectionsToFit(std::vector<SSearchSection>& sections, PHLMONITOR monitor) {
    if (sections.empty() || !monitor)
        return;

    const size_t MAX_ROWS = 8;
    for (auto& sec : sections) {
        if (sec.rows.size() > MAX_ROWS) {
            sec.rows.resize(MAX_ROWS);
        }
    }
}

inline float overviewResultsWidthTarget(PHLMONITOR monitor, const std::vector<SSearchSection>& sections) {
    if (!monitor)
        return 0.F;

    // Always the full span once any column exists: hiding empty columns and
    // growing the box as files arrived was the janky expand. Files take the
    // leftover inside a stable outer width.
    bool any = false;
    for (const auto& s : sections) {
        if (!s.rows.empty()) {
            any = true;
            break;
        }
    }
    if (!any)
        return 0.F;

    return overviewResultsSpanW(monitor);
}

inline float overviewResultsBandTarget(PHLMONITOR monitor, const std::vector<SSearchSection>& sections) {
    if (sections.empty() || !monitor)
        return 0.F;

    size_t maxRows = 0;
    for (const auto& sec : sections)
        maxRows = std::max(maxRows, sec.rows.size());
    if (maxRows == 0)
        maxRows = 1;

    const size_t VISIBLE_ROWS = std::clamp<size_t>(maxRows, 1, 8);
    const float CARD_H = 28.F + (sc<float>(VISIBLE_ROWS) * 36.F) + 28.F + (OVERVIEW_RESULT_PANELGAPL * 2.F);

    const float BUDGET = std::min(360.F, sc<float>(monitor->m_size.y) * 0.42F);
    return std::min(CARD_H, BUDGET);
}

inline int overviewFirstAppIndex(const std::vector<SSearchRow>& rows) {
    int firstCol0 = -1;
    for (size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].column != 0)
            continue;
        if (firstCol0 < 0)
            firstCol0 = sc<int>(i);
        if (rows[i].app && overviewWindowForApp(*rows[i].app))
            return sc<int>(i);
    }
    if (firstCol0 >= 0)
        return firstCol0;
    return rows.empty() ? -1 : 0;
}

// The axis helpers. They used to be defined at the bottom of this file with a
// forward declaration up here, because getOverviewWindowBox needs them; as
// one-line forwarders they can simply be defined where they are first needed.
inline Vector2D axisOffsetVector(float offset, ScrollOverview::Config::ELayout layout) {
    return OvGeom::axisOffsetVector(offset, axisOf(layout));
}

inline float axisValue(const Vector2D& vector, ScrollOverview::Config::ELayout layout) {
    return OvGeom::axisValue(vector, axisOf(layout));
}

inline float axisSize(const Vector2D& size, ScrollOverview::Config::ELayout layout) {
    return OvGeom::axisSize(size, axisOf(layout));
}

// THE transform, and the reason B1 holds: render, hit-test and drag all place a
// tile by calling this. The arithmetic itself is OverviewGeometry's and is
// unit-tested in scripts/geometry_test.cpp; what stays here is the monitor
// lookup, so no call site had to learn a new vocabulary.
inline CBox getOverviewBox(CBox box, PHLMONITOR monitor, float scale, const Vector2D& viewOffset, float offset, ScrollOverview::Config::ELayout layout,
                           bool round = true) {
    return OvGeom::overviewBox(box, viewportOf(monitor, scale), scale, viewOffset, offset, axisOf(layout), round);
}

inline CBox getOverviewGlobalBox(CBox globalBox, PHLMONITOR monitor, float scale, const Vector2D& viewOffset, float offset, ScrollOverview::Config::ELayout layout,
                                 bool round = true) {
    return OvGeom::overviewGlobalBox(globalBox, viewportOf(monitor, scale), scale, viewOffset, offset, axisOf(layout), round);
}

// X22: the top chrome -- search pill, results panel, workspace strip -- is
// STATIC SCREEN FURNITURE. It goes through the SAME transform as the cards but
// with a zero view offset, so it keeps the open/close scale (and with it the
// inset from the monitor's top edge that keeps it clear of the layer-shell bar
// -- X10's occlusion finding stands) while refusing to pan with the desktops.
inline const Vector2D OVERVIEW_CHROME_NO_PAN = {};

inline CBox getOverviewWindowBox(const PHLWINDOW& window, PHLMONITOR monitor, float scale, const Vector2D& viewOffset, float offset, ScrollOverview::Config::ELayout layout,
                                 bool round = true, const CBox* geometryOverride = nullptr) {
    if (!window)
        return {};

    // OVERVIEW_CPP_SPEC 13.5 X17. geometryOverride lets a caller push a SNAPSHOT
    // of a window's desktop geometry through the grid transform below. Everything
    // the user can see or hit goes through this function; anything that maps raw
    // desktop geometry itself lands somewhere the tile is not.
    auto geometry = geometryOverride ? *geometryOverride : window->geometricBox(Desktop::View::IGeometric::GEOMETRIC_CURRENT);

    // F3-b outbound. It has just stopped matching, so it owns no slot and
    // overviewGridSlot says no, but it is still drawn on the active card for one
    // release. `geometry` already IS its real rect on its own desktop, so easing
    // to it means the frame the release expires draws the identical box down the
    // ordinary path below and the handover is invisible. The offset carries the
    // direction that desktop lies in, in RENDERED px against logical geometry,
    // hence the divide -- same conversion as the inbound seed.
    //
    // `exiting` is the whole difference between this and the branch below: the
    // destination is OFF THE MONITOR, so the entrance curve would spend the
    // animation getting there and leave one or two visible frames behind it.
    // See gridReflowEase for the measurement.
    if (overviewGridReleasing(window, monitor)) {
        CBox home = geometry;
        if (const float UNIT = scale * std::max(monitor->m_scale, 0.01F); UNIT > 0.01F && std::abs(offset) > 0.01F)
            home.translate(axisOffsetVector(offset / UNIT, layout));

        geometry = overviewGridLerpBox(geometry, gridReflowBox(window, home, nullptr, false, true), overviewGridProgress(scale));
        offset   = 0.F;
    } else if (CBox slot; overviewGridSlot(window, monitor, slot)) {
        const bool PROMOTED = overviewGridPromoted(window, monitor);

        // F3-b-i: a promoted tile enters from the place it occupies on its own
        // desktop -- which is exactly the per-workspace offset suppressed below,
        // so it carries the direction that desktop lies in for free. That offset
        // is in RENDERED px while `slot` is logical, hence the divide.
        CBox        enterFrom;
        const CBox* enterPtr = nullptr;
        if (PROMOTED && std::abs(offset) > 0.01F) {
            if (const float UNIT = scale * std::max(monitor->m_scale, 0.01F); UNIT > 0.01F) {
                enterFrom = slot;
                enterFrom.translate(axisOffsetVector(offset / UNIT, layout));
                enterPtr = &enterFrom;
            }
        }

        geometry = overviewGridLerpBox(geometry, gridReflowBox(window, slot, enterPtr, PROMOTED), overviewGridProgress(scale));

        // The active card sits at offset 0. A promoted tile must ignore the
        // caller's per-workspace offset or it lands off-screen beside it.
        if (PROMOTED)
            offset = 0.F;
    }

    return getOverviewGlobalBox(geometry, monitor, scale, viewOffset, offset, layout, round);
}

inline CBox getOverviewDragWindowBox(const PHLWINDOW& window, PHLMONITOR monitor, float scale, const Vector2D& viewOffset, float offset, ScrollOverview::Config::ELayout layout,
                                     bool round = true) {
    if (!window)
        return {};

    const auto TARGET = window->layoutTarget();
    if (window->m_group && TARGET)
        return getOverviewGlobalBox(TARGET->position(), monitor, scale, viewOffset, offset, layout, round);

    return getOverviewWindowBox(window, monitor, scale, viewOffset, offset, layout, round);
}

inline float overviewPointDistanceSqToBox(const Vector2D& point, const CBox& box) {
    return OvGeom::pointDistanceSqToBox(point, box);
}

inline Vector2D getOverviewMousePosLocal(PHLMONITOR monitor) {
    if (!monitor)
        return {};

    return (g_pInputManager->getMouseCoordsInternal() - monitor->m_position) * monitor->m_scale;
}

inline bool isOverviewPointerOnMonitor(PHLMONITOR monitor) {
    const auto overview = monitor ? scrollOverviewAt(g_pInputManager->getMouseCoordsInternal()) : SP<IOverview>{};
    return overview && overview->pMonitor.lock() == monitor;
}

inline CBox getOverviewWorkspaceBox(PHLMONITOR monitor, float scale, const Vector2D& viewOffset, float offset, ScrollOverview::Config::ELayout layout) {
    return OvGeom::overviewWorkspaceBox(viewportOf(monitor, scale), scale, viewOffset, offset, axisOf(layout));
}

inline CBox getWorkspaceGlobalBox(PHLWORKSPACE workspace, PHLMONITOR fallbackMonitor) {
    const auto MONITOR = workspace && workspace->m_monitor ? workspace->m_monitor.lock() : fallbackMonitor;
    if (!MONITOR)
        return {};

    return {MONITOR->m_position, MONITOR->m_size};
}

inline CBox centerBoxInWorkspace(CBox box, PHLWORKSPACE workspace, PHLMONITOR fallbackMonitor) {
    return OvGeom::centerBoxIn(box, getWorkspaceGlobalBox(workspace, fallbackMonitor));
}

inline CBox clampBoxToWorkspace(CBox box, PHLWORKSPACE workspace, PHLMONITOR fallbackMonitor, float margin = 0.F) {
    return OvGeom::clampBoxTo(box, getWorkspaceGlobalBox(workspace, fallbackMonitor), margin);
}

inline CBox resizedOverviewBoxFromCorner(const CBox& originalBox, const Vector2D& delta, Layout::eRectCorner corner, const Vector2D& minSizePx,
                                         const std::optional<Vector2D>& maxSizePx) {
    return OvGeom::resizedBoxFromCorner(originalBox, delta, sc<uint8_t>(corner), minSizePx, maxSizePx);
}

inline CBox clampResizedOverviewBoxToWorkspace(const CBox& box, const CBox& workspaceBox, Layout::eRectCorner corner, float marginPx) {
    return OvGeom::clampResizedBoxTo(box, workspaceBox, sc<uint8_t>(corner), marginPx);
}

inline bool overviewBoxIntersectsMonitor(const CBox& box, PHLMONITOR monitor) {
    return monitor && OvGeom::boxIntersectsViewport(box, viewportOf(monitor));
}

inline bool overviewBoxFullyVisibleOnMonitor(const CBox& box, PHLMONITOR monitor) {
    return monitor && OvGeom::boxFullyVisibleInViewport(box, viewportOf(monitor));
}

inline double overviewBoxIntersectionArea(const CBox& a, const CBox& b) {
    return OvGeom::boxIntersectionArea(a, b);
}

inline double overviewBoxArea(const CBox& box) {
    return OvGeom::boxArea(box);
}

inline double overviewBoxCenterDistanceSquared(const CBox& a, const CBox& b) {
    return OvGeom::boxCenterDistanceSquared(a, b);
}

inline CBox getPinnedFloatingOverviewWindowBox(PHLMONITOR monitor, const PHLWINDOW& window, float targetOverviewScale, float animationProgress, float* renderScale) {
    if (!monitor || !window) {
        if (renderScale)
            *renderScale = 1.F;
        return {};
    }

    const auto MONITORSCALE = monitor->m_scale;
    const auto WINDOWSIZE   = window->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT) * MONITORSCALE;
    if (WINDOWSIZE.x <= 0 || WINDOWSIZE.y <= 0) {
        if (renderScale)
            *renderScale = 1.F;
        return {};
    }

    const CBox WINDOWBOX = {(window->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT) - monitor->m_position) * MONITORSCALE, WINDOWSIZE};
    const auto MONITORW  = sc<float>(monitor->m_size.x * MONITORSCALE);
    const auto MONITORH  = sc<float>(monitor->m_size.y * MONITORSCALE);

    const std::array<CBox, 4> QUADRANTS = {
        CBox{0.F, 0.F, MONITORW / 2.F, MONITORH / 2.F},
        CBox{MONITORW / 2.F, 0.F, MONITORW / 2.F, MONITORH / 2.F},
        CBox{0.F, MONITORH / 2.F, MONITORW / 2.F, MONITORH / 2.F},
        CBox{MONITORW / 2.F, MONITORH / 2.F, MONITORW / 2.F, MONITORH / 2.F},
    };

    size_t bestQuadrant = 0;
    double bestArea     = -1.0;
    for (size_t i = 0; i < QUADRANTS.size(); ++i) {
        const auto AREA = overviewBoxIntersectionArea(WINDOWBOX, QUADRANTS[i]);
        if (AREA <= bestArea)
            continue;

        bestQuadrant = i;
        bestArea     = AREA;
    }

    const bool RIGHT  = bestQuadrant == 1 || bestQuadrant == 3;
    const bool BOTTOM = bestQuadrant == 2 || bestQuadrant == 3;

    const auto FULLBOX = monitor->logicalBox();
    const auto WORKBOX = monitor->logicalBoxMinusReserved();

    const float RESERVEDLEFT   = std::max(0.F, sc<float>(WORKBOX.x - FULLBOX.x)) * MONITORSCALE;
    const float RESERVEDTOP    = std::max(0.F, sc<float>(WORKBOX.y - FULLBOX.y)) * MONITORSCALE;
    const float RESERVEDRIGHT  = std::max(0.F, sc<float>((FULLBOX.x + FULLBOX.width) - (WORKBOX.x + WORKBOX.width))) * MONITORSCALE;
    const float RESERVEDBOTTOM = std::max(0.F, sc<float>((FULLBOX.y + FULLBOX.height) - (WORKBOX.y + WORKBOX.height))) * MONITORSCALE;

    const auto WORKSPACEGAP       = sc<float>(ScrollOverview::Config::getWorkspaceGap()) * MONITORSCALE;
    const auto RESERVEDWIDTH      = RIGHT ? RESERVEDRIGHT : RESERVEDLEFT;
    const auto CALCULATEDWIDTH    = std::max(1.F, sc<float>((MONITORW - MONITORW * targetOverviewScale) / 2.F - 2.F * WORKSPACEGAP - RESERVEDWIDTH));
    const auto CALCULATEDSCALE    = CALCULATEDWIDTH / sc<float>(WINDOWSIZE.x);
    const auto WINDOWRENDERSCALE  = std::min(1.F, std::max(CALCULATEDSCALE, targetOverviewScale));
    const auto PROGRESS           = std::clamp(animationProgress, 0.F, 1.F);
    const auto CURRENTRENDERSCALE = 1.F + (WINDOWRENDERSCALE - 1.F) * PROGRESS;
    const auto TARGETWIDTH       = sc<float>(WINDOWSIZE.x) * CURRENTRENDERSCALE;
    const auto TARGETHEIGHT      = sc<float>(WINDOWSIZE.y) * CURRENTRENDERSCALE;

    if (renderScale)
        *renderScale = CURRENTRENDERSCALE;

    const float X = RIGHT ? MONITORW - TARGETWIDTH - WORKSPACEGAP - RESERVEDRIGHT : WORKSPACEGAP + RESERVEDLEFT;
    const float Y = BOTTOM ? MONITORH - TARGETHEIGHT - WORKSPACEGAP - RESERVEDBOTTOM : WORKSPACEGAP + RESERVEDTOP;

    CBox box = {{X, Y}, {TARGETWIDTH, TARGETHEIGHT}};

    box.x = WINDOWBOX.x + (box.x - WINDOWBOX.x) * PROGRESS;
    box.y = WINDOWBOX.y + (box.y - WINDOWBOX.y) * PROGRESS;
    box.round();

    return box;
}

inline std::chrono::milliseconds getOverviewIdleFrameInterval() {
    const int fps = std::clamp<int>(ScrollOverview::Config::getValue<int>("misc:render_unfocused_fps"), 1, 240);
    return std::chrono::milliseconds(std::max(1, 1000 / fps));
}

inline constexpr std::chrono::milliseconds OVERVIEW_WINDOW_FRAME_INTERVAL = std::chrono::milliseconds(33);

// Card shadow + corner-rounding helpers moved to OverviewChrome.hpp
// (included at the top of this file) so the dock can use them without
// pulling this whole header. They stay in namespace OvInternal.

inline float getWorkspaceRenderedPitch(PHLMONITOR monitor, float scale, ScrollOverview::Config::ELayout layout) {
    return OvGeom::workspaceRenderedPitch(viewportOf(monitor), scale, axisOf(layout), sc<float>(ScrollOverview::Config::getWorkspaceGap()));
}

inline float getWorkspaceLogicalPitch(PHLMONITOR monitor, float scale, ScrollOverview::Config::ELayout layout) {
    return OvGeom::workspaceLogicalPitch(viewportOf(monitor), scale, axisOf(layout), sc<float>(ScrollOverview::Config::getWorkspaceGap()));
}

inline bool overviewBoxesEqual(const CBox& a, const CBox& b) {
    return OvGeom::boxesEqual(a, b);
}

inline bool moveOverviewScrollingTargetToWorkspaceEdge(const SP<Layout::ITarget>& target, int side);
inline bool moveOverviewScrollingTargetNextToWindow(const SP<Layout::ITarget>& target, const PHLWINDOW& anchor, const std::string& direction);

inline double overviewTargetDirectionDelta(const SP<Layout::ITarget>& target, const PHLWINDOW& anchor, const std::string& direction) {
    if (!target || !anchor || !anchor->layoutTarget())
        return 0.0;

    const auto TARGETCENTER = target->position().middle();
    const auto ANCHORCENTER = anchor->layoutTarget()->position().middle();

    if (direction == "l" || direction == "r")
        return TARGETCENTER.x - ANCHORCENTER.x;

    return TARGETCENTER.y - ANCHORCENTER.y;
}

inline void moveOverviewTargetOneStep(const SP<Layout::ITarget>& target, const std::string& direction) {
    if (!target || direction.empty())
        return;

    g_layoutManager->moveInDirection(target, direction, true);
}

inline Layout::Tiled::CScrollingAlgorithm* overviewScrollingAlgorithmForTarget(const SP<Layout::ITarget>& target) {
    if (!target || !target->space() || !target->space()->algorithm())
        return nullptr;

    return dc<Layout::Tiled::CScrollingAlgorithm*>(target->space()->algorithm()->m_tiled.get());
}

inline Layout::Tiled::CScrollingAlgorithm* overviewScrollingAlgorithmForWorkspace(const PHLWORKSPACE& workspace) {
    if (!workspace || !workspace->m_space || !workspace->m_space->algorithm())
        return nullptr;

    return dc<Layout::Tiled::CScrollingAlgorithm*>(workspace->m_space->algorithm()->m_tiled.get());
}

inline bool isWorkspaceScrolling(const PHLWORKSPACE& workspace) {
    return overviewScrollingAlgorithmForWorkspace(workspace) != nullptr;
}

inline CBox getOverviewWorkspaceUsableBox(const PHLWORKSPACE& workspace, PHLMONITOR monitor, float scale, const Vector2D& viewOffset, float offset,
                                          ScrollOverview::Config::ELayout layout) {
    if (!workspace || !monitor)
        return {};

    if (const auto ALGO = overviewScrollingAlgorithmForWorkspace(workspace); ALGO && ALGO->m_scrollingData && ALGO->m_scrollingData->controller) {
        const auto USABLE = ALGO->usableArea();
        return getOverviewBox(USABLE, monitor, scale, viewOffset, offset, layout);
    }

    if (!workspace->m_space)
        return getOverviewWorkspaceBox(monitor, scale, viewOffset, offset, layout);

    auto USABLE = workspace->m_space->workArea();
    USABLE.translate(-monitor->m_position);
    USABLE.w = std::max(USABLE.w, 1.0);
    USABLE.h = std::max(USABLE.h, 1.0);

    return getOverviewBox(USABLE, monitor, scale, viewOffset, offset, layout);
}

inline double clampOverviewScrollingOffset(Layout::Tiled::CScrollingAlgorithm* algo, double offset) {
    if (!algo || !algo->m_scrollingData)
        return offset;

    const double MAXOFFSET = std::max(0.0, algo->m_scrollingData->maxWidth() - algo->primaryViewportSize());
    return std::clamp(offset, 0.0, MAXOFFSET);
}

inline bool moveOverviewScrollingTargetToWorkspaceEdge(const SP<Layout::ITarget>& target, int side) {
    if (!target || side == 0)
        return false;

    const auto ALGO = overviewScrollingAlgorithmForTarget(target);
    if (!ALGO || !ALGO->m_scrollingData)
        return false;

    const auto TDATA = ALGO->dataFor(target);
    if (!TDATA)
        return false;

    const auto SRC_COL = TDATA->column.lock();
    if (!SRC_COL)
        return false;

    const auto SRC_COL_WIDTH = SRC_COL->getColumnWidth();
    SRC_COL->remove(target);

    const int64_t INSERT_AFTER = side < 0 ? -1 : sc<int64_t>(ALGO->m_scrollingData->columns.size()) - 1;
    const auto    NEW_COL      = ALGO->m_scrollingData->add(INSERT_AFTER, SRC_COL_WIDTH);
    NEW_COL->add(TDATA);
    ALGO->m_scrollingData->centerOrFitCol(NEW_COL);
    ALGO->m_scrollingData->recalculate();
    ALGO->focusTargetUpdate(target);

    return true;
}

inline bool moveOverviewScrollingTargetNextToWindow(const SP<Layout::ITarget>& target, const PHLWINDOW& anchor, const std::string& direction) {
    if (!target || !anchor || !anchor->layoutTarget() || direction.empty())
        return false;

    const auto ALGO = overviewScrollingAlgorithmForTarget(target);
    if (!ALGO || !ALGO->m_scrollingData)
        return false;

    const auto TDATA      = ALGO->dataFor(target);
    const auto ANCHORDATA = ALGO->dataFor(anchor->layoutTarget());
    if (!TDATA || !ANCHORDATA)
        return false;

    const auto SRC_COL    = TDATA->column.lock();
    const auto ANCHOR_COL = ANCHORDATA->column.lock();
    if (!SRC_COL || !ANCHOR_COL)
        return false;

    const bool PRIMARYHORIZONTAL = ALGO->m_scrollingData->controller && ALGO->m_scrollingData->controller->isPrimaryHorizontal();
    const bool MOVINGCOLUMN      = PRIMARYHORIZONTAL ? direction == "l" || direction == "r" : direction == "u" || direction == "d";
    const bool STACKINGCOLUMN    = PRIMARYHORIZONTAL ? direction == "u" || direction == "d" : direction == "l" || direction == "r";

    if (MOVINGCOLUMN) {
        const auto SRC_COL_WIDTH = SRC_COL->getColumnWidth();
        SRC_COL->remove(target);

        const auto ANCHOR_COL_IDX = ALGO->m_scrollingData->idx(ANCHOR_COL);
        if (ANCHOR_COL_IDX < 0)
            return false;

        const bool    INSERT_BEFORE = direction == "l" || direction == "u";
        const int64_t INSERT_AFTER  = INSERT_BEFORE ? ANCHOR_COL_IDX - 1 : ANCHOR_COL_IDX;
        const auto    NEW_COL      = ALGO->m_scrollingData->add(INSERT_AFTER, SRC_COL_WIDTH);
        NEW_COL->add(TDATA);
        ALGO->m_scrollingData->centerOrFitCol(NEW_COL);
        ALGO->m_scrollingData->recalculate();
        ALGO->focusTargetUpdate(target);

        return true;
    }

    if (!STACKINGCOLUMN)
        return false;

    SRC_COL->remove(target);

    const auto ANCHOR_IDX    = ANCHOR_COL->idx(anchor->layoutTarget());
    const bool INSERT_BEFORE = direction == "l" || direction == "u";
    const int  INSERT_AFTER  = INSERT_BEFORE ? sc<int>(ANCHOR_IDX) - 1 : sc<int>(ANCHOR_IDX);
    ANCHOR_COL->add(TDATA, INSERT_AFTER);
    ALGO->m_scrollingData->centerOrFitCol(ANCHOR_COL);
    ALGO->m_scrollingData->recalculate();
    ALGO->focusTargetUpdate(target);

    return true;
}

inline void moveOverviewTargetNextToWindow(const SP<Layout::ITarget>& target, const PHLWINDOW& anchor, const std::string& direction) {
    if (!target || !anchor || direction.empty())
        return;

    if (moveOverviewScrollingTargetNextToWindow(target, anchor, direction))
        return;

    const auto PREVFALLBACK = ScrollOverview::Config::getValue<int>("binds:window_direction_monitor_fallback");
    ScrollOverview::Config::setValue("binds:window_direction_monitor_fallback", 0);
    auto restoreFallback   = Hyprutils::Utils::CScopeGuard([PREVFALLBACK] {
        ScrollOverview::Config::setValue("binds:window_direction_monitor_fallback", PREVFALLBACK);
    });

    const bool WANT_NEGATIVE = direction == "l" || direction == "u";
    const auto FORWARD       = direction;
    const auto BACKWARD      = direction == "l" ? "r" : direction == "r" ? "l" : direction == "u" ? "d" : "u";

    auto isDesiredSide = [&] {
        const auto DELTA = overviewTargetDirectionDelta(target, anchor, direction);
        return WANT_NEGATIVE ? DELTA < 0.0 : DELTA > 0.0;
    };

    for (size_t i = 0; i < 64 && isDesiredSide(); ++i) {
        const auto WORKSPACE = target->workspace();
        const auto BEFORE    = target->position();

        moveOverviewTargetOneStep(target, BACKWARD);

        if (target->workspace() != WORKSPACE || overviewBoxesEqual(BEFORE, target->position()))
            break;

        if (!isDesiredSide()) {
            moveOverviewTargetOneStep(target, FORWARD);
            return;
        }
    }

    for (size_t i = 0; i < 64 && !isDesiredSide(); ++i) {
        const auto WORKSPACE = target->workspace();
        const auto BEFORE    = target->position();

        moveOverviewTargetOneStep(target, FORWARD);

        if (target->workspace() != WORKSPACE || overviewBoxesEqual(BEFORE, target->position()))
            break;
    }
}

// X6: Hyprland's optimized blur FB is baked from the background+bottom pass, so
// it only ever holds the WALLPAPER -- never the overview backdrop, which this
// plugin draws later in its own pass. Layer-shell frost sampled from it is
// therefore stale the whole time the overview is open. Windows already dodge
// this via blockOverviewWindowBlurOptimization(); layers need the same.
// ponytail: blanket -- every top/overlay surface goes live for as long as the
// overview is open, measured +0.9pp GPU busy. If that ever matters, block only
// the surfaces whose box intersects the backdrop, or bake a second optimized FB
// that includes it instead of paying per-frame.
inline void blockOverviewLayerBlurOptimization(size_t firstElement) {
    if (!ScrollOverview::Config::getValue<bool>("plugin:hyprdrive:live_layer_frost"))
        return;

    auto& passElements = g_pHyprRenderer->m_renderPass.m_passElements;
    for (size_t i = firstElement; i < passElements.size(); ++i) {
        const auto& passElement = passElements[i];
        if (!passElement.element)
            continue;

        auto* surfacePassElement = dc<CSurfacePassElement*>(passElement.element.get());
        if (!surfacePassElement || !surfacePassElement->m_data.pLS)
            continue;

        surfacePassElement->m_data.blockBlurOptimization = true;
    }
}

inline void renderOverviewLayerLevel(PHLMONITOR monitor, uint32_t layer, const CBox& workspaceBox, float renderScale, const Time::steady_tp& now, float alpha = 1.F) {
    if (!monitor)
        return;

    bool pushedRenderHints = false;
    const bool MODULATEALPHA = alpha < 0.999F;

    for (auto const& ls : monitor->m_layerSurfaceLayers[layer]) {
        const auto LAYER = ls.lock();
        if (!Desktop::View::validMapped(LAYER))
            continue;

        if (!pushedRenderHints) {
            Render::SRenderModifData modif;
            modif.modifs.emplace_back(Render::SRenderModifData::RMOD_TYPE_SCALE, renderScale);
            modif.modifs.emplace_back(Render::SRenderModifData::RMOD_TYPE_TRANSLATE, workspaceBox.pos());

            g_pHyprRenderer->m_renderPass.add(makeUnique<CRendererHintsPassElement>(CRendererHintsPassElement::SData{.renderModif = modif}));
            pushedRenderHints = true;
        }

		auto& lsAlpha = LAYER->alpha()[Desktop::View::LS_ALPHA_FADE];
        float previousAlpha = 1.F;
        if (MODULATEALPHA && lsAlpha->value()) {
			previousAlpha = lsAlpha->value();
			lsAlpha->setValueAndWarp(previousAlpha * std::clamp(alpha, 0.F, 1.F));
		}

        g_pHyprRenderer->renderLayer(LAYER, monitor, now);

        if (MODULATEALPHA && lsAlpha->value())
			lsAlpha->setValueAndWarp(previousAlpha);
    }

    if (pushedRenderHints)
        g_pHyprRenderer->m_renderPass.add(makeUnique<CRendererHintsPassElement>(CRendererHintsPassElement::SData{.renderModif = Render::SRenderModifData{}}));
}

inline void focusOverviewFullscreenWindowIfActiveWorkspace(const PHLWINDOW& fullscreenWindow_, const PHLWORKSPACE& workspace, PHLMONITOR monitor) {
    const auto FULLSCREENWINDOW = getOverviewWindowToShow(fullscreenWindow_);

    if (!monitor || !workspace || workspace != monitor->m_activeWorkspace || !validMapped(FULLSCREENWINDOW) || FULLSCREENWINDOW->m_workspace != workspace)
        return;

    if (Desktop::focusState()->window() == FULLSCREENWINDOW)
        return;

    Desktop::focusState()->fullWindowFocus(FULLSCREENWINDOW, Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE, nullptr, true);
}

inline void syncWorkspaceGeometry(const PHLWORKSPACE& workspace) {
    if (!workspace || !workspace->m_space)
        return;

    for (const auto& targetRef : workspace->m_space->targets()) {
        const auto TARGET = targetRef.lock();
        if (TARGET)
            TARGET->warpPositionSize();
    }
}

inline PHLWINDOW getOverviewFullscreenVisibilityWindow(const PHLWORKSPACE& workspace, const PHLWINDOW& fallback) {
    const auto FULLSCREENWINDOW = workspace ? getOverviewWindowToShow(Fullscreen::controller()->getFullscreenWindow(workspace)) : PHLWINDOW{};

    if (shouldShowOverviewWindow(FULLSCREENWINDOW) && FULLSCREENWINDOW->m_workspace == workspace)
        return FULLSCREENWINDOW;

    return getOverviewWindowToShow(fallback);
}

inline float hyprlerp(const float& from, const float& to, const float perc) {
    return OvGeom::hyprlerp(from, to, perc);
}

inline Vector2D hyprlerp(const Vector2D& from, const Vector2D& to, const float perc) {
    return OvGeom::hyprlerp(from, to, perc);
}

// THE FULLSCREEN PATH: a fullscreen window on a non-scrolling desktop drawn,
// hit-tested, navigated and framed ON ITS OWN, its tiled neighbours skipped.
// That is exactly how such a desktop looks, so it is right when the overview
// draws a desktop as a scaled picture of itself.
//
// It is WRONG in mission control, where every window owns a grid slot --
// overviewGridWindows has no fullscreen rule, so the neighbours still got their
// slots and the skip left them as empty holes: "fullscreen apps make the other
// windows invisible" (user, 2026-09-12). Eight sites each carried their own copy
// of the test, so the mode was never asked at any of them; this is now the only
// place it is decided.
//
// The fullscreen window when that path applies; null when every window gets its
// own tile.
inline PHLWINDOW overviewFullscreenPathWindow(const PHLWORKSPACE& workspace) {
    if (!workspace || isWorkspaceScrolling(workspace) || ScrollOverview::Config::getMissionControl())
        return nullptr;

    const auto FULLSCREENWINDOW = getOverviewWindowToShow(Fullscreen::controller()->getFullscreenWindow(workspace));
    return shouldShowOverviewWindow(FULLSCREENWINDOW) && FULLSCREENWINDOW->m_workspace == workspace ? FULLSCREENWINDOW : nullptr;
}

} // namespace OvInternal

// The trailing `using namespace OvInternal;` used to live here so the moved code
// needed no edits -- but at file scope in a header it leaks the whole namespace
// into every TU that includes this. Each consumer opts in for itself now (right
// after the include), so a future includer decides its own qualification.

