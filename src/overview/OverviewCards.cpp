// COverview: workspace card rendering, window draw, redrawAll.
// Cut from Overview.cpp by pure code motion.
#include "OverviewParts.hpp"
#include "MotionClock.hpp"

using namespace OverviewDetail;


void COverview::renderWorkspaceBackground(PHLMONITOR monitor, size_t workspaceIdx, size_t activeIdx, float workspacePitch, float renderScale, int wallpaperMode,
                                                const Time::steady_tp& now) {
    const auto& workspaceImage = images[workspaceIdx];
    if (!workspaceImage || !workspaceImage->pWorkspace)
        return;

    const auto WORKSPACEOFFSET = workspaceOverviewOffset(workspaceIdx, activeIdx, workspacePitch);
    const auto RESTBOX          = getOverviewWorkspaceBox(monitor, renderScale, viewOffset->value(), WORKSPACEOFFSET, layout);
    const auto WORKSPACEBOX     = depthScaled(RESTBOX, RESTBOX.middle(), cardDepth(workspaceIdx)); // further back while behind another
    const auto WORKSPACEALPHA   = workspaceOverviewAlpha(workspaceIdx);

    if (!overviewBoxIntersectsMonitor(WORKSPACEBOX, monitor))
        return;

    const auto workspace         = workspaceImage->pWorkspace;
    const bool WASVISIBLE        = workspace->m_visible;
    const bool WASFORCERENDERING = workspace->m_forceRendering;
    workspace->m_visible         = true;
    workspace->m_forceRendering  = true;

    auto restoreWorkspaceState = Hyprutils::Utils::CScopeGuard([workspace, WASVISIBLE, WASFORCERENDERING] {
        workspace->m_visible        = WASVISIBLE;
        workspace->m_forceRendering = WASFORCERENDERING;
    });

    renderCardBody(monitor, WORKSPACEBOX, renderScale, wallpaperMode, WORKSPACEALPHA, now,
                   trackpadDetentActive && workspaceIdx == trackpadDetentWorkspace ? std::clamp(trackpadDetentFocus, 0.F, 1.F) : 0.F);
}

void COverview::renderCardBody(PHLMONITOR monitor, const CBox& box, float renderScale, int wallpaperMode, float alpha, const Time::steady_tp& now, float focus) {
    // ONE CARD AT A TIME while cards overlap (a transition: one sliding out
    // from under another, one passing behind the desktop in view). The fill
    // below is drawn at once but the shadow and outline are QUEUED, so every
    // card's outline landed on top of every card's fill: the desktop beneath
    // showed its border through the one above (user's recordings, 2026-11-06).
    // At rest the cards never overlap, and nothing is flushed.
    const bool INORDER = workspaceInsertTransition.active && workspaceInsertProgress->value() < 1.F;
    renderOverviewWorkspaceShadow(monitor, box, renderScale, wallpaperMode == 0, alpha);
    if (INORDER)
        OverviewRender::flushPass(monitor);

    // X86: workspace cards do NOT get their own live COverviewBlurPassElement.
    // Windows on the card use the same renderWindow live frost path as main tiles
    // (decoration:blur:*, blockBlurOptimization when needed). A whole-card live
    // blur was a different technique and compounded once per visible desktop,
    // so empty-search (many cards) frosted harder than search mode for free.
    // Wallpaper stays the card fill; glass comes from the windows themselves.

    if (wallpaperMode != 0 && alpha > 0.001F)
        renderWallpaperLayers(monitor, box, renderScale, now, alpha);

    // GNOME-style rounding (user-directed): composite the native-size wallpaper
    // FB into the card box with shader round/roundingPower. The corners are
    // genuinely clipped and antialiased against whatever is behind them --
    // shadow included -- which no painted-on mask could match exactly.
    if (alpha > 0.001F && frost.cardWallpaperFB && frost.cardWallpaperFB->isAllocated() && frost.cardWallpaperFB->getTexture()) {
        const auto    CARDROUND = getOverviewCardRounding(monitor, Hyprdrive::Config::getScale());
        const auto    BOXPX     = box.copy().round();
        const CRegion DMG{BOXPX};

        g_pHyprRenderer->draw(
            CTexPassElement::SRenderData{
                .tex           = frost.cardWallpaperFB->getTexture(),
                .box           = BOXPX,
                .a             = alpha,
                .damage        = DMG,
                .round         = CARDROUND.rounding,
                .roundingPower = CARDROUND.power,
                .flipEndFrame  = true,
            },
            DMG);
    } else if (alpha > 0.001F)
        renderOverviewLayerLevel(monitor, ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM, box, renderScale, now);

    // Outline the floating card so it separates from the dimmed canvas behind
    // it. Same accent the tile ring and strip use; same weight as Hyprland
    // window borders, floored at 1 so a theme with border_size 0 still frames
    // the card.
    if (alpha > 0.001F) {
        const auto CARDROUND   = getOverviewCardRounding(monitor, Hyprdrive::Config::getScale());
        const int  BORDERSIZE  = std::max(1, Hyprdrive::Config::getValue<int>("general:border_size"));
        const auto ACCENT      = Hyprdrive::Config::highlightColor();

        // While the workspace drag is running, the card the detent is settling
        // on wears a heavier ring. FOCUS is 1 on a card centre and 0 at the
        // midpoint between two, so the weight grows into the detent rather than
        // flicking on at the crossing -- which is what makes it read as the pad
        // catching rather than as a redraw.
        const float FOCUS = focus;

        CBorderPassElement::SBorderData border;
        border.box           = box.copy().round();
        border.grad1.reset(ACCENT.modifyA(1.F));
        border.a             = alpha;
        // Ceil, not round: at BORDERSIZE 1 a 0.6x growth rounds back to 1 and
        // the whole effect disappears on a thin-border theme.
        border.borderSize    = BORDERSIZE + sc<int>(std::ceil(FOCUS * sc<float>(BORDERSIZE)));
        border.round         = CARDROUND.rounding;
        border.roundingPower = CARDROUND.power;
        g_pHyprRenderer->m_renderPass.add(makeUnique<CBorderPassElement>(border));
        if (INORDER)
            OverviewRender::flushPass(monitor);

        // The NUMBER is not drawn here. This is the background pass -- it runs
        // before the card's own windows -- so a label written here is painted
        // over by every window on the desktop it is naming, and TileChrome's
        // app icon straddles the tile's bottom edge exactly where a
        // bottom-anchored plate lands. See renderDetentBadge, which draws it
        // centred on the card in a pass of its own after the windows.
    }
}

