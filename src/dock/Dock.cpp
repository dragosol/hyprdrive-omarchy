// The dock: its small renderers, render(), the frame pump and damage, and the
// entry points other modules call. Split from one file; see DockParts.hpp.
#include "DockParts.hpp"

namespace Hyprdrive::Dock {
using namespace DockDetail;
namespace DockDetail {
bool foldersAnywhere() {
    return Config::getValue<bool>(KEY_FOLDERS_ANYWHERE);
}
// The pinned folder a favourites entry names, when folders may sit there.
const Stacks::SStack* placedFolder(const std::string& id) {
    if (!id.starts_with("folder:/") || !foldersAnywhere())
        return nullptr;
    for (const auto& S : Stacks::all())
        if (S.isDir && S.path == std::string_view(id).substr(7))
            return &S;
    return nullptr;
}
// Does a favourites entry draw a cell? An app this machine has, or a placed
// folder. pinAt counts positions by this, so an app and a folder move alike.
bool drawnFavorite(const std::string& id) {
    return AppSearch::entryForId(id) || placedFolder(id);
}

bool overlayUp() {
    return Config::getValue<bool>(KEY_HOT_KEYS) && Config::getValue<bool>(KEY_HOTKEY_OVERLAY) && Time::steadyNow() < g_overlayUntil;
}

// X34. Reveal the dock while the overlay is up, on the same `want` the pressure
// barrier feeds. The reference calls `dock._show()` and then leans on the
// timeout's `_updateDashVisibility()` to put it back -- so the hold ENDS with
// the overlay rather than being undone separately, which is what a deadline
// read once per frame gives for free.
//
// Gated on `hotkeys-show-dock`, and the reference gates it again on the dock
// being hideable at all (`_intellihideIsEnabled || _autohideIsEnabled`) -- with
// autohide off the pill is already up and there is nothing to reveal.
bool overlayHoldsDock() {
    return Config::getValue<bool>(KEY_HOT_KEYS) && Config::getValue<bool>(KEY_HOTKEY_DOCK) && Config::getValue<bool>(KEY_AUTOHIDE) && Time::steadyNow() < g_overlayUntil;
}


// `Clutter.AnimationMode.EASE_OUT_QUAD`.
float easeOutQuad(float t) {
    return 1.F - (1.F - t) * (1.F - t);
}


float bounceCurve(float t) {
    if (t <= 0.F || t >= 1.F)
        return 0.F;

    return t < BOUNCE_UP_SHARE ? houseEase(true, t / BOUNCE_UP_SHARE) : 1.F - houseEase(false, (t - BOUNCE_UP_SHARE) / (1.F - BOUNCE_UP_SHARE));
}

// [FLIGHT] flightProgress moved to FlightController.cpp. lerpBox stays below --
// the hover panel eases with it, and the flight module keeps its own copy.

CBox lerpBox(const CBox& a, const CBox& b, float t) {
    return CBox{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.w + (b.w - a.w) * t, a.h + (b.h - a.h) * t};
}

// Pure, so the proportions can be asserted without a compositor. `icon-size-fixed`
// is false in the reference, which is what licenses the shrink: over the
// height-fraction cap D2D makes the icons smaller rather than overflowing.
//
// `units` is the width of every slot expressed in icon-widths: 1.0 per app icon,
// the clamped aspect ratio per preview. With no previews it equals `slots` and
// this is byte-for-byte the X27 formula.
SMetrics metrics(size_t slots, float units, float iconMax, float maxWidth) {
    if (slots == 0)
        return {};

    const float N      = sc<float>(slots);
    const float CHROME = 2.F * CONTAINER_PAD_X + 2.F * ITEM_MARGIN_X * N;
    const float AVAIL  = maxWidth - CHROME;

    SMetrics out;
    out.icon   = std::max(8.F, std::min(iconMax, AVAIL / std::max(units, 0.01F)));
    out.width  = CHROME + out.icon * units;
    out.height = ITEM_PAD_TOP + out.icon + ITEM_PAD_BOTTOM;
    return out;
}


// How far the hover cell sits inside the pill, on every side. The icon is
// ITEM_PAD_TOP from the pill's top and CONTAINER_PAD_X + ITEM_MARGIN_X from its
// left, and those are equal by construction, so one number covers all four.
float hoverInset() {
    return ITEM_PAD_TOP - HOVER_PAD;
}

// The hovered cell, not the icon: GNOME's highlight is the .app-well-app box,
// which is the icon plus its padding. Symmetric now that the vertical padding is.
CBox hoverBox(const CBox& iconBox, float scale) {
    return iconBox.copy().expand(HOVER_PAD * std::max(scale, 0.01F)).round();
}

// `dotSize` and `dotColor` are the caller's: both were read per ITEM -- a config
// lookup that hashes its key string, and a theme colour that re-reads the
// palette -- inside a loop over every icon of every monitor every frame (user,
// 2026-09-14). They do not vary within a pass.
void renderRunningDot(PHLMONITOR monitor, const SLayout& layout, const SItem& item, const STally& tally, int dotSize, const CHyprColor& dotColor) {
    const float SCALE = std::max(monitor->m_scale, 0.01F);
    const float SIZE  = std::max(1.F, sc<float>(dotSize) * SCALE);
    const float CX    = sc<float>(item.iconBox.x) + sc<float>(item.iconBox.width) / 2.F;
    const float TOP   = sc<float>(item.iconBox.y) + sc<float>(item.iconBox.height) + DOT_MARGIN_B * SCALE;

    const CHyprColor DOTCOL = dotColor;

    // Z12 (user, 2026-08-07, overrides X31's measured single dot): one dot per
    // WINDOW. Row one keeps today's exact position and grows outward from the
    // icon's centre; overflow wraps to a second row underneath; two rows max,
    // and windows beyond that are simply not shown.
    //
    // V6 scoped the CLICK to the active desktop and left this GLOBAL, on
    // purpose: the dots answer "is this app running, and how much of it" --
    // which is what makes it worth walking to another desktop for -- while the
    // click answers "do something here". A dot count that emptied out as you
    // walked away would be the reference's `isolate-workspaces`, which is a
    // config key of its own (dock:isolate_workspaces, default false) and is not
    // what the amendment asked for. `interesting()` is what that key moves, so
    // turning it on scopes both together, exactly as the reference does.
    size_t count = std::max<size_t>(itemWindowCount(item, tally), 1);

    const float  GAP    = 3.F * SCALE;
    const size_t PERROW = std::max<size_t>(1, sc<size_t>((sc<float>(item.iconBox.width) + GAP) / (SIZE + GAP)));
    count               = std::min(count, PERROW * 2);
    const size_t ROW1   = std::min(count, PERROW);
    const size_t ROW2   = count - ROW1;

    const auto drawRow = [&](size_t n, float top) {
        const float SPAN = sc<float>(n) * SIZE + sc<float>(n - 1) * GAP;
        for (size_t i = 0; i < n; ++i) {
            CRectPassElement::SRectData dot;
            dot.box   = CBox{std::round(CX - SPAN / 2.F + sc<float>(i) * (SIZE + GAP)), std::round(top), std::round(SIZE), std::round(SIZE)};
            dot.color = DOTCOL;
            // `border-radius: 5px` on a 5px box: a circle. Truncated, not
            // rounded -- a radius even slightly OVER half the box collapses the
            // circle into a diamond (seen on screen 2026-08-03).
            dot.round         = sc<int>(SIZE / 2.F);
            dot.roundingPower = 2.F;
            g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(dot));
        }
    };

    drawRow(ROW1, TOP);
    if (ROW2)
        drawRow(ROW2, TOP + SIZE + std::max(1.F, 2.F * SCALE));
}

// `_notificationBadgeCountToText`, same thresholds and the same rounding.
std::string badgeText(long long count) {
    if (count <= 9999)
        return std::to_string(count);

    char        buf[32] = {};
    const auto  N       = sc<double>(count);
    if (count < 100'000)
        std::snprintf(buf, sizeof(buf), "%.1fk", N / 1e3);
    else if (count < 1'000'000)
        std::snprintf(buf, sizeof(buf), "%.0fk", N / 1e3);
    else if (count < 100'000'000)
        std::snprintf(buf, sizeof(buf), "%.1fM", N / 1e6);
    else if (count < 1'000'000'000)
        std::snprintf(buf, sizeof(buf), "%.0fM", N / 1e6);
    else
        std::snprintf(buf, sizeof(buf), "%.1fB", N / 1e9);

    return buf;
}

void renderBadge(PHLMONITOR monitor, const SItem& item, bool badgesOn) {
    if (!item.uri.empty()) // X35: a location has no class to key a LauncherEntry on
        return;

    const auto COUNT = itemBadge(item, badgesOn);
    if (COUNT <= 0)
        return;

    const float SCALE = std::max(monitor->m_scale, 0.01F);
    const float ICON  = sc<float>(item.iconBox.width) / SCALE; // logical px: what the reference scales by

    // `_updateNotificationBadgeStyle`. The small-icon branch is not dead code --
    // the pill shrinks its icons rather than overflow once it hits the width cap,
    // which is exactly the case D2D's `iconSize < defaultIconSize` covers.
    const float MULT = ICON < BADGE_DEFAULT_ICON ? std::max(24.F, std::min(ICON + ICON * 0.3F, BADGE_DEFAULT_ICON)) / BADGE_DEFAULT_ICON : ICON / BADGE_DEFAULT_ICON;
    const float FONT = std::max(1.F, std::round(MULT * (BADGE_FONT_PT * BADGE_FONT_RATIO / 0.75F)));

    const auto  TEX = OverviewRender::text(badgeText(COUNT), CHyprColor{1.F, 1.F, 1.F, 1.F}, std::max(1, sc<int>(std::round(FONT * SCALE))), false, "", 0, BADGE_WEIGHT);
    if (!TEX || !TEX->ok())
        return;

    const float PADX = std::round(BADGE_PAD_X_EM * FONT * SCALE);
    const float PADY = std::round(BADGE_PAD_Y_EM * FONT * SCALE);
    const float W    = sc<float>(TEX->m_size.x) + 2.F * PADX;
    const float H    = sc<float>(TEX->m_size.y) + 2.F * PADY;

    const float X    = sc<float>(item.iconBox.x) + sc<float>(item.iconBox.width) - W - BADGE_MARGIN * SCALE;
    const float Y    = sc<float>(item.iconBox.y) + BADGE_MARGIN * SCALE;

    CRectPassElement::SRectData bg;
    bg.box   = CBox{std::round(X), std::round(Y), std::round(W), std::round(H)};
    bg.color = CHyprColor{1.F, 0.F, 0.F, 1.F}; // `background-color: red`
    // `border-radius: 1em` -- 1em is the font size, which on a badge this short
    // is always at least half its height, and css clamps a radius to that. So the
    // shape is a capsule, and H/2 is the number that stays one at any count.
    bg.round         = sc<int>(std::round(H / 2.F));
    bg.roundingPower = 2.F;
    g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(bg));

    CTexPassElement::SRenderData text;
    text.tex = TEX;
    text.box = CBox{std::round(X + PADX), std::round(Y + PADY), sc<float>(TEX->m_size.x), sc<float>(TEX->m_size.y)};
    text.a   = 1.F;
    g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(text));
}

// X34. The Hot-Key visual aid. `_updateNumberOverlay` numbers the icons 1..9,
// gives the TENTH a `0` -- the digit on its key, not its ordinal -- and leaves
// every icon past that without one.
//
// Reference divergence worth naming: the reference numbers `getAppIcons()`
// (every child with an `.icon`) but ACTIVATES out of a list filtered on
// `.child.app`, so once X35 puts trash and mounts in the dash the number drawn
// on an icon and the slot its key reaches can drift apart. Here both come from
// `LAYOUT.items`, which is ONE list, so they cannot.
void renderNumberOverlay(PHLMONITOR monitor, const SItem& item, int index) {
    if (!overlayUp() || index >= NUM_HOTKEYS)
        return;

    const int   DIGIT = (index + 1) % 10; // the tenth slot is `0`
    const float SCALE = std::max(monitor->m_scale, 0.01F);
    const float ICON  = sc<float>(item.iconBox.width) / SCALE; // logical px, as the reference measures

    const float FONT  = std::round(std::max(NUMBER_FONT_MIN, NUMBER_FONT_RATIO * ICON));
    const float SIZE  = std::round(FONT * NUMBER_BOX_RATIO * SCALE);

    const auto  TEX = OverviewRender::text(std::to_string(DIGIT), CHyprColor{1.F, 1.F, 1.F, 1.F}, std::max(1, sc<int>(std::round(FONT * SCALE))));
    if (!TEX || !TEX->ok())
        return;

    // `ActorAlign.START` on both axes and the CSS adds no margin: flush into the
    // icon's top-left corner.
    const float X = sc<float>(item.iconBox.x);
    const float Y = sc<float>(item.iconBox.y);

    CRectPassElement::SRectData bg;
    bg.box   = CBox{std::round(X), std::round(Y), SIZE, SIZE};
    bg.color = CHyprColor{0.F, 0.F, 0.F, NUMBER_BG_ALPHA};
    // Truncated for the same reason the running dot is: a radius even slightly
    // over half the box collapses the circle into a diamond.
    bg.round         = sc<int>(SIZE / 2.F);
    bg.roundingPower = 2.F;
    g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(bg));

    // `text-align: center`, and vertically too -- the label fills the bin.
    CTexPassElement::SRenderData text;
    text.tex = TEX;
    text.box = CBox{std::round(X + (SIZE - sc<float>(TEX->m_size.x)) / 2.F), std::round(Y + (SIZE - sc<float>(TEX->m_size.y)) / 2.F), sc<float>(TEX->m_size.x),
                    sc<float>(TEX->m_size.y)};
    text.a   = 1.F;
    g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(text));
}

// X32. A pinned favourite that is not running has no window class, so the class
// lookup that used to be the whole of this cannot name it -- its desktop entry
// is the only source. Ordered so a running app is still named by its class,
// which is what X27 measured.
std::string itemName(const SItem& item) {
    // The SCRATCHPAD carries no class, no desktop id and no uri, so every lookup
    // below answered with an empty string and its icon was the one slot in the
    // pill that showed no label at all on hover (user, 2026-09-12). Its name is
    // its own, the same way a location's is.
    if (item.scratchpad)
        return item.locationName;

    // X35. A location's name is its own -- no class to look it up by, and the
    // desktop-entry fallback below would answer "Files" for every one of them.
    if (!item.uri.empty())
        return item.locationName;

    if (const auto FROMCLASS = AppSearch::appNameForClass(item.wmClass); !FROMCLASS.empty())
        return FROMCLASS;

    if (const auto* ENTRY = AppSearch::entryForId(item.desktopId); ENTRY && !ENTRY->name.empty())
        return ENTRY->name;

    return item.wmClass;
}

std::vector<PHLWINDOW> peekWindowsOn(PHLMONITOR monitor, const std::vector<PHLWINDOW>& windows) {
    std::vector<PHLWINDOW> out;
    if (!monitor)
        return out;
    for (const auto& WINDOW : windows) {
        if (!WINDOW || !WINDOW->m_isMapped || WINDOW->m_monitor != monitor || !WINDOW->m_workspace)
            continue;
        // A window in flight keeps its label: it rides the flight.
        if (Flight::hasFlight(WINDOW)) {
            out.emplace_back(WINDOW);
            continue;
        }
        if (Minimize::isMinimized(WINDOW))
            continue;
        if (WINDOW->m_workspace == monitor->m_activeWorkspace || WINDOW->m_workspace == monitor->m_activeSpecialWorkspace)
            out.emplace_back(WINDOW);
    }
    return out;
}

void renderWindowPeek(PHLMONITOR monitor, float alpha) {
    if (alpha <= 0.F || g_tip.peek.empty() || scrollOverviewForMonitor(monitor))
        return;

    const float S = std::max(monitor->m_scale, 0.01F);
    // Each flying window's label box last frame, for its motion blur.
    static std::unordered_map<const Desktop::View::CWindow*, CBox> s_lastFlyingPlate;
    for (const auto& REF : g_tip.peek) {
        const auto WINDOW = REF.lock();
        const auto FLIGHT = WINDOW ? Flight::drawnFor(WINDOW) : std::nullopt;
        // A flight armed but not drawn yet has no box to ride; its window's real
        // box is where it is NOT (a restore parks it at alpha 0).
        const bool ARMED  = WINDOW && !FLIGHT && Flight::hasFlight(WINDOW);
        if (!WINDOW || !WINDOW->m_isMapped || ARMED || (!FLIGHT && Minimize::isMinimized(WINDOW)) || WINDOW->m_monitor != monitor) {
            if (WINDOW)
                s_lastFlyingPlate.erase(WINDOW.get());
            continue;
        }

        std::string app = AppSearch::appNameForClass(AppSearch::identityClass(WINDOW->m_class, WINDOW->m_title));
        if (app.empty())
            app = WINDOW->m_class;
        std::string title = WINDOW->m_title;
        if (title.size() > PEEK_TITLE_MAX) {
            size_t cut = PEEK_TITLE_MAX - 1;
            while (cut > 0 && (sc<unsigned char>(title[cut]) & 0xC0) == 0x80)
                --cut;
            title = title.substr(0, cut) + "…";
        }
        const std::string TEXT = title.empty() || title == app ? app : app + "  \u00b7  " + title;

        // IN FLIGHT: on the box the flight drew, faded with it, and smeared along
        // the same path as the window (FlightBlur, the minimize blur's samples
        // and half-duration fade). No size gate -- it follows the window all the
        // way into the icon and fades out with it.
        if (FLIGHT) {
            const CBox& B     = FLIGHT->box;
            const CBox  PLATE = OverviewRender::labelPillBox(monitor, TEXT, 0.0, 0.0, S);
            if (PLATE.height < 1 || B.width < 1 || B.height < 1 || FLIGHT->alpha <= 0.01F)
                continue;
            const double CX  = B.x + B.width / 2.0;
            const double BY  = B.y + std::min<double>(PEEK_TOP_GAPL * S + PLATE.height, B.height);
            const CBox   NOW{CX - PLATE.width / 2.0, BY - PLATE.height, PLATE.width, PLATE.height};
            const auto   IT   = s_lastFlyingPlate.find(WINDOW.get());
            const CBox   PREV = IT != s_lastFlyingPlate.end() ? FlightBlur::shape(FlightBlur::ESite::MINIMIZE, IT->second, NOW, FLIGHT->progress) : NOW;
            s_lastFlyingPlate[WINDOW.get()] = NOW;
            const auto DRAW = [&] { OverviewRender::labelPill(monitor, TEXT, CX, BY, alpha * FLIGHT->alpha, S); };
            if (!FlightBlur::drawBlurred(monitor, nullptr, PREV, NOW, DRAW))
                DRAW();
            continue;
        }
        s_lastFlyingPlate.erase(WINDOW.get());

        const auto  POS = WINDOW->m_realPosition->value() - monitor->m_position;
        const auto  SZ  = WINDOW->m_realSize->value();
        // Only the part of the window ON this monitor. A scrolling desktop keeps
        // windows parked beside the screen, and they were labelled too -- at the
        // edge, or off it entirely (user, 2026-09-29: "not useful"). Mostly out
        // of view (under half on screen): no label; otherwise it is centred on
        // what can be seen.
        const CBox  FULL{POS.x, POS.y, SZ.x, SZ.y};
        const CBox  SEEN = FULL.intersection(CBox{{}, monitor->m_size});
        if (FULL.width < 1 || FULL.height < 1 || SEEN.width < 1 || SEEN.height < 1 || SEEN.width * SEEN.height < 0.5 * FULL.width * FULL.height)
            continue;
        const CBox  BOX{SEEN.x * S, SEEN.y * S, SEEN.width * S, SEEN.height * S};
        const CBox  PLATE = OverviewRender::labelPillBox(monitor, TEXT, 0.0, 0.0, S);
        if (PLATE.height < 1 || PLATE.width > BOX.width || PLATE.height + PEEK_TOP_GAPL * S > BOX.height)
            continue; // a window too small to hold its own label keeps it off
        OverviewRender::labelPill(monitor, TEXT, BOX.x + BOX.width / 2.0, BOX.y + PEEK_TOP_GAPL * S + PLATE.height, alpha, S);
    }
}