void COverview::renderWorkspaceLive(PHLMONITOR monitor, size_t workspaceIdx, size_t activeIdx, float workspacePitch, float renderScale, int wallpaperMode, const Time::steady_tp& now) {
    const auto& workspaceImage = images[workspaceIdx];
    if (!workspaceImage || !workspaceImage->pWorkspace)
        return;

    const auto WORKSPACEOFFSET = workspaceOverviewOffset(workspaceIdx, activeIdx, workspacePitch);
    const auto WORKSPACEBOX     = getOverviewWorkspaceBox(monitor, renderScale, viewOffset->value(), WORKSPACEOFFSET, layout);
    const auto VISIBLEBOX       = workspaceOverviewVisibleBox(workspaceIdx, WORKSPACEBOX, renderScale, monitor);
    const bool WORKSPACEVISIBLE = overviewBoxIntersectsMonitor(VISIBLEBOX, monitor);
    const float DEPTH           = cardDepth(workspaceIdx);
    const CBox  CARDBOX         = depthScaled(WORKSPACEBOX, WORKSPACEBOX.middle(), DEPTH); // the clip: the card as drawn

    // B2: a promoted tile is drawn here, in its own workspace's pass — that is
    // where the m_visible / m_forceRendering override below lives — but at the
    // active card's offset. So an off-screen workspace still has to be walked.
    const auto ACTIVECARDBOX = getOverviewWorkspaceBox(monitor, renderScale, viewOffset->value(), 0.F, layout);
    const bool HASPROMOTED   = !WORKSPACEVISIBLE && std::ranges::any_of(workspaceImage->windows, [&](const auto& ref) {
        return overviewGridPromoted(getOverviewWindowToShow(ref.lock()), monitor);
    });

    // A SUSPENDED CLIENT CANNOT ASK FOR THE FRAME THAT WOULD WAKE IT (user,
    // 2026-09-19: a fullscreen YouTube video in Zen on another desktop "shows
    // static when in fact it's playing").
    //
    // Hyprland marks windows whose workspace is not active as suspended over
    // xdg-toplevel, and Firefox honours that literally: it stops painting AND
    // stops decoding. It then never calls CWLSurfaceResource::frame, so it never
    // reaches shouldAllowSurfaceFrame at all -- measured, zen was absent from the
    // gate trace entirely while every terminal on screen was in it. No amount of
    // adaptive polling reaches it, because a poll IS a frame callback and the
    // client has stopped listening for one.
    //
    // It belongs here rather than in sendOverviewFrameCallbacks: that runs only
    // on the pulses that do NOT repaint, and on a repainting frame the callbacks
    // go out through the render tail instead, so a window could stay suspended
    // for as long as anything on the overview was animating. This runs whenever
    // the card is drawn, which is the definition of the tile being on screen.
    // m_forceRendering below is the other half and not a substitute: it makes
    // the compositor DRAW the client's last buffer, not the client produce a new
    // one. Hyprland re-suspends on its own once the tile stops being drawn, and
    // the frame RATE stays the adaptive gate's decision downstream.
    if (!WORKSPACEVISIBLE && !HASPROMOTED)
        return;

    const auto workspace         = workspaceImage->pWorkspace;
    const bool WASVISIBLE        = workspace->m_visible;
    const bool WASFORCERENDERING = workspace->m_forceRendering;
    workspace->m_visible         = true;
    workspace->m_forceRendering  = true;

    auto restoreWorkspaceState = Hyprutils::Utils::CScopeGuard([workspace, WASVISIBLE, WASFORCERENDERING] {
        workspace->m_visible        = WASVISIBLE;
        workspace->m_forceRendering = WASFORCERENDERING;
    });

    // JUXTAPOSITION (user, 2026-09-29, a recording of the open animation): with
    // the chrome in a second pass, every app icon sat on top of EVERY tile -- a
    // tile flying over another wore the other's icon on its face. Chrome is now
    // drawn with its own tile, so it stacks exactly as the tiles do, and the
    // case the second pass was for -- a hovered tile growing into its
    // neighbours -- is met by drawing the hovered tile LAST: grown body and
    // chrome over the neighbours, as the one being pointed at should be. The
    // icon's overhang below a tile stays inside the grid gap.
    // The same progress the pill, the strip and the result cards fade on, so the
    // whole overview arrives and leaves as one thing.
    const float CHROMEFADE = overviewGridProgress(renderScale);
    const auto  HOVEREDTILE = TileChrome::hovered();
    // THE OTHER DESKTOPS' WINDOWS FADE IN WITH THE REVEAL (user, 2026-11-03,
    // a recording: "the overhang windows end up sliding in to adjacent desktops
    // out of nowhere ... a fade-in for windows in adjacent desktops during
    // overview reveal duration"). A canvas desktop's windows hang past its card,
    // so as the cards zoom they swept in at full strength over the desktop being
    // opened from (or closed to). They now arrive and leave with the overview's
    // own progress, as its chrome does; the current desktop stays solid, and a
    // tile a search promoted into it counts as its own.
    const bool  NEIGHBOUR      = workspaceIdx != activeIdx;
    // The neighbours' windows fade with the open itself -- 0 closed, 1 open,
    // wherever the fingers hold it and back again -- so the ones hanging into
    // view slide in rather than appearing whole (user, 2026-11-03). Their
    // frame and shadow fade with them (renderWindowLive: SRenderParams::alpha).
    const auto  NEIGHBOURALPHA = [&](bool promoted) { return NEIGHBOUR && !promoted ? CHROMEFADE : 1.F; };

    const auto renderOverviewWindow = [&](const PHLWINDOW& window) {
        // F3-b outbound: a releasing tile no longer matches, so the search half
        // of shouldShowOverviewWindow would delete it on its first outbound
        // frame. overviewGridReleasing only admits a window that was legally
        // shown a moment ago and is still validMapped.
        //
        // ONE predicate, and windowAtOverviewPoint asks the same one -- a tile
        // this function paints is a tile that function can hand back.
        bool PROMOTED = false;
        if (!overviewTileShown(window, monitor, &PROMOTED))
            return;
        if (drag.activeWindow && window == getOverviewWindowToShow(drag.activeWindow.lock()))
            return;

        if (!WORKSPACEVISIBLE && !PROMOTED)
            return;

        auto       windowBox = TileChrome::scaledBox(window, getOverviewWindowBox(window, monitor, renderScale, viewOffset->value(), WORKSPACEOFFSET, layout));
        // With its card, when that is further back (cardDepth); a promoted tile
        // sits on the card in view, not this one.
        if (!PROMOTED)
            windowBox = depthScaled(windowBox, WORKSPACEBOX.middle(), DEPTH);
        // A TILE CHANGING DESKTOPS under an insert/remove (a fullscreen app to
        // its own desktop and back): from where it was drawn, on the other
        // card, to its place on this one -- unclipped, it crosses between them.
        bool travelling = false;
        if (workspaceInsertTransition.active && workspaceInsertProgress->value() < 1.F)
            if (const auto IT = workspaceInsertTransition.tileFrom.find(window.get()); IT != workspaceInsertTransition.tileFrom.end() && IT->second.box.w >= 2) {
                const float T    = std::clamp(workspaceInsertProgress->value(), 0.F, 1.F);
                const CBox& FROM = IT->second.box;
                if (IT->second.card != workspaceKey(workspace)) {
                    windowBox  = CBox{FROM.x + (windowBox.x - FROM.x) * T, FROM.y + (windowBox.y - FROM.y) * T, FROM.w + (windowBox.w - FROM.w) * T,
                                      FROM.h + (windowBox.h - FROM.h) * T};
                    travelling = true;
                } else if (const CBox& C = IT->second.cardBox; !PROMOTED && C.w >= 2 && C.h >= 2 && CARDBOX.w >= 2 && CARDBOX.h >= 2) {
                    // THE WINDOWS LEFT BEHIND (a desktop a window just left
                    // re-tiles at once -- Hyprland does not animate a desktop
                    // it is not showing): from where they were on their card to
                    // where they are, IN the card's own terms, so the card's
                    // slide and depth carry them as well.
                    const CBox A = {(FROM.x - C.x) / C.w, (FROM.y - C.y) / C.h, FROM.w / C.w, FROM.h / C.h};
                    const CBox B = {(windowBox.x - CARDBOX.x) / CARDBOX.w, (windowBox.y - CARDBOX.y) / CARDBOX.h, windowBox.w / CARDBOX.w, windowBox.h / CARDBOX.h};
                    const CBox R = {A.x + (B.x - A.x) * T, A.y + (B.y - A.y) * T, A.w + (B.w - A.w) * T, A.h + (B.h - A.h) * T};
                    windowBox    = CBox{CARDBOX.x + R.x * CARDBOX.w, CARDBOX.y + R.y * CARDBOX.h, R.w * CARDBOX.w, R.h * CARDBOX.h};
                }
            }
        if (!overviewBoxIntersectsMonitor(windowBox, monitor))
            return;

        // INSTRUMENT (Membership::renderShadowEnabled). PAST the cull, so a line
        // here means a rectangle really was painted on this output.
        if (Membership::renderShadowEnabled()) {
            const auto LIVEWS = window->m_workspace ? window->m_workspace->m_id : WORKSPACE_INVALID;
            const auto CENTRE = windowBox.middle();
            // Would the hit-test hand this tile back? Same walk windowAtOverviewPoint does.
            const auto HIT    = windowAtOverviewPoint(CENTRE);
            if (HIT != window)
                Membership::renderDivergence("UNCLICK", workspace->m_id, LIVEWS, window->m_class,
                                             window->m_title + std::format("  box={},{} {}x{}  hit={}", (int)windowBox.x, (int)windowBox.y, (int)windowBox.width,
                                                                           (int)windowBox.height, HIT ? HIT->m_class : std::string{"(none)"}));
            if (PROMOTED && g_search.query.text.empty())
                Membership::renderDivergence("PROMONOQ", workspace->m_id, LIVEWS, window->m_class,
                                             window->m_title + std::format("  box={},{} {}x{}", (int)windowBox.x, (int)windowBox.y, (int)windowBox.width, (int)windowBox.height));
        }

        // TILES MOVED BY THE SEARCH (user, 2026-09-30: "0.5 motion blur on
        // windows coming in from other desktops / in motion while I type"):
        // promoted in from another card, sliding to their new slot as the
        // filter narrows, or easing home after it clears. Smeared from last
        // frame's box at half strength; the overview's own open/close zoom and
        // panning between desktops stay sharp.
        const auto PREVTILE = lastTileBoxes.find(window.get());
        // And the grid panning to another desktop from the KEYBOARD (user,
        // 2026-09-30), at the same 0.25 -- a trackpad pan follows the fingers
        // and stays sharp.
        const auto KEYRECENT = [](const Time::steady_tp& at) { return at.time_since_epoch().count() != 0 && Hyprdrive::Motion::msBetween(Time::steadyNow(), at) < 1000.F; };
        // The wheel's own site when the wheel moved it last.
        const bool WHEELLAST = KEYRECENT(wheelPanAt) && wheelPanAt >= keyPanAt && wheelPanAt >= keyBadgeBornAt;
        const bool KEYPAN    = viewOffset->isBeingAnimated() && (KEYRECENT(keyBadgeBornAt) || KEYRECENT(keyPanAt) || KEYRECENT(wheelPanAt));
        // And the open/close zoom when it PLAYS (a keypress) rather than
        // following the fingers (user, 2026-09-30: "works on the trackpad
        // toggle but not on keypress"), with the half-duration fade.
        const bool ZOOMING = scale->isBeingAnimated() && !m_isSwiping;
        // THE FIRST FRAME OF A KEYPRESS OPEN HAS NO LAST FRAME (user,
        // 2026-11-03: spamming Super, "no motion blur on overview windows fading
        // in, while trackpad does"). The smear runs from last frame's box, and
        // the frame an open starts on has none -- yet a keypress open covers
        // the most ground on exactly that frame, so a neighbour desktop's
        // window was drawn there sharp and see-through. A neighbour card is
        // its desktop zoomed about the screen's centre, so its box at the
        // zoom's start is this one scaled back by begun/now.
        std::optional<CBox> firstFramePrev;
        if (ZOOMING && NEIGHBOUR && !PROMOTED && PREVTILE == lastTileBoxes.end()) {
            const double   K = sc<double>(scale->begun()) / std::max(sc<double>(scale->value()), 0.01);
            const Vector2D C = monitor->m_transformedSize / 2.0;
            if (std::abs(K - 1.0) > 0.001)
                firstFramePrev = CBox{C.x + (windowBox.x - C.x) * K, C.y + (windowBox.y - C.y) * K, windowBox.w * K, windowBox.h * K};
        }
        const bool HAVEPREV = firstFramePrev || (PREVTILE != lastTileBoxes.end() && PREVTILE->second.box.w >= 2 && PREVTILE->second.box.h >= 2);
        const bool SEARCHMOTION = ((((!g_search.query.text.empty() || gridView().reflows.contains(window.get())) && !viewOffset->isBeingAnimated() || KEYPAN) &&
                                    !scale->isBeingAnimated()) ||
                                   ZOOMING) &&
            HAVEPREV;
        bool tileBlurred = false;
        if (SEARCHMOTION) {
            const CBox& P = firstFramePrev ? *firstFramePrev : PREVTILE->second.box;
            // Three of the user's blur settings (all a quarter of the frame's
            // motion by default -- user, 2026-09-30: 0.5 was too much): the
            // zoom playing from a keypress, the keyboard desktop pan, and tiles
            // moving under a search. Only the zoom has a clock to fade over.
            // The open decelerates, so its speed is up front; the close
            // accelerates, so its speed is at the end -- its progress is
            // mirrored, or the fade would be over before the tiles got going.
            const auto  SITE = ZOOMING ? Hyprdrive::FlightBlur::ESite::OVERVIEW :
                    KEYPAN            ? (WHEELLAST ? Hyprdrive::FlightBlur::ESite::WHEEL_PAN : Hyprdrive::FlightBlur::ESite::DESKTOP_SWITCH) :
                                        Hyprdrive::FlightBlur::ESite::SEARCH_TILES;
            const float PROG = ZOOMING ? (closing ? 1.F - scale->getPercent() : scale->getPercent()) : 0.F;
            const CBox  HALF = Hyprdrive::FlightBlur::shape(SITE, P, windowBox, PROG);
            // Only a real move: a frame-to-frame jump of a third of the screen
            // is a card swap, not motion.
            // (Not the first frame's: that one is computed, not a swap.)
            const bool JUMP = !firstFramePrev &&
                (std::abs(P.x - windowBox.x) > monitor->m_transformedSize.x / 3.0 || std::abs(P.y - windowBox.y) > monitor->m_transformedSize.y / 3.0);
            if (!JUMP)
                tileBlurred = Hyprdrive::FlightBlur::drawBlurred(
                    monitor, window, HALF, windowBox,
                    [&] {
                        renderWindowLive(monitor, window, windowBox, renderScale, now, PROMOTED ? &ACTIVECARDBOX : &CARDBOX, false, NEIGHBOURALPHA(PROMOTED), false);
                        // Its badge (the app icon) moves with it and smears
                        // with it (user, 2026-09-30).
                        TileChrome::render(monitor, window, windowBox, monitor->m_scale, CHROMEFADE);
                    },
                    Hyprdrive::FlightBlur::layoutSamples());
            if (tileBlurred)
                OverviewRender::flushPass(monitor);
        }
        if (!tileBlurred) {
            renderWindowLive(monitor, window, windowBox, renderScale, now, travelling ? nullptr : PROMOTED ? &ACTIVECARDBOX : &CARDBOX, false, travelling ? 1.F : NEIGHBOURALPHA(PROMOTED));
            TileChrome::render(monitor, window, windowBox, monitor->m_scale, CHROMEFADE);
        }
        lastTileBoxes[window.get()] = {windowBox, workspaceIdx, PROMOTED ? ACTIVECARDBOX : CARDBOX};
    };


    const auto fullscreenWindow  = overviewFullscreenPathWindow(workspace);
    const bool scrollingLayout   = isWorkspaceScrolling(workspace);
    const bool hasFullscreenPath = fullscreenWindow != nullptr;

    if (!scrollingLayout && hasFullscreenPath) {
        renderOverviewWindow(fullscreenWindow);
        OverviewRender::flushPass(monitor);
        for (const auto& windowRef : workspaceImage->windows) {
            const auto window = getOverviewWindowToShow(windowRef.lock());
            if (!shouldShowOverviewWindow(window) || !window->m_isFloating || window == fullscreenWindow)
                continue;

            renderOverviewWindow(window);
        }
        return;
    }

    bool       hoveredHere          = false;
    const auto renderWindowsByState = [&](bool floating) {
        for (const auto& windowRef : workspaceImage->windows) {
            const auto window = getOverviewWindowToShow(windowRef.lock());
            if (!window || window->m_isFloating != floating)
                continue;
            if (window == HOVEREDTILE) {
                hoveredHere = true; // drawn last, over its neighbours
                continue;
            }

            renderOverviewWindow(window);
        }
    };

    renderWindowsByState(false);
    renderWindowsByState(true);
    if (hoveredHere)
        renderOverviewWindow(HOVEREDTILE);
    renderClosingTiles(monitor, workspaceIdx, now);
}