std::string tooltipSubject(const PHLWINDOW& window) {
    return "w:" + std::to_string(reinterpret_cast<uintptr_t>(window.get()));
}

std::string tooltipSubject(const SItem& item) {
    if (!item.uri.empty())
        return "u:" + item.uri;
    if (!item.desktopId.empty())
        return "i:" + item.desktopId;
    if (!item.wmClass.empty())
        return "i:" + item.wmClass;
    return "i:" + item.icon;
}

// Y2. `bottomY` is the monitor-local RENDER y the plate's underside sits on. 0
// means "the pill", which is every caller that labels an icon. The hover panel
// passes its own top instead, so a preview's title rides above the previews
// rather than behind them.
// `subject` is the hovered icon or window. The fade restarts when THAT changes
// (sliding to the next icon), not when the same window's title ticks -- a
// streaming Grok/browser title used to punch the plate back to zero alpha
// every update.
// The hovered app's window labels (renderWindowPeek) name windows ON the
// desktop, so they go under the dock's own chrome: drawn before the hover
// panel, on the tooltip's fade (user, 2026-10-16: the panel "glitches when I
// open overview" -- the labels sat on top of its previews, went off the moment
// the overview opened and came back as it closed). renderTooltip, later in the
// frame, sets what they label; they follow it a frame behind, inside its fade.
static void renderTooltipPeek(PHLMONITOR monitor) {
    if (g_tip.name.empty() || g_tip.peek.empty() || g_tip.monitor != monitor->m_id)
        return;
    const float T = std::clamp(elapsedMs(g_tip.since) / (g_tip.out ? TOOLTIP_FADE_OUT_MS : TOOLTIP_FADE_MS), 0.F, 1.F);
    if (g_tip.out && T >= 1.F)
        return;
    renderWindowPeek(monitor, g_tip.out ? 1.F - houseEase(false, T) : houseEase(true, T));
}

void renderTooltip(PHLMONITOR monitor, const SLayout& layout, const CBox& anchor, const std::string& NAME, float bottomY = 0.F, const std::string& subject = {},
                   const std::vector<PHLWINDOW>& peek = {}) {
    const bool LIVE = !subject.empty() || !NAME.empty();

    // Nothing to show here and the tooltip is another monitor's: leave it alone.
    if (!LIVE && g_tip.monitor != monitor->m_id)
        return;

    if (LIVE) {
        const std::string& KEY = !subject.empty() ? subject : NAME;
        // New hover target, coming back from a fade-out, or arriving from
        // another monitor's dock: own entrance. Same target, new text: swap the
        // string, keep the alpha.
        if (KEY != g_tip.subject || g_tip.out || g_tip.monitor != monitor->m_id) {
            g_tip.monitor = monitor->m_id;
            g_tip.subject = KEY;
            g_tip.name    = NAME;
            g_tip.since   = Time::steadyNow();
            g_tip.out     = false;
        } else if (!NAME.empty())
            g_tip.name = NAME;
        g_tip.anchor = anchor;
        // Y2. Remembered with the anchor and for the same reason: the plate
        // outlives the hover so it can fade away, and by then the cell it was
        // labelling may be gone.
        g_tip.bottomY = bottomY;
        g_tip.peek.clear();
        for (const auto& WINDOW : peek)
            g_tip.peek.emplace_back(WINDOW);
    } else {
        if (g_tip.name.empty())
            return;
        if (!g_tip.out) {
            g_tip.out   = true;
            g_tip.since = Time::steadyNow();
        }
    }

    if (g_tip.name.empty())
        return;

    const float T = std::clamp(elapsedMs(g_tip.since) / (g_tip.out ? TOOLTIP_FADE_OUT_MS : TOOLTIP_FADE_MS), 0.F, 1.F);
    if (g_tip.out && T >= 1.F) {
        g_tip.name.clear();
        g_tip.subject.clear();
        g_tip.peek.clear();
        return;
    }

    // Entrances decelerate, exits accelerate: the house rule, both directions.
    const float ALPHA = g_tip.out ? 1.F - houseEase(false, T) : houseEase(true, T);

    // The frames this fade needs are asked for by needsFrames(), which reads the
    // same g_tip clock this line does. A pointer resting on an icon emits no
    // motion, so nothing else on the desktop would ask for them.

    const float SCALE = std::max(monitor->m_scale, 0.01F);
    const float CX    = sc<float>(g_tip.anchor.x) + sc<float>(g_tip.anchor.width) / 2.F;

    // X57. Drawn by the SHARED pill, the same one the overview tile caption
    // uses -- the host styles both with one css rule and this is that decision
    // in code.
    const float BOTTOM = g_tip.bottomY > 0.F ? g_tip.bottomY : sc<float>(layout.pill.y);
    OverviewRender::labelPill(monitor, g_tip.name, CX, BOTTOM - OverviewRender::LABEL_PILL_GAP * SCALE, ALPHA, SCALE);
}
}

// AN OUTPUT THAT IS GONE OWNS NO STATE. The dock keeps three maps keyed by
// monitor id -- the visual state damageIfChanged diffs against, the autohide
// reveal, and the frame-pump mark -- and none of them had anything that removed
// a row. Two of the three merely leak a handful of bytes per hotplug; the third
// is the latch described above FramePump::request. A stale row is not only
// waste either: nothing here promises a MONITORID is never handed out twice,
// and a row that outlives its output is the state a later one would be judged
// against.
//
// Called from the monitor.removed handler beside Flight::abortAll -- the same
// place, and for the same reason, as P0's overview teardown. The pump's mark
// and its queued doLater live in FramePump now and go in one call, after the
// dock's own rows (independent maps: no draw or callback runs in between).
void forgetMonitor(MONITORID id) {
    g_visualStates.erase(id);
    g_reveal.erase(id);
    g_baseLayouts.erase(id);
    g_arrange.erase(id);
    DockTilt::forget(id);
    FramePump::forget(id);
}

// IS ANYTHING ON THE DOCK STILL MOVING? The one list, in one place.
//
// It was three: an inline `autohideAnimating || WIGGLING || overlayUp` in
// damageIfChanged, a `T < 1` inside renderTooltip, and a four-term early return
// at the bottom of render(). Each had its own copy of the same doLater, and a
// new animation had to find and join all three -- which is how the tooltip fade
// ended up with a pump of its own that nothing else knew about.
//
// It is still called from TWO places, and that is deliberate rather than a job
// half done. damageIfChanged runs on render.pre AND on pointer motion, so it is
// the only one reached while the dock is fully away (render() returns on an
// empty pill) -- that is what lets a reveal start at all. The render tail runs
// after the flight loop has advanced, which is where the flight animations must
// be asked from: moving that request to before the tick is the exact reordering
// that made three reversals in a row fire against flights that had never drawn
// a frame (see the note above Flight::renderFlights).
const char* needsFramesReason(PHLMONITOR monitor, const SLayout& layout, const STally& tally) {
    // X30. A flip waiting out its delay looks EXACTLY like the frame before it,
    // so the visual-state check in damageIfChanged would stop the chain of
    // frames dead and the dock would freeze half-way -- or never start.
    if (autohideAnimating(monitor))
        return "autohide";

    // X31. The wiggle's rotation changes every frame while it runs.
    //
    // Gated on the pill being ON SCREEN for this monitor (user, 2026-09-14): a
    // wiggle nobody can see is not worth a frame, and `layout` here is the SLID
    // layout, which is empty once the dock is fully away.
    //
    // ponytail, STILL OPEN: this pumps frames for the 2500ms of every 3000ms the
    // urgent icon is deliberately STILL, as well as the 500 it moves. Waking on
    // a timer for the next burst was measured and then NOT built: CEventLoopManager
    // holds its timers by strong SP, nothing in this file is reachable from
    // PLUGIN_EXIT to remove one, and a timer that outlives the dlclose is the
    // exact crash shape main.cpp's removeAllOfType note describes. It needs an
    // unload hook first.
    if (!layout.pill.empty() && Config::getValue<bool>(KEY_DANCE_URGENT) && anyUrgent(layout.items, tally))
        return "urgent-dance";

    // The hop, dock hidden: frames only while it is in the air (plus one to
    // land it); the timer wakes the next one.
    if (layout.pill.empty() && Config::getValue<bool>(KEY_AUTOHIDE) && Config::getValue<bool>(KEY_URGENT_NOTIFY) && anyUrgent(baseLayout(monitor).items, tally) &&
        std::fmod(elapsedMs(urgentEpoch()), WIGGLE_PERIOD_MS) < URGENT_HOP_MS + 50.F)
        return "urgent-hop";

    // X34. The hotkey overlay fades.
    if (overlayUp())
        return "hotkey-overlay";

    if (g_menu && g_menu->monitor == monitor->m_id && elapsedMs(g_menu->hoverAt) < MENU_HOVER_MS + 20.F)
        return "menu-hover";
    if (g_menu && g_menu->monitor == monitor->m_id && menuOpening(*g_menu))
        return "menu-expand";
    if (g_menuGhost && g_menuGhost->menu.monitor == monitor->m_id)
        return "menu-collapse";

    // The press squish, and above all its spring-back, which runs after the
    // button came up with no pointer motion left to repaint it.
    if (pressAnimating(monitor->m_id))
        return "icon-press";

    // The hover lean follows the pointer on a spring, and springs back after it
    // leaves -- again with no motion left to ask for the frames.
    if (DockTilt::animating(monitor->m_id))
        return "icon-tilt";

    // Y2. A pointer resting on an icon emits no motion, so nothing else would
    // ask for the frames the tooltip fade needs. renderTooltip clears the name
    // when its exit finishes, so a settled tooltip answers false here.
    if (!g_tip.name.empty() && g_tip.monitor == monitor->m_id && elapsedMs(g_tip.since) < (g_tip.out ? TOOLTIP_FADE_OUT_MS : TOOLTIP_FADE_MS))
        return "tooltip-fade";

    // THE OTHER MODULES' TERMS are asked from FramePump's table (FramePump.cpp,
    // `SOURCES`), at three fixed points in this chain so the order every term is
    // asked in is the order it always was: Stacks::needsFrames and
    // arrangeAnimating below both retire finished entries as they answer, so
    // they are not pure predicates and moving them past each other is not a
    // refactor. A new module's animation is one line in that table.
    if (const char* FOREIGN = FramePump::foreignReason(monitor, FramePump::ESlot::AFTER_TOOLTIP))
        return FOREIGN;

    // V3. The icon flying home after a missed drop. It runs after the button
    // came up, so no pointer motion is left to repaint it -- this is the only
    // thing asking for those frames, in both contexts (the overview throttles
    // plain damage, which is why the ONE list is also the right list here).
    if (ghostFlying())
        return "ghost-flight";

    if (const char* FOREIGN = FramePump::foreignReason(monitor, FramePump::ESlot::AFTER_GHOST_FLIGHT))
        return FOREIGN;

    // The arrangement glides on a desktop nothing else is repainting.
    if (arrangeAnimating(monitor->m_id))
        return "arrange";

    if (const char* FOREIGN = FramePump::foreignReason(monitor, FramePump::ESlot::AFTER_ARRANGE))
        return FOREIGN;

    // X25/X47/Y6/Y8. A minimized window's commits damage no monitor, the hover
    // panel's entrance and live previews need frames of their own, and a bounce
    // or a launch zoom runs on a dock nothing else is touching.
    if (panelNeedsFramesOn(monitor))
        return "hover-panel";
    if (!g_flight.bounces.empty())
        return "bounce";
    if (springAnimating())
        return "spring";
    if (!g_flight.launchZooms.empty())
        return "launch-zoom";

    return nullptr;
}