// OMARCHY'S OWN CLOSE, borrowed rather than invented: `windowsOut` on this rice
// is `popin 87%` on a linear bezier at speed 1.49, with fadeOut alongside it.
// Hyprland speeds are deciseconds, so 1.49 is 149ms, and popin 87% means the
// window shrinks to 87% of its size about its centre while the alpha runs out.
// Read off the live animation tree rather than hardcoded from the theme file, so
// a user who retunes windowsOut gets the overview's copy retuned with it.
//
// The ANIMATION TREE, not Config::getValue: animations are not config values,
// and getValue on a key that does not exist dereferences an empty handle --
// the first version of this read `animations:windowsOut:speed` that way and
// would have taken the compositor down on the first close. Same accessor the
// constructor uses for windowsMove.
static float closingTileDurationMs() {
    const auto CONFIG = Config::animationTree()->getAnimationPropertyConfig("windowsOut");
    const auto VALUES = CONFIG && CONFIG->pValues ? CONFIG->pValues.lock() : CONFIG;
    if (!VALUES || !VALUES->internalEnabled)
        return 0.F; // windowsOut off = no close animation, same as the desktop
    return VALUES->internalSpeed > 0.01F ? VALUES->internalSpeed * 100.F : 149.F;
}

// BORROWED, not captured (measured 2026-09-23 with /tmp/hyprdrive-close.log):
// the first frame after window.close already finds the window DESTROYED -- foot
// exits the instant it is asked -- so there is never a frame in which to
// photograph it ourselves. What does exist on that frame is Hyprland's own
// snapshot, taken at unmap for its windowsOut. It is a MONITOR-sized
// framebuffer with the window at its real position, so the tile is drawn by
// scaling the whole snapshot until the window's region lands on the tile's box
// and clipping to that box.
//
// Waiting for the unmap is also what keeps a refused close honest: an app that
// answers "save changes?" stays mapped, no fadeout ever appears, and the entry
// times out instead of animating a ghost out from under a tile that is still
// there.
// Called once per workspace CARD per frame -- the current desktop and the
// neighbours peeking in at the edges. The ghost is drawn only on the card whose
// pass drew the tile; drawn on every card it stacked its alpha and faded late.
void COverview::renderClosingTiles(PHLMONITOR monitor, size_t cardIdx, const Time::steady_tp& now) {
    if (closingTiles.empty() || !monitor)
        return;

    // The windowsOut leaf, read once per overview frame rather than once per
    // card (the memo the overview's other per-frame reads use): a live change
    // to it still shows on the next frame, as before.
    static uint64_t memoFrame  = 0;
    static bool     memoFilled = false;
    static float    memoDur    = 0.F;
    if (!memoFilled || memoFrame != g_overviewFrame) {
        memoFrame  = g_overviewFrame;
        memoDur    = closingTileDurationMs();
        memoFilled = true;
    }
    const float     DURMS    = memoDur;
    constexpr float WAITMS   = 1000.F; // longest a still-mapped window may take to actually go
    constexpr float POPIN    = 0.87F;

    for (auto& tile : closingTiles) {
        const auto WINDOW = tile.window.lock();

        if (!tile.captured) {
            // Still mapped: the close was asked for, not done. Wait for it.
            if (WINDOW && WINDOW->m_isMapped)
                continue;

            // Gone. Find the snapshot Hyprland took of it -- the one whose box
            // holds the window's last real geometry, nearest first.
            const CBox REALPX{(tile.realBox.x - monitor->m_position.x) * monitor->m_scale, (tile.realBox.y - monitor->m_position.y) * monitor->m_scale,
                              tile.realBox.width * monitor->m_scale, tile.realBox.height * monitor->m_scale};
            for (const auto& FADE : Desktop::fadingOutState()->fadeouts()) {
                if (!FADE || FADE->monitor().lock() != monitor)
                    continue;
                const auto FB = FADE->framebuffer();
                if (!FB || !FB->isAllocated())
                    continue;
                const CBox RB = FADE->renderBox();
                if (!RB.containsPoint(REALPX.middle()))
                    continue;
                tile.fb       = FB;
                tile.fbBox    = RB;     // where the snapshot sits, monitor px
                tile.srcBox   = REALPX; // where the window sits inside it
                tile.captured = true;
                tile.start    = now;    // the animation starts at the unmap, like Hyprland's
                break;
            }
            continue;
        }

        // The animation clock: the speed setting stretches it, 0 snaps it.
        const float ELAPSED = Hyprdrive::Motion::Clock::animMsBetween(now, tile.start);
        const float T       = DURMS <= 0.F ? 1.F : std::clamp(ELAPSED / DURMS, 0.F, 1.F);
        if (tile.cardIdx != cardIdx || T >= 1.F || !tile.fb || !tile.fb->isAllocated())
            continue;
        const auto TEX = tile.fb->getTexture();
        if (!TEX || tile.srcBox.width < 1.F || tile.srcBox.height < 1.F)
            continue;


        // popin 87%: the tile shrinks about its centre, linearly, alpha out.
        const float SCALE = 1.F - ((1.F - POPIN) * T);
        const CBox  TILE{tile.box.x + (tile.box.width * (1.F - SCALE) / 2.F), tile.box.y + (tile.box.height * (1.F - SCALE) / 2.F), tile.box.width * SCALE,
                         tile.box.height * SCALE};

        // The whole snapshot, scaled so the window's region of it covers TILE.
        const double K = TILE.width / tile.srcBox.width;
        const CBox   DRAW{TILE.x - ((tile.srcBox.x - tile.fbBox.x) * K), TILE.y - ((tile.srcBox.y - tile.fbBox.y) * K), tile.fbBox.width * K, tile.fbBox.height * K};

        CRegion damageRegion{TILE};
        g_pHyprRenderer->draw(
            CTexPassElement::SRenderData{
                .tex     = TEX,
                .box     = DRAW,
                .a       = 1.F - T,
                .damage  = damageRegion,
                .clipBox = TILE,
            },
            damageRegion);
    }

    std::erase_if(closingTiles, [&](const SClosingTile& t) {
        // The wait for an unmap is a timeout (real time); the shrink is an
        // animation (the animation clock, as it is drawn above).
        const float SINCE = t.captured ? Hyprdrive::Motion::Clock::animMsBetween(now, t.start) : Hyprdrive::Motion::msBetween(now, t.start);
        if (!t.captured) {
            const auto W = t.window.lock();
            // Refused close (still mapped past the wait), or unmapped with no
            // snapshot to borrow: let it go without an animation.
            return (W && W->m_isMapped) ? SINCE >= WAITMS : !W || SINCE >= WAITMS;
        }
        return SINCE >= DURMS || !t.fb || !t.fb->isAllocated();
    });

    // The pass does not schedule itself, and must not ask from inside: while
    // anything is animating OR still waiting on its unmap (closingTiles is not
    // empty), onPreRender keeps the reflow pacer running for it.
}