// The bool the render path asks for, and the NAME of whichever term answered,
// for `hyprdrive:debug frames`. One chain, so the answer and the explanation
// can never disagree (2026-09-15).
bool needsFrames(PHLMONITOR monitor, const SLayout& layout, const STally& tally) {
    return needsFramesReason(monitor, layout, tally) != nullptr;
}

bool damageIfChanged(PHLMONITOR monitor) {
    if (!monitor)
        return false;

    // X31. The bus connection is opened by the first frame that could use it, so
    // a dock switched off -- or one with both badge and urgent-notify off --
    // never opens one at all. Idempotent.
    const bool ENABLED = Config::getValue<bool>(KEY_ENABLED);
    if (ENABLED && (Config::getValue<bool>(KEY_BADGE) || Config::getValue<bool>(KEY_URGENT_NOTIFY)))
        LauncherEntry::start();

    // ONE walk of the window list for this whole call: updateAutohide's urgent
    // test, needsFrames' wiggle test and the per-item signature below all read
    // it instead of each walking the list for themselves. Skipped outright with
    // the dock off -- this function still runs on every pointer motion event.
    const auto TALLY  = ENABLED ? buildTally() : STally{};
    const bool BADGES = ENABLED && badgesEnabled();

    updateAutohide(monitor, TALLY);

    // X30. A flip waiting out its delay looks EXACTLY like the frame before it,
    // so the signature check below would stop the chain of frames dead and the
    // dock would freeze half-way -- or never start. render.pre only fires when a
    // frame is already happening, so the next one has to be asked for here, and
    // after the current frame (doLater) for the same reason the live previews
    // are: damage asked for during the pass is cleared with the pass.
    // X31's wiggle is in here for the same reason: its rotation changes every
    // frame while it runs, and nothing else on an idle desktop asks for one.
    //
    // ponytail: this pumps frames for the 2500ms of every 3000ms the urgent icon
    // is deliberately STILL, as well as the 500 it moves. Waking on a timer for
    // the next burst instead would save them; measure it before building it.
    // One layout, used by both the wiggle test and the signature below.
    // layout() rebuilds the whole item model -- a scan of every window, a set of
    // running classes, and a fresh vector of SItems carrying several strings
    // each -- and this function runs on every frame AND on every pointer motion
    // event, which arrive far faster than frames do. It was being built twice.
    const auto LAYOUT = layout(monitor);

    if (needsFrames(monitor, LAYOUT, TALLY))
        pumpFrame(monitor, dockDamageRegion(monitor, LAYOUT));

    SDockVisualState state;
    if (!LAYOUT.pill.empty()) {
        state.pillUp = true;
        state.pillX  = sc<int>(LAYOUT.pill.x);
        state.pillY  = sc<int>(LAYOUT.pill.y);
        state.pillW  = sc<int>(LAYOUT.pill.width);

        for (const auto& ITEM : LAYOUT.items) {
            // X32. The desktop id is carried as well as the class because a
            // pinned favourite HAS no class: without it, launching a pinned app
            // -- or pinning one while another is already pinned -- leaves the
            // state identical and the running dot never appears.
            // X35. The icon NAME, because the trash swaps `user-trash` for
            // `user-trash-full` without moving a box. Ceiling: the swap is seen
            // when the poll next runs AND something asks for a frame, so on a
            // wholly idle desktop the new icon can lag the deletion by a beat.
            auto& ITEMSTATE = state.items.emplace_back(SDockItemState{ITEM.desktopId, ITEM.wmClass, ITEM.uri, ITEM.icon, ITEM.running});

            // A location has no class, so every badge lookup below would key on
            // the empty string -- and a location has no notifications anyway.
            if (!ITEM.running || !ITEM.uri.empty())
                continue;

            // X31. A badge arriving, or an app going urgent, changes what the
            // pill LOOKS like without moving a single box.
            ITEMSTATE.badge  = itemBadge(ITEM, BADGES);
            ITEMSTATE.urgent = itemUrgent(ITEM, TALLY);
        }

        const auto MOUSE = (g_pInputManager->getMouseCoordsInternal() - monitor->m_position) * monitor->m_scale;
        for (size_t i = 0; i < LAYOUT.items.size(); ++i) {
            if (hoverBox(LAYOUT.items[i].iconBox, monitor->m_scale).containsPoint(MOUSE)) {
                state.hovered = sc<int>(i);
                break;
            }
        }

        // V11 V4. Everything the popup draws, in one string. Without it an
        // open fan is invisible to the comparison and its entrance stops on
        // its first frame -- the same failure the hover panel had.
        state.stacks = Stacks::visualState();

        state.hoverShowApps = !LAYOUT.showApps.empty() && hoverBox(LAYOUT.showApps, monitor->m_scale).containsPoint(MOUSE);
        if ((state.hovered >= 0 || state.hoverShowApps) && DockTilt::hoverEnabled()) {
            state.tiltX = sc<int>(std::round(MOUSE.x));
            state.tiltY = sc<int>(std::round(MOUSE.y));
        }
        state.caret         = caretIndex();
        state.overlay       = overlayUp();

        if (ghostShowing()) {
            const auto GHOST = ghostBoxAt(monitor, g_dockDrag.drag->home, g_dockDrag.drag->grab, g_dockDrag.drag->at);
            state.ghostX     = sc<int>(GHOST.x);
            state.ghostY     = sc<int>(GHOST.y);
        }

        if (g_menu && g_menu->monitor == monitor->m_id) {
            state.menuUp    = true;
            state.menuClass = g_menu->item.wmClass;
            state.menuUri   = g_menu->item.uri;
            for (size_t i = 0; i < g_menu->rows.size(); ++i) {
                if (!g_menu->rows[i].box.empty() && g_menu->rows[i].box.containsPoint(MOUSE)) {
                    state.menuRow = sc<int>(i);
                    break;
                }
            }
        }
    }

    auto& LAST = g_visualStates[monitor->m_id];
    if (LAST == state)
        return false;

    LAST = std::move(state);
    g_pHyprRenderer->damageMonitor(monitor);
    return true;
}

// X54. The PUBLIC way to minimize or restore. Everything must come through
// these: onMinimizeDispatcher called Minimize::minimize straight through, so a
// keybind-driven minimize never armed a flight at all -- which is why every rig
// pointed at the dispatcher measured "no animation", and why the diagnostic log
// came back EMPTY rather than wrong.
bool dockOwnsNativeDrag() {
    return g_dockNativeDrag;
}

void clearDockNativeDrag() {
    g_dockNativeDrag = false;
}

void setWindowDragActive(bool active) {
    g_flight.windowDragActive = active;
}

// THE ONE FROSTED PLATE. Every plate in this rice is the same two draws in the
// same order -- the live blur behind it, then the theme's tint on top, both at
// the caller's alpha -- and they were five copies of that pair before V2 and V4
// each added a sixth. See the note in Dock.hpp.
// The theme's accent: Hyprland's active border, the colour labelPill's ring uses.
CHyprColor accentColor() {
    if (auto* const ACTIVE = sc<::Config::CGradientValueData*>(Hyprdrive::Config::valueRef<::Config::IComplexConfigValue>("general:col.active_border").ptr());
        ACTIVE && !ACTIVE->m_colors.empty())
        return ACTIVE->m_colors[0];
    return Hyprdrive::Config::themeForeground();
}

void frostedPlate(PHLMONITOR monitor, const CBox& box, int rounding, float alpha, const CBox& clip, float roundingPower, bool currentPass) {
    // Every caller (the dock, the stacks' fan/pane/field, the menu) comes
    // through here: a zero or negative box would RASSERT in Hyprland's renderer
    // and take the compositor down (review 2026-09-24: the pane's search field
    // on its first frame, with a small dock icon).
    if (!Stacks::drawableBox(box) || alpha <= 0.F)
        return;
    queueDockFrost(monitor, box, rounding, alpha, clip, nullptr, currentPass, roundingPower);

    const auto                  TINT = background();
    CRectPassElement::SRectData bg;
    bg.box           = box;
    bg.color         = CHyprColor{TINT.r, TINT.g, TINT.b, TINT.a * alpha};
    bg.round         = rounding;
    bg.roundingPower = roundingPower;
    bg.clipBox       = clip;
    if (currentPass)
        g_pHyprRenderer->addPassElement(makeUnique<CRectPassElement>(bg));
    else
        g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(bg));
}