float COverview::workspaceScrollingSpanRatio(const PHLWORKSPACE& workspace, PHLMONITOR monitor) const {
    if (!workspace || !monitor || monitor->m_size.x <= 0.F || monitor->m_size.y <= 0.F)
        return 1.0F;

    // Once per workspace per drawn frame. stripRow asks for every image and
    // stripMiniBox asks again for every mini, and each answer walks that
    // workspace's windows, casts the layout algorithm and searches `images`.
    // Keyed on the OUTPUT's frame counter, so the answer a hit test between
    // frames gets is the one the minis were last drawn at.
    const uint64_t FRAME = overviewFrameOn(monitor);
    if (spanRatioFrame != FRAME || spanRatioMonitor != monitor->m_id) {
        spanRatioMemo.clear();
        spanRatioFrame   = FRAME;
        spanRatioMonitor = monitor->m_id;
    } else if (const auto IT = spanRatioMemo.find(workspace->m_id); IT != spanRatioMemo.end())
        return IT->second;

    const float VALUE = workspaceScrollingSpanRatioLive(workspace, monitor);
    spanRatioMemo[workspace->m_id] = VALUE;
    return VALUE;
}

float COverview::workspaceScrollingSpanRatioLive(const PHLWORKSPACE& workspace, PHLMONITOR monitor) const {
    if (!isWorkspaceScrolling(workspace))
        return 1.0F;

    const auto ALGO = overviewScrollingAlgorithmForWorkspace(workspace);
    if (!ALGO || !ALGO->m_scrollingData || !ALGO->m_scrollingData->controller)
        return 1.0F;

    if (!ALGO->m_scrollingData->controller->isPrimaryHorizontal())
        return 1.0F;

    const auto USABLE = ALGO->usableArea();
    const auto CAMERA_TRANS = ALGO->m_scrollingData->controller->getCameraTranslation(USABLE);

    const SWorkspaceImage* wimgPtr = nullptr;
    for (const auto& img : images) {
        if (img && img->pWorkspace == workspace) {
            wimgPtr = img.get();
            break;
        }
    }

    float minX = sc<float>(monitor->m_position.x) + sc<float>(monitor->m_size.x);
    float maxX = sc<float>(monitor->m_position.x);
    size_t tiledCount = 0;

    if (wimgPtr && !wimgPtr->windows.empty()) {
        for (const auto& winRef : wimgPtr->windows) {
            const auto WINDOW = getOverviewWindowToShow(winRef.lock());
            if (!WINDOW || !overviewStripMiniShown(WINDOW, workspace) || WINDOW->m_isFloating)
                continue;

            const auto GEOM = WINDOW->geometricBox(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
            const float LEFT = sc<float>(GEOM.x) - sc<float>(CAMERA_TRANS.x);
            const float RIGHT = LEFT + sc<float>(GEOM.w);

            minX = std::min(minX, LEFT);
            maxX = std::max(maxX, RIGHT);
            tiledCount++;
        }
    } else {
        for (const auto& w : Desktop::windowState()->windows()) {
            const auto WINDOW = getOverviewWindowToShow(w);
            if (!WINDOW || WINDOW->m_workspace != workspace || !overviewStripMiniShown(WINDOW, workspace) || WINDOW->m_isFloating)
                continue;

            const auto GEOM = WINDOW->geometricBox(Desktop::View::IGeometric::GEOMETRIC_CURRENT);
            const float LEFT = sc<float>(GEOM.x) - sc<float>(CAMERA_TRANS.x);
            const float RIGHT = LEFT + sc<float>(GEOM.w);

            minX = std::min(minX, LEFT);
            maxX = std::max(maxX, RIGHT);
            tiledCount++;
        }
    }

    if (tiledCount == 0)
        return 1.0F;

    float virtualRight = maxX;
    const double MAXW = ALGO->m_scrollingData->maxWidth();
    if (MAXW > 0.0) {
        const float TAPE_RIGHT = sc<float>(USABLE.x) + sc<float>(MAXW);
        virtualRight = std::max(virtualRight, TAPE_RIGHT);
    }

    const float LEFT_MARGIN = std::max(0.F, minX - sc<float>(monitor->m_position.x));
    const float TOTAL_VIRTUAL_WIDTH = (virtualRight - sc<float>(monitor->m_position.x)) + LEFT_MARGIN;

    if (TOTAL_VIRTUAL_WIDTH <= sc<float>(monitor->m_size.x))
        return 1.0F;

    return TOTAL_VIRTUAL_WIDTH / sc<float>(monitor->m_size.x);
}

void COverview::renderDraggedWindow(PHLMONITOR monitor, size_t activeIdx, float workspacePitch, float renderScale, const Time::steady_tp& now) {
    auto*      dragOwner = g_pointerGrabOverview && g_pointerGrabOverview->drag.activeWindow ? g_pointerGrabOverview : this;
    const auto WINDOW    = getOverviewWindowToShow(dragOwner->drag.activeWindow.lock());
    if (!shouldShowOverviewWindow(WINDOW) || !WINDOW->m_workspace)
        return;

    const auto GLOBALBOX  = dragOwner->draggedWindowGlobalBox();
    const auto MONITORBOX = monitor ? monitor->logicalBox() : CBox{};
    if (GLOBALBOX.empty() || MONITORBOX.empty() || GLOBALBOX.intersection(MONITORBOX).empty())
        return;

    // ROUNDED, for X73's reason: the box follows the cursor and is scaled, so it
    // lands on fractional coordinates, and a 1px border drawn in the annulus
    // outside a fractional box keeps only its top and left edges. Measured on
    // the drag path exactly as on the hover path -- one ring row, one ring
    // column -- once the dragged window started drawing a ring at all.
    auto windowBox = CBox{
        (GLOBALBOX.pos() - monitor->m_position) * monitor->m_scale,
        GLOBALBOX.size() * monitor->m_scale,
    };
    windowBox.round();

    // MOTION BLUR WHILE IT RESIZES (user, 2026-09-30: the proxy shrinking into
    // a strip cell and growing back out). Only while its SIZE is changing --
    // a proxy that merely follows the pointer stays sharp -- and then with the
    // layout blur's samples (FlightBlur.hpp). Drawn into a nested pass, so it
    // must not flush the real one.
    const CBox PREV  = dragBlurWindow.lock() == WINDOW ? dragBlurPrev : CBox{};
    dragBlurPrev     = windowBox;
    dragBlurWindow   = WINDOW;
    // Over 2px: windowBox is rounded (x and w apart), so a proxy that only
    // follows the pointer wobbles a pixel in size -- and every wobble was one
    // frame through the blurred path (user's recording, 2026-10-10).
    const bool GROWS = PREV.w >= 1 && (std::abs(PREV.w - windowBox.w) > 2.0 || std::abs(PREV.h - windowBox.h) > 2.0);
    if (GROWS &&
        Hyprdrive::FlightBlur::drawBlurred(monitor, WINDOW, Hyprdrive::FlightBlur::shape(Hyprdrive::FlightBlur::ESite::DRAG, PREV, windowBox, 0.F), windowBox,
                                                [&] { renderWindowLive(monitor, WINDOW, windowBox, dragOwner->scale->value(), now, nullptr, true, 1.F, false); },
                                                Hyprdrive::FlightBlur::layoutSamples()))
        return;

    renderWindowLive(monitor, WINDOW, windowBox, dragOwner->scale->value(), now, nullptr, true);
}

bool COverview::hasVisiblePrecomputedBlurWindow(PHLMONITOR monitor, size_t activeIdx, float workspacePitch, float renderScale) const {
    if (!monitor)
        return false;

    const auto DRAGGEDWINDOW = getOverviewWindowToShow(drag.activeWindow.lock());

    for (size_t workspaceIdx = 0; workspaceIdx < images.size(); ++workspaceIdx) {
        const auto& workspaceImage = images[workspaceIdx];
        if (!workspaceImage || !workspaceImage->pWorkspace)
            continue;

        const auto WORKSPACEOFFSET = workspaceOverviewOffset(workspaceIdx, activeIdx, workspacePitch);
        const auto WORKSPACEBOX     = getOverviewWorkspaceBox(monitor, renderScale, viewOffset->value(), WORKSPACEOFFSET, layout);
        const auto VISIBLEBOX       = workspaceOverviewVisibleBox(workspaceIdx, WORKSPACEBOX, renderScale, monitor);
        // B2: with a query typed, an off-screen workspace can still own a
        // promoted tile drawn on the active card, so it must not be culled here.
        // ...and after the query is EMPTIED a releasing tile is still crossing
        // the active card, so it must keep its frost the whole way: section C
        // outranks this cull.
        const bool HASPROMOTED = std::ranges::any_of(workspaceImage->windows, [&](const auto& ref) { return overviewGridPromoted(getOverviewWindowToShow(ref.lock()), monitor); });
        if (!overviewBoxIntersectsMonitor(VISIBLEBOX, monitor) && g_search.query.text.empty() && !HASPROMOTED)
            continue;

        const auto workspace = workspaceImage->pWorkspace;

        const auto isVisiblePrecomputedBlurWindow = [&](const PHLWINDOW& window) {
            if (window == DRAGGEDWINDOW || !OverviewWindow::shouldUseBlurFramebuffer(window))
                return false;

            const auto windowBox = getOverviewWindowBox(window, monitor, renderScale, viewOffset->value(), WORKSPACEOFFSET, layout);
            return overviewBoxIntersectsMonitor(windowBox, monitor);
        };

        {
            const auto fullscreenWindow = overviewFullscreenPathWindow(workspace);
            if (fullscreenWindow) {
                if (isVisiblePrecomputedBlurWindow(fullscreenWindow))
                    return true;

                continue;
            }
        }

        for (const auto& windowRef : workspaceImage->windows) {
            if (isVisiblePrecomputedBlurWindow(getOverviewWindowToShow(windowRef.lock())))
                return true;
        }
    }

    return false;
}

void COverview::renderPinnedFloatingWindows(PHLMONITOR monitor, float overviewScale, const Time::steady_tp& now) {
    if (!monitor)
        return;

    const auto TARGETOVERVIEWSCALE = Hyprdrive::Config::getScale();
    const auto ANIMATIONPROGRESS   = (1.F - TARGETOVERVIEWSCALE) > 0.001F ? (1.F - overviewScale) / (1.F - TARGETOVERVIEWSCALE) : 1.F;

    for (const auto& windowRef : pinnedFloatingWindows) {
        const auto window = getOverviewWindowToShow(windowRef.lock());
        if (!shouldShowPinnedFloatingOverviewWindow(window))
            continue;
        if (drag.activeWindow && window == getOverviewWindowToShow(drag.activeWindow.lock()))
            continue;

        if (window->m_monitor != monitor)
            continue;

        float renderScale = 1.F;
        CBox  windowBox   = getPinnedFloatingOverviewWindowBox(monitor, window, TARGETOVERVIEWSCALE, ANIMATIONPROGRESS, &renderScale);

        if (!overviewBoxIntersectsMonitor(windowBox, monitor))
            continue;

        renderWindowLive(monitor, window, windowBox, renderScale, now);
    }
}

void COverview::renderWindowLive(PHLMONITOR monitor, PHLWINDOW window, const CBox& windowBox, float renderScale, const Time::steady_tp& now, const CBox* workspaceBox,
                                       bool dragged, float alphaMul, bool ownsPass) {
    if (!window)
        return;

    forced.forceWindow(window);
    forced.forceWindowSurfaces(window);

    // WAKING A CLIENT IS NOT FREE, AND OPEN IS THE WORST MOMENT TO DO IT (user,
    // 2026-09-19: "the new open makes the animation stutter because it has to
    // grab all those window frames").
    //
    // GNOME never pays this: a Mutter window actor already holds the last
    // committed buffer as a texture, so its overview scales textures it had and
    // asks no client for anything. We draw windows live, so opening used to say
    // "wake up" to every client at once -- un-suspend, wl_surface.enter, frame
    // callbacks -- and they all decoded and repainted during the 300-400ms the
    // open animation needed for itself.
    //
    // We hold the last buffer too, and that is what the animation draws anyway,
    // so nothing is lost by waiting: while the overview is still animating,
    // windows are drawn from what they last committed, exactly as Mutter would.
    // Once it lands, wakes are spread a few per frame rather than all at once,
    // so the cost is a couple of frames of ramp instead of one stalled one.
    // THE RENDER PASS RECORDS; IT DOES NOT WAKE (user, 2026-09-20: separate the
    // overview animation from the live previews "so they don't conflict").
    //
    // Waking a client -- wl_surface.enter, then asking for its next frame -- used
    // to happen right here, inside the pass that is compositing. That is the same
    // shape of mistake the search field's letter tick had: work that belongs to
    // the event loop, done mid-composite, where a scheduleFrame is liable to be
    // absorbed by the frame already in flight and where it competes with the
    // animation for the one thread Hyprland composites on.
    //
    // Note this is NOT parallelism -- there is no second thread to move to. It is
    // the same win as the letter tick: the render pass does one job, and the
    // wake happens between frames where it can be paced and where the frame it
    // asks for is the NEXT one rather than the one being drawn.
    //
    // Membership here is also what the frame gate consults, so "we drew it" and
    // "it may have frames" stop being two questions answered by two different
    // pieces of geometry.
    if (drawnSet.insert(window.get()).second)
        drawnWindows.emplace_back(window);

    // IF WE DREW IT, WE ASK FOR ITS NEXT FRAME (user, 2026-09-19: after
    // switching desktop from the strip, the live window on the previous desktop
    // stops painting).
    //
    // The render walk and the frame-callback walk decide independently which
    // tiles exist, and they disagree: a promoted tile is drawn at the active
    // card's offset while its own workspace card is off screen, so
    // sendOverviewFrameCallbacks skipped the workspace and never reached the
    // window this pass is busy drawing. Measured across four samples with the
    // video on the previous desktop: forced[zen] climbing 691 -> 865 (drawn
    // every frame) while sent[zen] sat frozen at 387/151 (asked for nothing).
    //
    // Asking from here makes "drawn" and "gets frames" the same question by
    // construction rather than by two visibility tests agreeing. It is not a
    // second throttle either: this goes through CWLSurfaceResource::frame like
    // every other request, so shouldAllowSurfaceFrame -- and with it the whole
    // poll/stream gate -- still decides whether the callback is delivered.
    // (the frame request that used to live here now runs from wakeDrawnWindows)
    // E8-f: renderWindow multiplies the texture by the WORKSPACE alpha, so a
    // workspace sitting at 0 draws this window's border and nothing else. The
    // window's own alpha is already forced per frame right above; forcing the
    // workspace only on rebuild is what let a later zeroing survive until the
    // overview was reopened -- which was the user's workaround, not a fix.
    forceWorkspaceAlphaVisible(window->m_workspace);

    // alphaMul is the strip's fade (and the neighbour desktops' fade on open).
    // It rides SRenderParams::alpha, the flight fade's lever (Y5), so the
    // shadow, the border and every decoration fade WITH the content (user,
    // 2026-11-03: the frames and shadows still popped in). It used to warp the
    // window's own alpha, which only the surface reads -- the chrome stayed
    // solid around a window that was still invisible.

    OverviewWindow::renderOverviewWindow({
        .monitor              = monitor,
        .window               = window,
        .windowBox            = windowBox,
        .renderScale          = renderScale,
        .now                  = now,
        .workspaceBox         = workspaceBox,
        // The ring has three owners, and DRAGGED has to be one of them in its
        // own right. It used to ride the keyboard clause by accident -- a drag
        // leaves closeOnWindow on the window it picked up -- and X74 gating that
        // clause behind g_search.nav.keyboardTile took the ring off the dragged
        // window with it, because a drag is pointer input and clears the flag.
        //
        // It cannot come from hovered() either: updateTileHover drops the hover
        // outright while a drag is live, so the tile under the cursor is not
        // reported as hovered and the window being carried is not under the
        // cursor anyway once it has left its slot.
        // The keyboard's ring only on the monitor that HAS the keyboard: every
        // monitor keeps its own closeOnWindow, and with two they both drew one.
        .selected             = (g_search.nav.keyboardTile && g_search.nav.zone == EOverviewNavZone::Tile && closeOnWindow == window && activeScrollOverview().get() == this) ||
            TileChrome::hovered() == window || dragged,
        .dragged              = dragged,
        .alpha                = std::clamp(alphaMul, 0.F, 1.F),
        .ownsRenderPass       = ownsPass,
    });
}

void COverview::refreshWindows() {
    redrawAll();
}

// P6i2 INSTRUMENT. See Membership::renderShadowEnabled. Runs at the end of
// onPreRender -- after the rebuild decision, so it grades the list the frame
// about to be drawn will actually walk, not the one before it.
void COverview::renderListShadow() {
    if (!Membership::renderShadowEnabled())
        return;

    std::unordered_map<const void*, WORKSPACEID> seenOnCard;

    for (const auto& img : images) {
        if (!img || !img->pWorkspace)
            continue;

        const auto CARDID = img->pWorkspace->m_id;

        std::vector<PHLWINDOW> snapshot;
        for (const auto& ref : img->windows) {
            const auto WINDOW = getOverviewWindowToShow(ref.lock());
            if (!WINDOW || !shouldShowOverviewWindow(WINDOW))
                continue;

            const auto LIVEWS = WINDOW->m_workspace ? WINDOW->m_workspace->m_id : WORKSPACE_INVALID;

            if (std::ranges::find(snapshot, WINDOW) != snapshot.end()) {
                Membership::renderDivergence("DUP", CARDID, LIVEWS, WINDOW->m_class, WINDOW->m_title);
                continue;
            }
            snapshot.emplace_back(WINDOW);

            if (const auto IT = seenOnCard.find(WINDOW.get()); IT != seenOnCard.end())
                Membership::renderDivergence("CROSS", CARDID, IT->second, WINDOW->m_class, WINDOW->m_title);
            else
                seenOnCard.emplace(WINDOW.get(), CARDID);
        }

        const auto LIVE = overviewGridWindows(img->pWorkspace);

        for (const auto& WINDOW : snapshot) {
            if (std::ranges::find(LIVE, WINDOW) == LIVE.end())
                Membership::renderDivergence("STALE", CARDID, WINDOW->m_workspace ? WINDOW->m_workspace->m_id : WORKSPACE_INVALID, WINDOW->m_class, WINDOW->m_title);
        }

        for (const auto& WINDOW : LIVE) {
            if (std::ranges::find(snapshot, WINDOW) == snapshot.end())
                Membership::renderDivergence("MISSING", CARDID, WINDOW->m_workspace ? WINDOW->m_workspace->m_id : WORKSPACE_INVALID, WINDOW->m_class, WINDOW->m_title);
        }
    }

    Membership::renderPass();
}

void COverview::redrawAll(bool forcelowres) {
    lastBuiltQuery = g_search.query.text;
    rebuildWorkspaceImages();
    seedRememberedSelections();

    for (const auto& img : images) {
        img->windows.clear();
        img->overflowLeft   = 0.F;
        img->overflowRight  = 0.F;
        img->overflowTop    = 0.F;
        img->overflowBottom = 0.F;
    }
    pinnedFloatingWindows.clear();

    std::unordered_map<WORKSPACEID, SP<SWorkspaceImage>> imagesByWorkspace;
    imagesByWorkspace.reserve(images.size());

    for (const auto& img : images) {
        if (img && img->pWorkspace)
            imagesByWorkspace.emplace(img->pWorkspace->m_id, img);
    }

    std::vector<PHLWINDOW> addedWindows;
    addedWindows.reserve(Desktop::windowState()->windows().size());

    std::vector<PHLWINDOW> addedPinnedFloatingWindows;
    addedPinnedFloatingWindows.reserve(Desktop::windowState()->windows().size());

    // X58(a)'s remaining half needs the ACTIVE card by id, not by pointer: the
    // fallback below hands a homeless promoted window to it.
    const auto MONITOR  = pMonitor.lock();
    const auto ACTIVEWS = MONITOR ? MONITOR->m_activeWorkspace : PHLWORKSPACE{};
    const auto ACTIVEID = ACTIVEWS ? ACTIVEWS->m_id : WORKSPACE_INVALID;

    const auto addOverviewWindow = [&](const PHLWINDOW& window) {
        const auto overviewWindow = getOverviewWindowToShow(window);
        if (!shouldShowOverviewWindow(overviewWindow) || !overviewWindow->m_workspace)
            return;

        if (std::ranges::find(addedWindows, overviewWindow) != addedWindows.end())
            return;

        // Y9, and THIS is the site that mattered. `aae900b` taught the grid
        // LAYOUT that a minimized window belongs to the desktop it came from, so
        // it got a slot -- but the render walks `workspaceImage->windows`, and
        // membership of that list is decided here by workspace id. A stashed
        // window's m_workspace is the stash, the stash owns no card, this lookup
        // missed, and the window was never added to anything and so never drawn.
        // One lookup, not the twenty scattered `m_workspace ==` comparisons a
        // first reading suggested.
        WORKSPACEID homeId = Hyprdrive::Minimize::originalWorkspace(overviewWindow);
        if (homeId == WORKSPACE_INVALID)
            homeId = overviewWindow->m_workspace->m_id;

        auto imageIt = imagesByWorkspace.find(homeId);

        // X58(a), the half Y9 could not reach: THE HOME DESKTOP CAN BE GONE.
        // Minimizing the LAST window of a desktop lets Hyprland reap it -- the
        // same reap Minimize::restore already re-creates around -- so the home
        // id above names a desktop with no card, no image, and no place in a
        // render list that is keyed by card. The window was dropped here, and a
        // query could never bring it back: X58(a)'s "into the grid like any
        // other match" failing on exactly the windows Y9 made findable.
        //
        // Measured on b05b72c (zrig/v1/probeA): the SAME stashed window, same
        // query, viewed from desktop 3 -- with a second window keeping desktop 5
        // alive it promotes at 310720 px; with desktop 5 reaped it is 0 px. And
        // the grid had already solved for it either way, because
        // overviewMainGridWindows scans every window rather than a card's list:
        // the survivor in the reaped run sits a half-tile left of centre with an
        // empty slot beside it. So this was never a membership question -- the
        // layout said the tile was there and only the DRAWING disagreed.
        //
        // The active card is where a promoted tile is laid out (overviewGridSlot)
        // and drawn (its per-card offset is suppressed), so it is the honest
        // owner for one that has no card of its own. Scoped to promotable: with
        // an empty query shouldShowOverviewWindow already dropped the window
        // above, so Y9's "put away stays put away" is untouched, and a window
        // whose home card still exists never reaches this line.
        if (imageIt == imagesByWorkspace.end() && ACTIVEID != WORKSPACE_INVALID && overviewGridPromotable(overviewWindow, MONITOR))
            imageIt = imagesByWorkspace.find(ACTIVEID);

        if (imageIt == imagesByWorkspace.end())
            return;

        addedWindows.emplace_back(overviewWindow);
        imageIt->second->windows.emplace_back(overviewWindow);
    };

    const auto addPinnedFloatingWindow = [&](const PHLWINDOW& window) {
        const auto overviewWindow = getOverviewWindowToShow(window);
        if (!shouldShowPinnedFloatingOverviewWindow(overviewWindow))
            return;

        if (std::ranges::find(addedPinnedFloatingWindows, overviewWindow) != addedPinnedFloatingWindows.end())
            return;

        addedPinnedFloatingWindows.emplace_back(overviewWindow);
        pinnedFloatingWindows.emplace_back(overviewWindow);
    };

    for (const auto& window : Desktop::windowState()->windows()) {
        if (getOverviewWindowToShow(window) != window)
            continue;

        addOverviewWindow(window);
        addPinnedFloatingWindow(window);
    }

    for (const auto& window : Desktop::windowState()->windows()) {
        if (getOverviewWindowToShow(window) == window)
            continue;

        addOverviewWindow(window);
        addPinnedFloatingWindow(window);
    }

    updateWorkspaceOverflow();
}