void minimizeWithFlight(PHLMONITOR monitor, PHLWINDOW window, bool bounceOnLand, std::optional<CBox> startBox) {
    Flight::minimize(monitor, window, bounceOnLand, startBox);
}

CBox roomPillGlobal(PHLMONITOR monitor) {
    if (!monitor || Config::getValue<bool>(KEY_AUTOHIDE) || !Config::getValue<bool>("plugin:hyprdrive:dock:windows_make_room"))
        return {};
    const auto& L = baseLayout(monitor);
    if (L.pill.w < 1 || L.pill.h < 1)
        return {};
    const double S = std::max(monitor->m_scale, 0.01F);
    return CBox{L.pill.x / S + monitor->m_position.x, L.pill.y / S + monitor->m_position.y, L.pill.w / S, L.pill.h / S};
}

void onWindowOpen(const PHLWINDOW& window) {
    // An app set to always open in an empty workspace, launched any other way
    // (the launcher, a keybind, a terminal, its own "new window"): its new main
    // window moves to the nearest empty desktop. Not a dialog, a child, a
    // floating window (picture-in-picture, a popup), nor one already alone.
    if (!window || window->m_isFloating || window->parent() || window->isModal() || !window->m_workspace || window->m_workspace->m_isSpecialWorkspace)
        return;
    if (!opensInEmptyDesktop(AppSearch::idForClass(window->m_class)))
        return;
    // Next turn of the loop: not from inside the window's own map.
    g_pEventLoopManager->doLater([REF = PHLWINDOWREF{window}] {
        const auto W = REF.lock();
        if (!W || !W->m_isMapped || !W->m_workspace || W->m_workspace->m_isSpecialWorkspace)
            return;
        const bool ALONE = std::ranges::none_of(Desktop::windowState()->windows(), [&](const PHLWINDOW& O) {
            return O && O != W && O->m_isMapped && !O->isHidden() && O->m_workspace == W->m_workspace;
        });
        if (ALONE)
            return;
        if (const auto MONITOR = W->m_workspace->m_monitor.lock())
            moveToEmptyWorkspace(MONITOR, W);
    });
}

void restoreWithFlight(PHLMONITOR monitor, PHLWINDOW window) {
    Flight::restore(monitor, window);
}

// ---- what the FLIGHT module reaches back into the dock for -----------------
//
// Three pieces of dock state outlive the extraction because they are the dock's,
// not the flight's: the autohide reveal, the Y8 bounce list, and the Y1 hover
// panel. They are named (external linkage) so FlightController.cpp can call
// them; each is a mutator of a g_ that stays here.

// X54. Bring the pill out NOW, skipping show_delay. A flight is 180ms and the
// delay alone is 250, so "ask nicely and wait" meant the animation was over
// before the dock arrived -- and Dock::render returns early on an empty pill, so
// the flight was never drawn at all. The autohide slide still plays; the flight
// simply targets the icon while it moves, which is the right look anyway.
void revealNow(PHLMONITOR monitor) {
    if (!monitor)
        return;

    auto& R = g_reveal[monitor->m_id];
    if (R.shown)
        return;

    R.shown     = true;
    R.pending   = false;
    R.changedAt = Time::steadyNow();
}

// Y8/Z10. The icon hops when the thing it is receiving is acknowledged.
void armBounce(const std::string& wmClass) {
    g_flight.bounces.emplace_back(SBounce{.wmClass = wmClass, .startedAt = Time::steadyNow()});
}

// Y8's hop, evaluated. Exported (Dock.hpp) because a stack cell hops with the
// same motion off a ledger of its own -- the curve and HOP_MS are shared, the
// ledgers are not.
float hopLift(float t, float iconHeight) {
    return bounceCurve(std::clamp(t, 0.F, 1.F)) * BOUNCE_HEIGHT * iconHeight;
}

// Y8. How far this app's icon is lifted out of the pill right now. Reads the
// largest of any overlapping hops rather than summing them, so two windows
// landing together is one bounce and not a double-height launch.
float bounceLift(const std::string& wmClass, float iconHeight) {
    float lift = 0.F;
    for (const auto& BOUNCE : g_flight.bounces) {
        if (BOUNCE.wmClass != wmClass)
            continue;

        lift = std::max(lift, hopLift(elapsedMs(BOUNCE.startedAt) / HOP_MS, iconHeight));
    }

    return lift;
}

// Y7. Zero at the top of the bottom-30% band and one at the pill, continuous
// between. Uses the pill as laid out THIS frame, so a dock still sliding out
// glows against where it actually is rather than where it will settle.
float dragGlow(PHLMONITOR monitor, const SLayout& layout, const Vector2D& mouse);

// THE DOCK IS "FOCUSED" -- its outline lit -- while the pointer is on the pill
// (or the margin under it, or the reveal edge: what autohide counts as on it),
// or the dock is being used past its pill: its menu, its hover previews, an
// open stack, a drag of one of its icons or files, a window dragged at it.
// `mouse` is monitor-local render px (MOUSELOCAL).
static bool dockInUse(PHLMONITOR monitor, const SLayout& layout, const Vector2D& mouse) {
    if (layout.pill.empty())
        return false;
    CBox reach = layout.pill;
    reach.height += EDGE_MARGIN * monitor->m_scale;
    const auto BOX    = monitor->logicalBox();
    const auto POINT  = g_pInputManager->getMouseCoordsInternal();
    const bool ATEDGE = BOX.containsPoint(POINT) && POINT.y >= BOX.y + BOX.h - REVEAL_BAND;
    return reach.containsPoint(mouse) || ATEDGE || menuOpen() || panelHoldsDock(monitor) || Stacks::holdsDock(monitor) || dragging() || DockDrop::draggingOut() ||
        dragGlow(monitor, layout, mouse) > 0.001F;
}

float dragGlow(PHLMONITOR monitor, const SLayout& layout, const Vector2D& mouse) {
    if (!g_flight.windowDragActive || layout.pill.empty())
        return 0.F;

    const float H       = sc<float>(monitor->m_size.y) * monitor->m_scale;
    const float BANDTOP = H * (1.F - DRAG_GLOW_BAND);
    const float PILLTOP = sc<float>(layout.pill.y);

    if (sc<float>(mouse.y) <= BANDTOP || PILLTOP <= BANDTOP)
        return 0.F;

    return std::clamp((sc<float>(mouse.y) - BANDTOP) / (PILLTOP - BANDTOP), 0.F, 1.F);
}

// [FLIGHT] settleFlights is gone, and it is not a rename: P0's monitor-removed
// settle WAS a second landing path, written beside the first and already
// missing pieces of it (a driven restore's geometry write-back, Z18b). It is
// Flight::abortAll(reason) now -- the same landing every other end of a
// flight runs, with its invariant checked, called from monitor.removed,
// config.reloaded and PLUGIN_EXIT.

const char* frameReason(PHLMONITOR monitor) {
    if (!monitor)
        return nullptr;

    const auto LAYOUT = layout(monitor);
    const auto TALLY  = buildTally();
    return needsFramesReason(monitor, LAYOUT, TALLY);
}

// THE SIZE AN ICON TEXTURE IS ASKED FOR (IconSize.hpp, 2026-10-15): the
// RESTING cell on this monitor, device px -- every cell of a pill is the same
// size. Not the box being drawn: that one grows in on spawn, lerps through a
// re-arrange and blows up on launch, and asking for each of those sizes would
// decode a texture per frame. The hover lean (1.03) is inside IconSize's
// headroom; the transient zooms are drawn from the resting texture.
double restingIconPx(PHLMONITOR monitor) {
    const auto& BASE = baseLayout(monitor);
    if (!BASE.items.empty())
        return BASE.items.front().iconBox.width;
    return BASE.showApps.width;
}

// What the icon warmer decodes ahead for the dock: each item's icon at the
// size its resting cell is drawn at on each monitor (the px every draw below
// passes appIcon).
std::vector<std::pair<std::string, double>> iconWarmList() {
    std::vector<std::pair<std::string, double>> out;
    for (const auto& MON : State::monitorState()->monitors()) {
        const double PX = restingIconPx(MON);
        for (const auto& ITEM : baseLayout(MON).items)
            if (!ITEM.icon.empty() && !ITEM.scratchpad && PX > 0.0)
                out.emplace_back(ITEM.icon, PX);
    }
    return out;
}

// The hop's drawing: each urgent app's icon, at its RESTING x, rising out of the
// bottom edge by the hop's lift. Only ever called with the dock fully away.
void renderUrgentHop(PHLMONITOR monitor) {
    if (!Config::getValue<bool>(KEY_AUTOHIDE) || !Config::getValue<bool>(KEY_URGENT_NOTIFY))
        return;

    const auto  TALLY = buildTally();
    const auto& BASE  = baseLayout(monitor);
    std::vector<const SItem*> urgent;
    for (const auto& ITEM : BASE.items)
        if (ITEM.running && ITEM.uri.empty() && itemUrgent(ITEM, TALLY))
            urgent.push_back(&ITEM);
    if (urgent.empty())
        return;

    const float LIFT = urgentHopLift();
    if (LIFT < 0.F) {
        armHopTimer(msToNextHop());
        return;
    }

    const float EDGE = sc<float>(monitor->m_size.y * monitor->m_scale); // monitor-local render px
    for (const SItem* ITEM : urgent) {
        const auto TEX = OverviewRender::appIcon(ITEM->icon, false, false, restingIconPx(monitor));
        if (!TEX || !TEX->ok())
            continue;
        const CBox& IB = ITEM->iconBox;
        const float UP = std::round(LIFT * URGENT_HOP_LIFT * sc<float>(IB.height));
        if (UP < 1.F)
            continue;
        CTexPassElement::SRenderData d;
        d.tex = TEX;
        d.box = CBox{IB.x, EDGE - UP, IB.width, IB.height};
        d.a   = 1.F;
        g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(d));
    }
}

// THE STACK PEEK (user, 2026-09-25: "show the bounce so we see the tip of the
// stack that bounced"). With the dock hidden, a stack whose folder just got
// something new pops the top of its pile up from the screen edge on its hop's
// arc, where its cell would be -- the stacks' own kind of urgent hop.
// True while a peek is still running here: the only stacks motion a hidden
// dock shows, so the only thing it needs frames for (review 2026-11-09).
bool renderStackPeek(PHLMONITOR monitor) {
    const auto& BASE    = baseLayout(monitor);
    const float EDGE    = sc<float>(monitor->m_size.y * monitor->m_scale); // monitor-local render px
    bool        running = false;
    for (const auto& ITEM : BASE.items) {
        if (!ITEM.stack || ITEM.placeholder || ITEM.stackPath.empty())
            continue;
        const CBox& IB  = ITEM.iconBox;
        const float AGE = Stacks::bounceAgeMs(ITEM.stackPath);
        if (AGE < 0.F || AGE >= STACK_PEEK_MS)
            continue;
        running = true;
        // Up quickly, hold so it registers, back down: rise over the first 22%,
        // hold to 70%, sink over the rest -- a bit over half the pile showing.
        const float T     = AGE / STACK_PEEK_MS;
        const float RISE  = std::clamp(T / 0.22F, 0.F, 1.F), FALL = std::clamp((T - 0.70F) / 0.30F, 0.F, 1.F);
        const float CURVE = T < 0.70F ? 1.F - (1.F - RISE) * (1.F - RISE) : 1.F - FALL * FALL;
        const float UP    = std::round(CURVE * 0.55F * sc<float>(IB.height));
        if (UP < 1.F)
            continue;
        const CBox  AT{IB.x, EDGE - UP, IB.width, IB.height};
        if (!Stacks::drawCell(monitor, ITEM, AT, 1.F, DockTilt::SPose{}))
            if (const auto TEX = OverviewRender::appIcon(ITEM.icon, false, false, restingIconPx(monitor)); TEX && TEX->ok() && Stacks::drawableBox(AT)) {
                CTexPassElement::SRenderData d;
                d.tex = TEX;
                d.box = AT;
                g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(d));
            }
    }
    return running;
}

void render(PHLMONITOR monitor) {
    // Y6/Y8. Retire what has run its course before anything asks whether the
    // dock still needs frames.
    std::erase_if(g_flight.bounces, [](const SBounce& b) { return elapsedMs(b.startedAt) >= HOP_MS; });
    std::erase_if(g_flight.launchZooms, [](const SLaunchZoom& z) { return elapsedMs(z.startedAt) >= LAUNCH_ZOOM_MS; });
    // V3. Same retirement, same place: the icon that flew home has arrived.
    if (g_dockDrag.ghostHome && elapsedMs(g_dockDrag.ghostHome->startedAt) >= GHOST_HOME_MS)
        g_dockDrag.ghostHome.reset();

    const auto LAYOUT = layout(monitor);
    if (LAYOUT.pill.empty()) {
        renderUrgentHop(monitor);
        if (Config::getValue<bool>(KEY_AUTOHIDE)) {
            // This branch returns before the frame pump at the end of render(),
            // so the peek asks for its own next frame while it runs. Only the
            // peek: Stacks::needsFrames also answers for download bars and
            // shakes, which a hidden dock does not draw, and it kept every
            // monitor at full rate for as long as a download ran.
            if (renderStackPeek(monitor))
                pumpFrame(monitor);
        }
        return;
    }

    // A pointer on ANOTHER monitor is nowhere on this dock. Mapped into this
    // monitor's pixels it is merely off-screen today, but nothing guarantees an
    // arrangement where it cannot land on this pill, and every hover below --
    // panel, tooltip, highlight -- has to agree on which dock the pointer is at.
    const auto POINTER    = g_pInputManager->getMouseCoordsInternal();
    const auto MOUSELOCAL = monitor->logicalBox().containsPoint(POINTER) ? (POINTER - monitor->m_position) * monitor->m_scale : Vector2D{-1e6, -1e6};
    const auto& MOUSE      = MOUSELOCAL;
    const auto HOVEREDITEM = std::ranges::find_if(LAYOUT.items, [&](const SItem& item) { return hoverBox(item.iconBox, monitor->m_scale).containsPoint(MOUSE); });
    const bool HASHOVER    = HOVEREDITEM != LAYOUT.items.end();
    const bool HOVERAPPS   = !LAYOUT.showApps.empty() && hoverBox(LAYOUT.showApps, monitor->m_scale).containsPoint(MOUSE);

    renderTooltipPeek(monitor);

    // Hover previews BEFORE the pill. Live window thumbnails that leak downward
    // used to paint over the icons (flushPass composites them on top of the
    // already-queued pill). The panel still sits above the dock; the pill
    // covers anything that spills into its box.
    {
        const bool OVERPANEL   = panelContains(monitor, MOUSE);
        // The ICON drag hides the panel from its first press (it is pulling the
        // very icon the panel belongs to). A PREVIEW drag must not -- until it
        // goes live the gesture is still a click, and a panel that blinks away
        // under the press would make every preview click look broken.
        const bool ICONDRAG    = g_dockDrag.drag.has_value();
        const bool TEAROUT     = g_dockDrag.panel && g_dockDrag.panel->live;
        const bool PANELWANTED = (HASHOVER || OVERPANEL) && !g_menu && !ICONDRAG && !TEAROUT;
        CBox        panelAnchor;
        std::string panelClass;
        std::string panelDesktopId;
        std::string panelScratchpad;
        if (PANELWANTED) {
            const bool FROMICON = HASHOVER;
            panelAnchor         = FROMICON ? HOVEREDITEM->iconBox : g_hover.panel->anchor;
            panelClass          = FROMICON ? HOVEREDITEM->wmClass : g_hover.panel->wmClass;
            panelDesktopId      = FROMICON ? HOVEREDITEM->desktopId : g_hover.panel->desktopId;
            panelScratchpad     = FROMICON ? HOVEREDITEM->scratchpadName : g_hover.panel->scratchpadName;
        }
        // The panel belongs to one monitor. Hovering here while it is up on
        // another moves it here -- at once, since one panel cannot be leaving
        // there and arriving here -- and a monitor that neither wants it nor
        // owns it leaves it strictly alone.
        if (PANELWANTED && g_hover.panel && g_hover.panel->monitor != monitor->m_id) {
            g_hover.panel.reset();
            forgetPanelGeometry();
        }
        if (PANELWANTED || !g_hover.panel || g_hover.panel->monitor == monitor->m_id)
            renderHoverPreview(monitor, LAYOUT, panelAnchor, panelClass, panelDesktopId, panelScratchpad, Time::steadyNow());

    }

    // Y7. Drawn FIRST so the pill's own frost and background land on top of it
    // and the halo only shows where it reaches past the pill's edges.
    if (const float GLOW = dragGlow(monitor, LAYOUT, MOUSELOCAL); GLOW > 0.001F) {
        const auto ACCENT = Hyprdrive::Config::highlightColor();

        // A stack of expanded rounded rects with a falling alpha. There is no
        // soft-shadow primitive to hand, and a real blur for a halo would be a
        // third full-monitor kawase every frame of a drag; three rings read as a
        // glow and cost three rects.
        // ponytail: swap for a proper falloff if this ever looks banded.
        constexpr int RINGS = 3;
        for (int i = RINGS; i >= 1; --i) {
            const float SPREAD = DRAG_GLOW_PX * monitor->m_scale * sc<float>(i) / sc<float>(RINGS);

            CRectPassElement::SRectData ring;
            ring.box           = LAYOUT.pill.copy().expand(SPREAD);
            ring.color         = ACCENT.modifyA(GLOW * DRAG_GLOW_ALPHA / sc<float>(i * i));
            ring.round         = LAYOUT.rounding + sc<int>(std::round(SPREAD));
            ring.roundingPower = 2.F;
            g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(ring));
        }
    }

    // X13/X29/X86: dock pill. Live on the desktop (single surface). In overview
    // it samples the same decoration:blur:* behind-chrome buffer as the strip
    // so strength cannot drift with search-result count. Frost itself is
    // skipped when decoration:blur:enabled is off, matching Omarchy windows.
    frostedPlate(monitor, LAYOUT.pill, LAYOUT.rounding, 1.F);

    // Hyprland border weight, floored at 1 so a theme with border_size 0 still
    // frames the pill. Coloured like a window's border (user, 2026-10-16): the
    // resting (inactive) colour until the pointer is on the dock or using it,
    // then the accent, faded between on Hyprland's `border` animation
    // (BorderFade.hpp) -- in the overview and on the desktop alike.
    {
        const int  BORDERSIZE = std::max(1, Hyprdrive::Config::getValue<int>("general:border_size"));
        const auto COLOR      = BorderFade::color(monitor, BorderFade::key("dock-pill", sc<uint64_t>(monitor->m_id)), dockInUse(monitor, LAYOUT, MOUSELOCAL),
                                                  Hyprdrive::Config::restingBorderColor(), Hyprdrive::Config::highlightColor().modifyA(1.F));

        CBorderPassElement::SBorderData border;
        border.box           = LAYOUT.pill.copy().round();
        border.grad1.reset(COLOR);
        border.a             = 1.F;
        border.borderSize    = BORDERSIZE;
        border.round         = LAYOUT.rounding;
        border.roundingPower = 2.F;
        g_pHyprRenderer->m_renderPass.add(makeUnique<CBorderPassElement>(border));
    }

    // (user, 2026-08-03): GNOME lights the hovered app cell with a translucent
    // lighter fill and a slightly stronger outline, and labels it. Mouse coords
    // are global logical; the layout is monitor-local RENDER px.
    if (HASHOVER || HOVERAPPS) {
        // (user, 2026-08-03) A flat fill, no outline. The radius is CONCENTRIC
        // with the pill rather than a constant: the cell is inset from the pill
        // by the same amount on every side, so its curve only reads as parallel
        // to the dock's if the radius drops by that inset too.
        // X56 (user, 2026-08-06): themed, so the dock re-colours with
        // everything else when the Hyprland theme changes. The alpha is the
        // tuned value and stays -- only the hue comes from the config.
        const auto ACCENT = Hyprdrive::Config::highlightColor();

        CRectPassElement::SRectData fill;
        fill.box           = hoverBox(HASHOVER ? HOVEREDITEM->iconBox : LAYOUT.showApps, monitor->m_scale);
        fill.color         = ACCENT.modifyA(0.22F);
        fill.round         = std::max(0, LAYOUT.rounding - sc<int>(std::round(hoverInset() * monitor->m_scale)));
        fill.roundingPower = 2.F;
        g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(fill));
    }

    // X32/G12. Where the dragged app would land. Drawn under the icons so a
    // caret at the very first slot is not sitting on top of one.
    if (const CBox CARET = caretBox(LAYOUT, caretIndex(), monitor->m_scale); !CARET.empty()) {
        CRectPassElement::SRectData caret;
        caret.box           = CARET;
        caret.color         = CHyprColor{1.F, 1.F, 1.F, 0.90F};
        caret.round         = sc<int>(std::round(CARET.width / 2.F));
        caret.roundingPower = 2.F;
        g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(caret));
    }

    // ONE walk of the window list for the whole icon row, and one read of each
    // config key and theme colour the row needs -- all four were being asked
    // per ITEM, per monitor, per frame (user, 2026-09-14).
    const auto       TALLY    = buildTally();
    const bool       SHOWDOT  = Config::getValue<bool>(KEY_SHOW_RUNNING);
    const int        DOTSIZE  = Config::getValue<int>(KEY_DOT_SIZE);
    const CHyprColor DOTCOLOR = Hyprdrive::Config::dockIndicatorColor();
    const bool       DANCE    = Config::getValue<bool>(KEY_DANCE_URGENT);
    const bool       BADGES   = badgesEnabled();
    const double     RESTPX   = restingIconPx(monitor); // the texture size every icon below asks for

    for (const auto& ITEM : LAYOUT.items) {
        // Whitened for the SCRATCHPAD, for the same reason the user asked for it
        // on Show Applications (2026-08-06, below): the only icon that suits it
        // is symbolic -- near-black artwork on a dark frosted pill, beside a row
        // of full-colour app icons. Every other item here brings its own colour.
        if (const auto TEX = OverviewRender::appIcon(ITEM.icon, ITEM.scratchpad, false, RESTPX); TEX && TEX->ok()) {
            CTexPassElement::SRenderData data;
            data.tex = TEX;
            data.box = ITEM.iconBox;

            // Y8. The ICON leaves the pill, the running dot and the badge stay
            // where they are -- which is what macOS does and what keeps the hop
            // from reading as the whole cell coming loose.
            data.box.y -= bounceLift(ITEM.wmClass, sc<float>(ITEM.iconBox.height));

            // V11 V4. A stack has no window class, so Y8's hop cannot key on
            // one -- its acknowledgement and its refusal are keyed on the path
            // instead. Both are additive with the app hop above, which cannot
            // be running on the same cell.
            // Dock stacks B: the Moses gap's faint folder; nothing else of it.
            if (ITEM.placeholder) {
                data.a = 0.45F * std::clamp(ITEM.appear, 0.F, 1.F);
                if (Stacks::drawableBox(data.box))
                    g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(data));
                continue;
            }

            // Spring loading: a held drag waiting on this app wears the ring,
            // then the icon flashes twice before the app comes forward.
            if (const float FLASH = springFlash(ITEM); FLASH > 0.F)
                data.a *= 1.F - 0.6F * FLASH;

            // Dock stacks B: the cell a drag of files would land in lifts and
            // takes the accent ring.
            if (springArmed(ITEM) || (ITEM.stack && !ITEM.stackPath.empty() && ITEM.stackPath == DockDrop::hoveredStack()) || (!ITEM.stack && ITEM.uri == "trash:///" && DockDrop::hoveringTrash())) {
                data.box.y -= std::round(0.12 * ITEM.iconBox.height);
                CBorderPassElement::SBorderData ring;
                const double PAD = std::round(3.0 * monitor->m_scale);
                ring.box        = CBox{data.box.x - PAD, data.box.y - PAD, data.box.width + 2 * PAD, data.box.height + 2 * PAD}.round();
                ring.grad1      = ::Config::CGradientValueData(accentColor());
                ring.round      = sc<int>(std::round(ring.box.width * 0.22));
                ring.borderSize = std::max(1, sc<int>(std::round(2.0 * monitor->m_scale)));
                if (Stacks::drawableBox(ring.box))
                    g_pHyprRenderer->m_renderPass.add(makeUnique<CBorderPassElement>(ring));
            }

            // Dock stacks: the Trash hops when something lands in it.
            if (!ITEM.stack && ITEM.uri == "trash:///")
                data.box.y -= Stacks::bounceLift("trash:///", sc<float>(ITEM.iconBox.height));

            if (ITEM.stack) {
                data.box.y -= Stacks::bounceLift(ITEM.stackPath, sc<float>(ITEM.iconBox.height));
                data.box.x += Stacks::shakeOffset(ITEM.stackPath, sc<float>(ITEM.iconBox.width));
            }

            // X31 `dance-urgent-applications`. CBox::rot is radians about the
            // box's own centre, which is the reference's pivot (0.5, 0.5) --
            // so the wiggle is one assignment, not a transform stack.
            if (ITEM.running && ITEM.uri.empty() && DANCE && itemUrgent(ITEM, TALLY))
                data.box.rot = wiggleRadians();

            // The hover lean, the press, and the light on the icon's face, in
            // perspective -- DockTilt. The squish is keyed on the same subject
            // the tooltip uses, so a scratchpad slot and an app never share one.
            // Dock stacks A: a stack draws its own cell -- the pile of its newest
            // items, the folder, or a dimmed missing folder -- without the tilt.
            const auto& PRESSKEY = ITEM.subject;
            const auto  POSE     = DockTilt::pose(monitor->m_id, PRESSKEY, pointerIn(ITEM.iconBox, MOUSE, monitor->m_scale), pressDepth(monitor->m_id, PRESSKEY),
                                                  pressAt(monitor->m_id, PRESSKEY));
            if (ITEM.stack && ITEM.stackDir && !ITEM.placeholder && pointerIn(ITEM.iconBox, MOUSE, monitor->m_scale))
                Stacks::warm(ITEM.stackPath, monitor->m_scale);
            if (!(ITEM.stack && Stacks::drawCell(monitor, ITEM, data.box, std::clamp(ITEM.appear, 0.F, 1.F), POSE)))
                DockTilt::queue(TEX, data.box, std::clamp(ITEM.appear, 0.F, 1.F), POSE);
        }

        // Both of these are deliberately OUTSIDE the texture check: an app with
        // no desktop entry still runs and can still have a count, and losing its
        // indicators as well as its icon would hide it twice over.
        //
        // X32 gates them on RUNNING: a pinned favourite with no window is the
        // one icon in the pill that must NOT get a dot, and that absence is the
        // only thing distinguishing it from the app next to it.
        if (ITEM.running) {
            if (SHOWDOT)
                renderRunningDot(monitor, LAYOUT, ITEM, TALLY, DOTSIZE, DOTCOLOR);

            renderBadge(monitor, ITEM, BADGES);
        }

        // X34. Over the badge and the dot, because it is transient and it is the
        // thing being read for as long as it is up. OUTSIDE the running gate:
        // a pinned app that is not running still has a key.
        renderNumberOverlay(monitor, ITEM, sc<int>(&ITEM - LAYOUT.items.data()));
    }

    // THE ARRANGEMENT's leavers: an app that closed shrinks and fades where it
    // stood while its neighbours close the gap over it. Only here -- they are
    // not in LAYOUT.items, so nothing can hover or click them on the way out.
    // Shifted by the same autohide slide the live items got.
    if (const auto ARR = g_arrange.find(monitor->m_id); ARR != g_arrange.end() && !ARR->second.exits.empty()) {
        const double DY = LAYOUT.pill.y - arranged(monitor->m_id, baseLayout(monitor)).pill.y;
        for (const auto& EXIT : ARR->second.exits) {
            const float T = std::clamp(Hyprdrive::Config::animElapsedMs(EXIT.since) / EXIT_MS, 0.F, 1.F);
            if (T >= 1.F)
                continue;
            const auto TEX = OverviewRender::appIcon(EXIT.item.icon, EXIT.item.scratchpad, false, RESTPX);
            if (!TEX || !TEX->ok())
                continue;
            const float U = 1.F - T;
            CTexPassElement::SRenderData data;
            data.tex = TEX;
            data.box = scaledAboutCentre(EXIT.item.iconBox, 0.2F + 0.8F * U * U).translate({0.0, DY}).round();
            data.a   = U * U;
            g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(data));
        }
    }

    // X32 Show Applications, drawn last of the icon row so its cell is complete
    // before the plates go over the top.
    if (!LAYOUT.showApps.empty()) {
        // User, 2026-08-06: "make the Show Applications icon white, it's too
        // dark". view-app-grid-symbolic is near-black artwork sitting on a dark
        // frosted pill; every other icon in the dock is a full-colour app icon,
        // so this is the only one that needed it.
        // ...white on a DARK dock only: on a light theme the white dots vanished
        // into the plate (user, 2026-09-29), and the artwork's own near-black
        // is the readable one there.
        if (const auto TEX = OverviewRender::appIcon(Config::getValue<std::string>(KEY_SHOW_APPS_ICON), !Config::isLightTheme(), false, RESTPX); TEX && TEX->ok()) {
            DockTilt::queue(TEX, LAYOUT.showApps, 1.F,
                            DockTilt::pose(monitor->m_id, "show-apps", pointerIn(LAYOUT.showApps, MOUSE, monitor->m_scale), pressDepth(monitor->m_id, "show-apps"),
                                           pressAt(monitor->m_id, "show-apps")));
        }
    }

    // V3. THE ICON THE POINTER IS CARRYING, and the one flying home after a
    // miss. Both are the same clone of the same texture at the same cell size,
    // which is the point: the thing that lands back in the pill has to be the
    // thing that left it. Drawn over the whole icon row (it passes across it on
    // the way out) and under the launch blow-up (which is the answer to a drop
    // that DID land, and belongs on top of the one that did not).
    //
    // No frost: this is an icon clone, exactly as the launch zoom is, not a
    // plate. The dock's glass is the surface it comes off and goes back to.
    if (const auto TEX = ghostShowing() && g_dockDrag.drag->monitor == monitor->m_id ? OverviewRender::appIcon(g_dockDrag.drag->icon, false, false, RESTPX) : nullptr; TEX && TEX->ok()) {
        CTexPassElement::SRenderData data;
        data.tex = TEX;
        data.box = ghostBoxAt(monitor, g_dockDrag.drag->home, g_dockDrag.drag->grab, g_dockDrag.drag->at);
        data.a   = DRAG_GHOST_ALPHA;
        g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(data));
    }

    if (g_dockDrag.ghostHome && g_dockDrag.ghostHome->monitor == monitor->m_id) {
        const float E = houseEase(false, std::clamp(elapsedMs(g_dockDrag.ghostHome->startedAt) / GHOST_HOME_MS, 0.F, 1.F));

        if (const auto TEX = OverviewRender::appIcon(g_dockDrag.ghostHome->icon, false, false, RESTPX); TEX && TEX->ok()) {
            CTexPassElement::SRenderData data;
            data.tex = TEX;
            data.box = lerpBox(g_dockDrag.ghostHome->from, g_dockDrag.ghostHome->home, E).round();
            // Fades as it arrives, so the clone and the real icon underneath it
            // are never two solid icons in one cell for a frame.
            data.a = DRAG_GHOST_ALPHA * (1.F - E);
            g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(data));
        }
    }

    // Y6. GNOME's launch blow-up, drawn over the icon row so the growing clone
    // is not clipped by the neighbours it passes over.
    for (const auto& ZOOM : g_flight.launchZooms) {
        if (ZOOM.monitor != MONITOR_INVALID && ZOOM.monitor != monitor->m_id)
            continue;
        const auto TEX = OverviewRender::appIcon(ZOOM.icon, false, false, RESTPX);
        if (!TEX || !TEX->ok())
            continue;

        const float LT    = std::clamp(elapsedMs(ZOOM.startedAt) / LAUNCH_ZOOM_MS, 0.F, 1.F);
        const float E     = Motion::housePreset() ? easeOutQuad(LT) : Motion::ease(Motion::ERole::DECEL, LT);
        const float GROWN = 1.F + (LAUNCH_ZOOM_SCALE - 1.F) * E;
        const float W     = sc<float>(ZOOM.from.width) * GROWN;
        const float H     = sc<float>(ZOOM.from.height) * GROWN;

        // pivot_point (0.5, 0.5): it grows about its own centre.
        float x = sc<float>(ZOOM.from.x) - (W - sc<float>(ZOOM.from.width)) / 2.F;
        float y = sc<float>(ZOOM.from.y) - (H - sc<float>(ZOOM.from.height)) / 2.F;

        // Z11 (user): GNOME's blow-up drifts UPWARD off a bottom dock. Its
        // clamp is baked into the tween's END STATE and EASED from frame one
        // (`translation_y: containedY - scaledY` inside the same ease) --
        // per-frame clamping only engages once the box already hits the edge,
        // which read as "expands in all directions".
        const float MONW = sc<float>(monitor->m_size.x) * monitor->m_scale;
        const float MONH = sc<float>(monitor->m_size.y) * monitor->m_scale;
        const float FW   = sc<float>(ZOOM.from.width) * LAUNCH_ZOOM_SCALE;
        const float FH   = sc<float>(ZOOM.from.height) * LAUNCH_ZOOM_SCALE;
        const float FX   = sc<float>(ZOOM.from.x) - (FW - sc<float>(ZOOM.from.width)) / 2.F;
        const float FY   = sc<float>(ZOOM.from.y) - (FH - sc<float>(ZOOM.from.height)) / 2.F;
        x += (std::clamp(FX, 0.F, std::max(0.F, MONW - FW)) - FX) * E;
        y += (std::clamp(FY, 0.F, std::max(0.F, MONH - FH)) - FY) * E;

        CTexPassElement::SRenderData data;
        data.tex = TEX;
        data.box = CBox{std::round(x), std::round(y), std::round(W), std::round(H)};
        data.a   = 1.F - E;
        g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(data));
    }

    // Above the pill by construction (MENU_GAP), so the previews below cannot
    // overdraw it and it does not have to wait for them.
    // V11 V4. Over the icon row, under the menu: the menu is modal and the
    // popup is not, so a menu opened over an open fan must be on top of it.
    // It reads its own clock, like every other easing surface here: the popup
    // runs off wall time rather than off the frame stamp, so there is nothing
    // for a caller to hand it.
    Stacks::render(monitor, LAYOUT);

    renderMenu(monitor, LAYOUT);

    const auto NOW = Time::steadyNow();

    // Y5. The flight is a real window at real SIZE, not a thumbnail, so where it
    // is legal it draws the way the overview draws a tile -- live blur, frost
    // scaled to the render scale, real decorations.
    //
    // It takes that path EVERYWHERE, desktop included -- which is the point of
    // the user's instruction. What differs off the overview is only PASS
    // OWNERSHIP: inside the overview the plugin owns the render pass and the
    // path may flush it mid-draw, while at RENDER_POST_WINDOWS a flush renders
    // and CLEARS everything Hyprland queued for the rest of the frame. Measured:
    // flushing there wedged the frame loop and stranded a restoring window on
    // the stash permanently. Blur scaling and forced live blur do not depend on
    // the flush, so the frost is correct in both.
    // The flight draws through drawFlightSurface (below, named so
    // FlightController.cpp can call it); this is the pass-ownership fact it
    // needs, computed once per frame here where the render context is.
    const bool OWNSPASS = scrollOverviewForMonitor(monitor) != nullptr;

    // Hover previews were drawn under the pill, above.

    // The menu says the app's name louder than the tooltip does, and the two
    // plates would otherwise sit on top of each other.
    //
    // A preview under the pointer names ITS OWN window -- the full title, not
    // the app -- one gap above the panel. The icon still names the APP: with
    // the panel up that plate sits above the previews, centred on the icon,
    // so the wall of thumbnails does not bury "what is this". Empty-name is
    // the fade-OUT: without it the plate would pop off instead of leaving.
    const auto CELL = panelCellAt(monitor, MOUSE);

    // An open stack's fan or pane covers where its tooltip would sit (user,
    // 2026-09-24): the popup already says what it is.
    if (g_menu || Stacks::isOpen())
        renderTooltip(monitor, LAYOUT, CBox{}, "");
    else if (CELL)
        renderTooltip(monitor, LAYOUT, CELL->first, CELL->second->m_title, sc<float>(g_hover.drawn.y), tooltipSubject(CELL->second),
                      peekWindowsOn(monitor, {CELL->second}));
    else if (HASHOVER) {
        const float ABOVE = (panelOn(monitor) && g_hover.drawn.height >= 1) ? sc<float>(g_hover.drawn.y) : 0.F;
        // Dock stacks A: a stack's tooltip says when its folder is gone (and,
        // later, a trash error) after its name.
        std::string tipName = itemName(*HOVEREDITEM);
        if (HOVEREDITEM->stack)
            if (const auto SUFFIX = Stacks::tooltipSuffix(HOVEREDITEM->stackPath); !SUFFIX.empty())
                tipName += "  \u00b7  " + SUFFIX;
        renderTooltip(monitor, LAYOUT, HOVEREDITEM->iconBox, tipName, ABOVE, HOVEREDITEM->subject,
                      HOVEREDITEM->uri.empty() && !HOVEREDITEM->scratchpad ? peekWindowsOn(monitor, itemWindows(*HOVEREDITEM)) : std::vector<PHLWINDOW>{});
    } else if (HOVERAPPS)
        renderTooltip(monitor, LAYOUT, LAYOUT.showApps, scrollOverviewForMonitor(monitor) ? "Exit Overview" : "Show Overview", 0.F, "show-apps");
    else
        renderTooltip(monitor, LAYOUT, CBox{}, "");

    // X46/Y5/Y4/Z16. The flight loop lives in FlightController.cpp now -- WHOLE,
    // drawing and all. It advances every flight one frame and paints it in this
    // pass, one flight at a time, in the order it always did.
    //
    // It is deliberately not a tick() that returns a draw list. That version was
    // written, compiled, and passed z4/z4c/z6/z7 before z5 caught it:
    // mutate-every-flight-then-draw-every-flight moved when the dock damages
    // itself relative to when its flights advance, and three reversals in a row
    // then fired against flights that had never drawn a frame. The extraction is
    // the state machine; the loop is a loop.
    Flight::renderFlights(monitor, LAYOUT, NOW, OWNSPASS);

    // ASKED FOR HERE, after the flight loop has advanced -- see the note on
    // needsFrames for why this site cannot be folded into the other one.
    // The ponytail note that used to sit here -- "whole-monitor damage at the
    // refresh rate, the pill's rectangle would do" -- is answered by
    // dockDamageRegion, which is that conversion.
    if (needsFrames(monitor, LAYOUT, TALLY))
        pumpFrame(monitor, dockDamageRegion(monitor, LAYOUT));
    // An icon drawn from a stand-in size while its own waits for the decode
    // budget (a size slider being dragged): one more frame decodes the next few.
    else if (OverviewRender::takeAppIconsPending())
        pumpFrame(monitor, dockDamageRegion(monitor, LAYOUT));
}


// The runnable check for X27 is scripts/x27_gate.py: it measures the pill that
// actually landed on screen against the reference numbers. A pure-math assert
// was considered and dropped -- it would have had to link the whole plugin, and
// the proportions are exactly the thing a pixel measurement proves better.
//
// X25's is scripts/x25_gate.py, and it asks the one question a still frame
// cannot: two frames of the same minimized window, and the preview must have
// CHANGED between them.

}
