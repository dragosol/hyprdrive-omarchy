// Dock stacks: drawing the open popup (render) and its scrollbar arc. Split out of
// DockStacks.cpp; the state is still there (StackParts.hpp).
#include "StackParts.hpp"
#include "MotionCurves.hpp"

using namespace StacksDetail;

namespace Hyprdrive::Stacks {

    // ---------------------------------------------------------------- paint --
    // THE FAN'S SCROLLBAR (user, 2026-10-08): a thin arc beside the icon
    // column that follows the fan's own curve. A rect pass element cannot bend,
    // and a chain of turned segments shows its joints, so the arc is stroked by
    // cairo -- antialiased, round or square ends after the theme's corners --
    // into one texture, re-rasterised only when its shape or its thumb moves.
    // `pts`: the arc's centreline, render px, bottom first. The thumb covers
    // [t0, t1] of its length.
    static void drawArcBar(const std::vector<Vector2D>& pts, float t0, float t1, float thick, const CHyprColor& track, const CHyprColor& thumb, bool roundEnds, float alpha) {
        if (pts.size() < 2 || alpha <= 0.01F)
            return;
        double X0 = 1e9, Y0 = 1e9, X1 = -1e9, Y1 = -1e9;
        for (const auto& P : pts) {
            X0 = std::min(X0, P.x), Y0 = std::min(Y0, P.y), X1 = std::max(X1, P.x), Y1 = std::max(Y1, P.y);
        }
        const double PAD = std::ceil(thick) + 2.0;
        const CBox   BOX = CBox{std::floor(X0 - PAD), std::floor(Y0 - PAD), std::ceil(X1 - X0 + 2 * PAD) + 1, std::ceil(Y1 - Y0 + 2 * PAD) + 1};
        if (!drawableBox(BOX))
            return;

        size_t H = std::hash<std::string>{}(std::format("{:.0f},{:.0f}|{:.3f},{:.3f}|{:.2f}|{}|{:.3f},{:.3f},{:.3f},{:.3f}|{:.3f},{:.3f},{:.3f},{:.3f}", BOX.x, BOX.y, t0, t1, thick,
                                                           roundEnds, track.r, track.g, track.b, track.a, thumb.r, thumb.g, thumb.b, thumb.a));
        for (const auto& P : pts)
            H ^= std::hash<long long>{}(sc<long long>(std::llround(P.x * 4.0)) * 1000003LL + std::llround(P.y * 4.0)) + 0x9e3779b97f4a7c15ULL + (H << 6) + (H >> 2);
        const auto KEY = std::to_string(H);

        if (KEY != g_arcBar.key || !g_arcBar.tex) {
            const int W = sc<int>(BOX.width), HT = sc<int>(BOX.height);
            auto*     SURF = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W, HT);
            auto*     CR   = cairo_create(SURF);
            cairo_set_antialias(CR, CAIRO_ANTIALIAS_BEST);
            cairo_translate(CR, -BOX.x, -BOX.y);
            cairo_set_line_width(CR, thick);
            cairo_set_line_cap(CR, roundEnds ? CAIRO_LINE_CAP_ROUND : CAIRO_LINE_CAP_BUTT);
            cairo_set_line_join(CR, CAIRO_LINE_JOIN_ROUND);

            std::vector<double> len(pts.size(), 0.0);
            for (size_t i = 1; i < pts.size(); ++i)
                len[i] = len[i - 1] + pts[i].distance(pts[i - 1]);
            const double TOTAL = std::max(1.0, len.back());
            const auto   at    = [&](double l) {
                for (size_t i = 1; i < pts.size(); ++i)
                    if (len[i] >= l) {
                        const double F = (l - len[i - 1]) / std::max(1e-6, len[i] - len[i - 1]);
                        return pts[i - 1] + (pts[i] - pts[i - 1]) * F;
                    }
                return pts.back();
            };
            const auto strokeSpan = [&](double l0, double l1, const CHyprColor& c) {
                if (l1 - l0 < 0.5 || c.a <= 0.0)
                    return;
                const auto A = at(l0);
                cairo_move_to(CR, A.x, A.y);
                for (size_t i = 1; i + 1 < pts.size(); ++i)
                    if (len[i] > l0 && len[i] < l1)
                        cairo_line_to(CR, pts[i].x, pts[i].y);
                const auto B = at(l1);
                cairo_line_to(CR, B.x, B.y);
                cairo_set_source_rgba(CR, c.r, c.g, c.b, c.a);
                cairo_stroke(CR);
            };
            // Round ends reach half a stroke past each end: inset by that so
            // the bar stays inside its arc, and the thumb inside the track.
            const double CAP = roundEnds ? thick / 2.0 : 0.0;
            strokeSpan(CAP, TOTAL - CAP, track);
            const double MINL = std::min(TOTAL - 2 * CAP, thick * 4.0);
            double       l0   = CAP + (TOTAL - 2 * CAP) * std::clamp<double>(t0, 0.0, 1.0);
            double       l1   = CAP + (TOTAL - 2 * CAP) * std::clamp<double>(t1, 0.0, 1.0);
            if (l1 - l0 < MINL) {
                const double MID = std::clamp((l0 + l1) / 2.0, CAP + MINL / 2.0, TOTAL - CAP - MINL / 2.0);
                l0 = MID - MINL / 2.0, l1 = MID + MINL / 2.0;
            }
            strokeSpan(l0, l1, thumb);

            cairo_surface_flush(SURF);
            g_arcBar.tex = makeShared<Render::GL::CGLTexture>(DRM_FORMAT_ARGB8888, cairo_image_surface_get_data(SURF), sc<uint32_t>(cairo_image_surface_get_stride(SURF)),
                                                             Vector2D{sc<double>(W), sc<double>(HT)});
            cairo_destroy(CR);
            cairo_surface_destroy(SURF);
            g_arcBar.key = KEY;
            g_arcBar.box = BOX;
        }

        CTexPassElement::SRenderData d;
        d.tex = g_arcBar.tex;
        d.box = g_arcBar.box;
        d.a   = alpha;
        g_pHyprRenderer->addPassElement(makeUnique<CTexPassElement>(d));
    }

    void render(PHLMONITOR monitor, const Dock::SLayout& layout) {
        retireHops();

        if (!g_popup || !monitor || g_popup->monitor != monitor->m_id)
            return;

        // The popup is anchored to a CELL, and the layout reflows underneath it
        // (an app opens, the dock slides). Re-read the anchor every frame from
        // the item that still owns this path, exactly as the reference's 16 ms
        // dock-follow timer does -- and if the cell is gone, so is the popup.
        bool found = false;
        for (const auto& ITEM : layout.items) {
            if (!ITEM.stack || canonical(ITEM.stackPath) != g_popup->path)
                continue;
            g_popup->anchor = ITEM.iconBox;
            found           = true;
            break;
        }
        if (!found || layout.pill.empty()) {
            g_popup.reset();
            g_itemDrag.reset();
            return;
        }

        tick(*g_popup);
        const float PROGRESS = popupProgress(*g_popup);
        if (g_popup->leaving && PROGRESS <= 0.F) {
            traceEvent("fully closed (popup gone)");
            g_popup.reset();
            g_itemDrag.reset();
            return;
        }

        g_popup->lastDrawn = Time::steadyNow();
        const float SCALE = std::max(monitor->m_scale, 0.01F);
        const float ALPHA = Motion::houseFixed(true, PROGRESS); // the same curve both ways, so a reversal is seamless

        if (g_popup->grid)
            layoutPane(*g_popup, monitor, layout.pill, PROGRESS);
        else
            layoutFan(*g_popup, monitor, layout.pill, PROGRESS);

        // FROST. User law: every new surface, by default -- and through the
        // dock's own plate call, so the popup re-themes with the rest of the
        // dock rather than beside it. The 2 px floor is the popup's own: a
        // plate that thin is the entrance's first frame, not a surface.
        // READABLE WITHOUT FROST (dock stacks A). With Hyprland's blur off the
        // plate is only the theme tint at the window opacity (~0.72), and a pane
        // of file names over a terminal of the same colour reads as two layers
        // of text. The dock pill holds icons and is fine; these hold words. So
        // a second pass of the SAME theme tint brings them to ~0.95 -- still the
        // theme's colour, and with blur on the frost does this job instead.
        const bool BLUR       = Config::getValue<bool>("decoration:blur:enabled");
        const auto drawPlate = [&](const CBox& box, float radius, float alpha) {
            if (box.width < 2.0 || box.height < 2.0)
                return;
            Dock::frostedPlate(monitor, box, sc<int>(std::round(radius * SCALE)), alpha);
            if (!BLUR) {
                CRectPassElement::SRectData solid;
                solid.box           = box;
                solid.color         = Config::dockBackgroundColor().modifyA(0.8F * alpha);
                solid.round         = sc<int>(std::round(radius * SCALE));
                solid.roundingPower = 2.F;
                if (drawableBox(solid.box))
                    g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(solid));
            }
        };

        // Motion blur (StackBlur): while the fan's rows are sampled, what they
        // draw is collected rather than queued.
        std::vector<StackBlur::SSample>* sink    = nullptr;
        int                              sinkGroup = 0;
        CBox                             sinkBounds;
        const auto drawTex = [&](const SP<Render::ITexture>& tex, const CBox& box, float alpha, float rot = 0.F) {
            if (!tex || !tex->ok() || !drawableBox(box))
                return;
            if (sink) {
                CBox b = box;
                b.rot  = rot;
                sink->push_back(StackBlur::SSample{.tex = tex, .box = b, .alpha = alpha, .group = sinkGroup});
                // Bounds that hold the box however it is turned.
                const double R  = std::hypot(box.width, box.height) / 2.0 + 2.0;
                const double CX = box.x + box.width / 2.0, CY = box.y + box.height / 2.0;
                const CBox   B{CX - R, CY - R, 2 * R, 2 * R};
                if (sinkBounds.width < 1)
                    sinkBounds = B;
                else {
                    const double X0 = std::min(sinkBounds.x, B.x), Y0 = std::min(sinkBounds.y, B.y);
                    const double X1 = std::max(sinkBounds.x + sinkBounds.width, B.x + B.width), Y1 = std::max(sinkBounds.y + sinkBounds.height, B.y + B.height);
                    sinkBounds = CBox{X0, Y0, X1 - X0, Y1 - Y0};
                }
                return;
            }
            CTexPassElement::SRenderData d;
            d.tex     = tex;
            d.box     = box;
            d.box.rot = rot;
            d.a       = alpha;
            g_pHyprRenderer->addPassElement(makeUnique<CTexPassElement>(d));
        };

        // THE DRAG GHOST, and the threshold that promotes a press into a drag.
        // Both ride the pointer read this function already has to do; there is
        // no motion hook and no second input path -- see the note on SItemDrag.
        const Vector2D MOUSE = (g_pInputManager->getMouseCoordsInternal() - monitor->m_position) * monitor->m_scale;
        // The pointer moving takes the selection back from the keyboard.
        if (MOUSE.distance(g_popup->lastMouse) > 2.0) {
            g_popup->keyboard = false;
            g_popup->typing   = false;
        }
        g_popup->lastMouse = MOUSE;

        const auto drawGhost = [&] {
            if (!g_itemDrag || !g_itemDrag->live)
                return;
            const float SZ = GHOST_SIZE * SCALE;
            drawTex(OverviewRender::appIcon(themedIcon(g_itemDrag->icon), false, false, SZ), CBox{std::round(MOUSE.x - SZ / 2.0), std::round(MOUSE.y - SZ / 2.0), std::round(SZ), std::round(SZ)},
                    Dock::DRAG_GHOST_ALPHA);
        };

        if (g_popup->grid) {
            auto&      P     = *g_popup;
            const auto FG    = Config::themeForeground();
            const auto MUTED = Config::themeMutedText();
            const auto HEAD  = Config::themeHeading();
            drawPlate(P.plate, GRID_RADIUS, ALPHA);
            // The field's frost with the plate's: a backdrop blur has nothing to
            // smear, and inside the blurred pass it would frost an empty buffer.
            Dock::frostedPlate(monitor, P.fieldBox, sc<int>(std::round(8.F * SCALE)), ALPHA * 0.5F);

            // MOTION BLUR ON THE ENTRANCE AND EXIT (user, 2026-09-30): while
            // the pane grows out of its icon or back into it, everything on the
            // plate is drawn into a nested pass and smeared from last frame's
            // plate to this one (FlightBlur.hpp) -- the minimize flight's blur,
            // at dock:minimize_motion_blur samples. Settled, it draws directly.
            // The stack pane blur setting (FlightBlur::shape): an exit runs its progress
            // 1 -> 0, so its own clock is the complement.
            const CBox PREVPLATE = FlightBlur::shape(FlightBlur::ESite::STACK_PANE, P.lastPlate, P.plate, P.leaving ? 1.F - PROGRESS : PROGRESS);
            P.lastPlate          = P.plate;
            const bool MOVING    = (P.leaving || PROGRESS < 1.F) && PREVPLATE.width >= 2 && PREVPLATE.height >= 2;
            const auto drawBody  = [&] {
            // The dock's own outline (theme accent, Hyprland's border weight),
            // so the pane reads as part of the dock (user, 2026-09-24).
            {
                CBorderPassElement::SBorderData border;
                border.box           = P.plate.copy().round();
                border.grad1         = ::Config::CGradientValueData(Config::highlightColor().modifyA(ALPHA));
                border.borderSize    = std::max(1, Config::getValue<int>("general:border_size"));
                border.round         = sc<int>(std::round(GRID_RADIUS * SCALE));
                border.roundingPower = 2.F;
                if (drawableBox(border.box))
                    g_pHyprRenderer->addPassElement(makeUnique<CBorderPassElement>(border));
            }

            // HEADER: the stack's label, "12 of 340", and the sort on the right
            // (click it to change it).
            std::string label;
            for (const auto& ST : all())
                if (ST.path == P.path)
                    label = ST.label;
            static const char* SORTS[] = {"Name", "Date Added", "Date Modified", "Kind"};
            const std::string  COUNT   = std::to_string(P.visible.size()) + " of " + std::to_string(P.entries.size());
            const std::string  SORTL   = std::string(SORTS[sc<int>(P.sort)]) + "  ";
            float              hx      = sc<float>(P.headerBox.x);
            if (const auto T = OverviewRender::text(label, FG, sc<int>(std::round(13.F * SCALE)), false, "", sc<int>(P.headerBox.width * 0.5), 600); T && T->ok()) {
                drawTex(T, CBox{hx, P.headerBox.y + (P.headerBox.height - T->m_size.y) / 2.0, T->m_size.x, T->m_size.y}.round(), ALPHA);
                hx += sc<float>(T->m_size.x) + 10.F * SCALE;
            }
            if (const auto T = OverviewRender::text(COUNT, MUTED, sc<int>(std::round(11.F * SCALE)), false, "", 0, 500); T && T->ok())
                drawTex(T, CBox{hx, P.headerBox.y + (P.headerBox.height - T->m_size.y) / 2.0, T->m_size.x, T->m_size.y}.round(), ALPHA);
            if (const auto T = OverviewRender::text(SORTL, HEAD, sc<int>(std::round(11.F * SCALE)), false, "", 0, 600); T && T->ok()) {
                P.sortBox = CBox{P.headerBox.x + P.headerBox.width - T->m_size.x, P.headerBox.y + (P.headerBox.height - T->m_size.y) / 2.0, T->m_size.x, T->m_size.y}.round();
                drawTex(T, P.sortBox, ALPHA);
            }

            // THE FIELD: an inset plate (frosted above), the query or its
            // placeholder, a caret.
            const bool        EMPTYQ = P.query.empty();
            const std::string SHOWN  = EMPTYQ ? (sushiAvailable() ? std::string{"Search"} : std::string{"Search · install sushi for previews"}) : P.query;
            float             caretX = sc<float>(P.fieldBox.x) + 10.F * SCALE;
            if (const auto T = OverviewRender::text(SHOWN, EMPTYQ ? MUTED : FG, sc<int>(std::round(13.F * SCALE)), false, "", sc<int>(P.fieldBox.width - 20.F * SCALE), 500);
                T && T->ok()) {
                drawTex(T, CBox{caretX, P.fieldBox.y + (P.fieldBox.height - T->m_size.y) / 2.0, T->m_size.x, T->m_size.y}.round(), ALPHA);
                if (!EMPTYQ)
                    caretX += sc<float>(T->m_size.x) + 1.F;
            }
            CRectPassElement::SRectData caret;
            caret.box   = CBox{caretX, P.fieldBox.y + P.fieldBox.height * 0.25, std::max(1.0, 1.5 * SCALE), P.fieldBox.height * 0.5}.round();
            caret.color = Dock::accentColor().modifyA(ALPHA);
            if (drawableBox(caret.box))
                    g_pHyprRenderer->addPassElement(makeUnique<CRectPassElement>(caret));

            // THE CELLS, clipped to the grid: icon, two-line name, and the
            // previewed / selected / hovered highlight.
            const double GTOP = P.fieldBox.y + P.fieldBox.height;
            const CBox   CLIP{P.plate.x, GTOP, P.plate.width, std::max(0.0, P.plate.y + P.plate.height - GTOP)};
            for (const auto& R : P.rows) {
                const bool PREV  = R.vis >= 0 && R.vis == P.previewing;
                const bool SEL   = R.vis >= 0 && P.keyboard && R.vis == P.hover;
                const bool HOVER = !P.keyboard && rowHit(R, MOUSE);
                CBox       cell  = R.box;
                if (PREV) {
                    cell.x -= cell.width * 0.075;
                    cell.y -= cell.height * 0.075;
                    cell.width *= 1.15;
                    cell.height *= 1.15;
                }
                if (PREV || SEL || HOVER) {
                    CRectPassElement::SRectData hl;
                    hl.box     = cell;
                    hl.color   = Dock::accentColor().modifyA(ALPHA * (PREV ? 0.28F : SEL ? 0.2F : 0.12F));
                    hl.round   = sc<int>(std::round(10.F * SCALE));
                    hl.clipBox = CLIP;
                    if (drawableBox(hl.box))
                    g_pHyprRenderer->addPassElement(makeUnique<CRectPassElement>(hl));
                }
                const float ICON = GRID_ICON * SCALE * (PREV ? 1.15F : 1.F);
                CBox        ib{std::round(cell.x + (cell.width - ICON) / 2.0), std::round(cell.y + 8.F * SCALE), std::round(ICON), std::round(ICON)};
                const auto TEX = R.vis >= 0 && R.vis < sc<int>(P.visible.size()) ? iconTexture(P.entries[P.visible[sc<size_t>(R.vis)]], sc<int>(std::round(ICON))) :
                                                                                  OverviewRender::appIcon(themedIcon(R.icon), false, false, ICON);
                if (TEX && TEX->ok() && drawableBox(ib)) {
                    CTexPassElement::SRenderData d;
                    d.tex     = TEX;
                    d.box     = ib;
                    d.a       = ALPHA;
                    d.clipBox = CLIP;
                    g_pHyprRenderer->addPassElement(makeUnique<CTexPassElement>(d));
                }
                if (const auto T = OverviewRender::textWrapped(R.name, FG, sc<int>(std::round(11.F * SCALE)), sc<int>(cell.width - 8.F * SCALE), sc<int>(std::round(30.F * SCALE)),
                                                               false, "", 500);
                    T && T->ok() && drawableBox(cell)) {
                    CTexPassElement::SRenderData l;
                    l.tex     = T;
                    l.box     = CBox{cell.x + (cell.width - T->m_size.x) / 2.0, ib.y + ICON + 6.F * SCALE, T->m_size.x, T->m_size.y}.round();
                    l.a       = ALPHA;
                    l.clipBox = CLIP;
                    g_pHyprRenderer->addPassElement(makeUnique<CTexPassElement>(l));
                }
            }

            // STATES: loading, no matches, and the read cap.
            const char* STATE = P.loading && P.entries.empty() ? "Loading…" : (P.visible.empty() ? (P.query.empty() ? "Empty folder" : "No matches") : nullptr);
            if (STATE)
                if (const auto T = OverviewRender::text(STATE, MUTED, sc<int>(std::round(13.F * SCALE)), false, "", 0, 500); T && T->ok())
                    drawTex(T, CBox{CLIP.x + (CLIP.width - T->m_size.x) / 2.0, CLIP.y + (CLIP.height - T->m_size.y) / 2.0, T->m_size.x, T->m_size.y}.round(), ALPHA);
            if (P.truncated)
                if (const auto T = OverviewRender::text("showing first 5 000", MUTED, sc<int>(std::round(10.F * SCALE)), false, "", 0, 500); T && T->ok())
                    drawTex(T, CBox{P.plate.x + (P.plate.width - T->m_size.x) / 2.0, P.plate.y + P.plate.height - T->m_size.y - 4.F * SCALE, T->m_size.x, T->m_size.y}.round(),
                            ALPHA);
            };
            if (!MOVING || !FlightBlur::drawBlurred(monitor, nullptr, PREVPLATE, P.plate, drawBody))
                drawBody();

            drawGhost();
            return;
        }

        // THE FAN. One frosted plate per row: the rows are not a rectangle, so
        // a single enclosing plate would be a panel with a fan painted on it,
        // which is a different object. One blur region per row, and only while a
        // fan is actually up.
        // PILE -> FAN (dock stacks A): the items the cell's pile shows fly from
        // their pile slot (box and angle) to their row, on that row's own eased
        // progress -- and back onto the pile on close. The cell draws only its
        // outline meanwhile (popupShowsStack), so they visibly left it.
        const auto PILE = pileLayout(g_popup->path, g_popup->anchor);

        // The scrollbar's gutter and the filter field both sit right of the
        // icon column, clear of a hovered (blown-up), turned icon.
        const float ICONCLEAR = FAN_ICON * SCALE / 2.F * HOVER_SCALE * 1.3F + 6.F * SCALE;
        const float BARTHICK  = std::max(2.F, 1.5F * sc<float>(std::max(1, Config::getValue<int>("general:border_size")))) * SCALE;
        const float FANCX     = sc<float>(g_popup->anchor.x + g_popup->anchor.width / 2.0) + g_popup->edgeShift;
        const float FANBASE   = sc<float>(layout.pill.y) - FAN_GAP * SCALE;

        // THE PILE THE FAN SCROLLED INTO (user, 2026-10-08: "items in the fan
        // dropping neatly into the stack on the dock, and the reverse"). Rows
        // scrolled off the bottom are papers on the cell, the latest on top;
        // each new one pushes the rest a slot down, and the fourth one down
        // fades into the stack. When the fan folds they settle into the cell's
        // own pile -- the one it draws once the popup is gone -- so nothing
        // jumps at the hand-off.
        {
            auto&       P  = *g_popup;
            const float SP = P.visible.empty() ? 0.F : P.scrollPos;
            const float W  = 1.F - PROGRESS; // how far into the hand-off to the cell's own pile
            if (SP > 0.01F)
                P.scrolled = true;
            std::unordered_set<std::string> drawn;
            for (const auto& R : P.rows)
                if (!R.path.empty())
                    drawn.insert(R.path);
            const auto pileSlotOf = [&](const std::string& path) -> const SPileItem* {
                for (const auto& PI : PILE)
                    if (PI.entry.path == path)
                        return &PI;
                return nullptr;
            };
            const int PX = sc<int>(std::round(P.anchor.width));
            const int J1 = std::min(sc<int>(P.visible.size()) - 1, sc<int>(std::floor(SP - 1.F + 1e-4F)));
            for (int j = std::max(0, sc<int>(std::floor(SP)) - 5); j <= J1; ++j) {
                const float DEPTH = SP - sc<float>(j) - 1.F; // 0: the top slot
                if (DEPTH < 0.F || DEPTH >= 4.F)
                    continue;
                const auto& E   = P.entries[P.visible[sc<size_t>(j)]];
                float       rot = 0.F;
                CBox        box = pileSlot(P.anchor, DEPTH, rot);
                float       a   = std::clamp(4.F - DEPTH, 0.F, 1.F); // the fourth paper down fades into the stack
                if (const auto* HOME = pileSlotOf(E.path); HOME && W > 0.F) {
                    box = CBox{box.x + (HOME->box.x - box.x) * W, box.y + (HOME->box.y - box.y) * W, box.width + (HOME->box.width - box.width) * W,
                               box.height + (HOME->box.height - box.height) * W};
                    rot += (HOME->rot - rot) * W;
                    a += (1.F - a) * W;
                } else
                    a *= PROGRESS;
                drawn.insert(E.path);
                if (const auto TEX = iconTexture(E, PX, true))
                    drawTex(TEX, OverviewRender::padBox(box, TEX), a, rot);
            }
            // The cell's own papers that neither the fan nor the scrolled pile
            // holds fade back in as it folds.
            if (P.scrolled && W > 0.F)
                for (const auto& PI : PILE)
                    if (!drawn.contains(PI.entry.path))
                        if (const auto TEX = iconTexture(PI.entry, PX, true))
                            drawTex(TEX, OverviewRender::padBox(PI.box, TEX), W, PI.rot);
        }

        // THE SCROLLBAR (user, 2026-10-08): minimal, only while the fan moves,
        // fading after. A thin arc beside the icon column that bends with the
        // fan's own curve -- the same shift and step the rows sit on -- so it
        // reshapes itself with the fan: fewer slots, a shorter arc. The theme's
        // accent for the thumb, its foreground faintly for the track, its
        // border weight for the thickness, square ends on a square theme.
        if (g_popup->barAlpha > 0.F && fanMaxScroll(*g_popup) > 0.F) {
            const auto&  P  = *g_popup;
            const size_t SL = fanRows(P);
            const auto   centre = [&](float g) {
                return Vector2D{FANCX + FAN_SHIFT_K * std::pow(std::max(g, 0.F), FAN_SHIFT_P) * SCALE, FANBASE - FAN_STEP * SCALE * (g + 1.F) + FAN_ROW_H * SCALE / 2.F};
            };
            std::vector<Vector2D> pts;
            const float           G0 = -0.3F, G1 = sc<float>(SL) - 1.F + 0.3F;
            for (float g = G0; g <= G1 + 1e-3F; g += 0.1F) {
                const auto C  = centre(g);
                const auto D  = centre(g + 0.05F) - centre(g - 0.05F);
                const auto L  = std::max(1e-3, std::hypot(D.x, D.y));
                pts.push_back(C + Vector2D{-D.y / L, D.x / L} * ICONCLEAR); // the right-hand normal of an upward curve
            }
            const float  N    = sc<float>(P.visible.size());
            const float  E    = Hyprdrive::Motion::smoothstep(P.barAlpha);
            drawArcBar(pts, P.scrollPos / N, (P.scrollPos + sc<float>(SL)) / N, BARTHICK, Config::themeForeground().modifyA(0.10F), Config::highlightColor().modifyA(0.95F),
                       Config::themeRounding() > 0, ALPHA * E);
        }

        g_popup->hoverKey.clear(); // re-found below, from the row actually under the pointer
        const auto drawFanRows = [&](const std::vector<SRow>& ROWS) {
        for (size_t i = 0; i < ROWS.size(); ++i) {
            const auto& R   = ROWS[i];
            const float ROT = R.rot; // its tilt, or where it is in its spin
            const SPileItem* FROMPILE = nullptr;
            for (const auto& PI : PILE)
                if (!R.action && PI.entry.path == R.path)
                    FROMPILE = &PI;

            // Per-row alpha: a row that has not started its stagger is not
            // drawn at all rather than drawn at zero, which keeps its frost off
            // the queue too.
            const float E = R.e; // the row's own eased progress: 0 in the cell, 1 in place
            const float A = R.a; // its own opacity channel
            if (A <= 0.02F && !FROMPILE)
                continue;

            // The previewed row is enlarged 1.15x; it, the keyboard's selection
            // and the pointer's row take the accent on their label pill.
            const bool PREV  = !R.action && R.vis == g_popup->previewing;
            const bool SEL   = !R.action && g_popup->keyboard && R.vis == g_popup->hover;
            const bool HOVER = !g_popup->keyboard && rowHit(R, MOUSE); // the "N more" row too: it opens the folder (2026-09-25)
            // The hover blow-up: this row's eased 0..1 on the frame clock.
            const std::string ROWKEY = R.path.empty() ? R.name : R.path;
            if (HOVER) {
                if (g_popup->hoverKey != ROWKEY)
                    g_popup->hoverMoving = true; // a new target: keep the frames coming
                g_popup->hoverKey = ROWKEY;
                g_popup->hoverAmt.try_emplace(ROWKEY, 0.F);
            }
            float HB = 1.F;
            if (const auto IT = g_popup->hoverAmt.find(ROWKEY); IT != g_popup->hoverAmt.end()) {
                const float U = IT->second;
                HB            = 1.F + (HOVER_SCALE - 1.F) * (fanMotion().serp ? Hyprdrive::Motion::outCubicPow(U) : Hyprdrive::Motion::outQuad(U));
            }

            const float KT   = PREV ? 1.15F : 1.F; // the preview's enlargement (the size text is rasterised at)
            const float KS   = KT * HB;              // ... and the hover blow-up
            // The texture size (IconSize.hpp): the row at full hover blow-up,
            // fixed per row kind -- ICON itself animates with the hover and the
            // flight, and a size per frame would be a decode per frame. At rest
            // GL_LINEAR then minifies ~1.1x, hovered not at all.
            const int   FANPX = sc<int>(std::round(FAN_ICON * SCALE * KT * HOVER_SCALE));
            const float K    = KS * R.k;             // ... times the row's flight scale

            // THE ROW TURNS AS ONE PIECE about its icon's centre -- the pivot the
            // extension sets -- so the label rides the same arc as the icon
            // (2026-09-24: each part turning about its own centre slid the labels
            // off). No plate behind the row: like the extension's, a row is its
            // icon and its label on a pill of its own, and a pill is a texture,
            // so it can turn with the row where a frost region cannot.
            const float  ICON = FAN_ICON * SCALE * K;
            const CBox&  box  = R.box;
            const double PX   = box.x + box.width - FAN_PAD_X * SCALE * R.k - FAN_ICON * SCALE * R.k / 2.0;
            const double PY   = box.y + box.height / 2.0;
            const double CS = std::cos(ROT), SN = std::sin(ROT);
            // Where a part centred at (x, y) on the upright row lands once turned.
            const auto turned = [&](double x, double y, double w, double h) {
                const double DX = x - PX, DY = y - PY;
                return CBox{PX + DX * CS - DY * SN - w / 2.0, PY + DX * SN + DY * CS - h / 2.0, w, h};
            };
            CBox  ib    = turned(PX, PY, ICON, ICON).round();
            float ICONROT = ROT;
            // SCROLLED OFF THE BOTTOM: the icon flies on from the bottom slot
            // into the top of the cell's pile, turning to that paper's angle,
            // while its label fades where it was.
            const float DROPE = R.drop * R.drop * (3.F - 2.F * R.drop);
            if (DROPE > 0.F) {
                float      prot = 0.F;
                const CBox PT   = pileSlot(g_popup->anchor, 0.F, prot);
                ib              = CBox{ib.x + (PT.x - ib.x) * DROPE, ib.y + (PT.y - ib.y) * DROPE, ib.width + (PT.width - ib.width) * DROPE,
                          ib.height + (PT.height - ib.height) * DROPE};
                ICONROT         = ROT + (prot - ROT) * DROPE;
            }
            // OUT PAST THE TOP: the icon shrinks into the "N more in Files"
            // row's folder (and comes back out of it scrolling down), its label
            // fading where it was.
            float ICONA = A;
            if (R.top > 0.F && g_popup->moreIconK > 0.F) {
                const float  T  = R.top * R.top * (3.F - 2.F * R.top);
                const double SZ = FAN_ICON * SCALE * g_popup->moreIconK * 0.45;
                const double CXI = ib.x + ib.width / 2.0, CYI = ib.y + ib.height / 2.0;
                const double NX = CXI + (g_popup->moreIcon.x - CXI) * T, NY = CYI + (g_popup->moreIcon.y - CYI) * T;
                const double NW = ib.width + (SZ - ib.width) * T;
                ib      = CBox{NX - NW / 2.0, NY - NW / 2.0, NW, NW};
                ICONROT = ROT + (g_popup->moreIconRot - ROT) * T;
                ICONA   = A * (1.F - R.top * R.top * R.top);
            }
            const float LABELA = A * (1.F - R.drop) * (1.F - R.drop) * (1.F - R.top) * (1.F - R.top);

            if (const auto TEX = OverviewRender::text(R.name, Config::themeForeground(), sc<int>(std::round(FAN_FONT * SCALE * KT)), false, "",
                                                      sc<int>(std::round(FAN_LABEL_MAX * SCALE * KT)), 500);
                TEX && TEX->ok()) {
                // Rendered at the row's full size (one cached texture) and drawn
                // scaled with the row -- flight and blow-up -- rather than
                // re-rasterised at every size.
                const double LW = TEX->m_size.x * R.k * HB, LH = TEX->m_size.y * R.k * HB;
                const double PADX = 10.0 * SCALE * K, PADY = 5.0 * SCALE * K;
                const double RIGHT = PX - ICON / 2.0 - 8.0 * SCALE * K; // the pill ends just left of the icon
                const double CXL = RIGHT - PADX - LW / 2.0;

                // The pill: the theme's background, the accent for the row the
                // pointer, the keyboard or the preview is on.
                const auto PILLW = sc<int>(std::round((TEX->m_size.x + 20.0 * SCALE * KT))), PILLH = sc<int>(std::round(TEX->m_size.y + 10.0 * SCALE * KT));
                // Always the dock's own dark, so the label stays readable; the
                // row being pointed at, selected or previewed says so with the
                // blow-up and a heavier outline, not an accent fill (an accent
                // behind light text washed it out, 2026-09-24).
                const CHyprColor BG = Config::dockBackgroundColor().modifyA(0.92F);
                // Bordered like the dock itself (user, 2026-09-24): the theme
                // accent at Hyprland's border weight, baked into the pill so
                // it turns with it.
                const int BORDER = std::max(1, Config::getValue<int>("general:border_size")) * ((PREV || SEL || HOVER) ? 2 : 1);
                const auto PILLTEX = OverviewRender::roundedRect(PILLW, PILLH, sc<int>(std::round(8.F * SCALE * KT)), BG, Config::highlightColor(), BORDER);
                drawTex(PILLTEX, OverviewRender::padBox(turned(CXL, PY, LW + 2 * PADX, LH + 2 * PADY), PILLTEX), LABELA, ROT);
                drawTex(TEX, turned(CXL, PY, LW, LH).round(), LABELA, ROT);
            }

            if (FROMPILE) {
                // Lerp box and angle from the pile slot to the row; drawn opaque
                // throughout, because it never stops being on screen.
                const CBox& P0 = FROMPILE->box;
                CBox        fly{P0.x + (ib.x - P0.x) * E, P0.y + (ib.y - P0.y) * E, P0.width + (ib.width - P0.width) * E, P0.height + (ib.height - P0.height) * E};
                const auto FLYTEX = iconTexture(FROMPILE->entry, FANPX, true);
                drawTex(FLYTEX, OverviewRender::padBox(fly, FLYTEX), 1.F - E * (1.F - ICONA), FROMPILE->rot + (ICONROT - FROMPILE->rot) * E);
            } else
            {
                // Padded, so its turned edge is filtered, not stair-stepped. A
                // file's real preview when it has one (2026-09-24).
                const bool HASENTRY = R.vis >= 0 && sc<size_t>(R.vis) < g_popup->visible.size() && g_popup->visible[sc<size_t>(R.vis)] < g_popup->entries.size();
                const auto ICONTEX  = HASENTRY ? iconTexture(g_popup->entries[g_popup->visible[sc<size_t>(R.vis)]], FANPX, true) :
                                                 OverviewRender::appIcon(themedIcon(R.icon), false, true, FANPX);
                drawTex(ICONTEX, OverviewRender::padBox(ib, ICONTEX), ICONA, ICONROT);
            }
        }
        };

        // REAL MOTION BLUR (user, 2026-09-24; StackBlur.hpp): while the fan
        // moves, lay it out at SAMPLES instants across the time this frame
        // covers and let StackBlur average them. The last sample is "now", so
        // p.rows -- what clicks are tested against -- ends as the current layout.
        const int  SAMPLES = std::clamp(Config::getValue<int>("plugin:hyprdrive:dock:stack_motion_blur"), 0, 32);
        const bool MOVING  = std::abs(PROGRESS - g_popup->lastProgress) > 1e-4F;
        if (SAMPLES > 1 && MOVING && StackBlur::usable(monitor)) {
            std::vector<StackBlur::SSample> samples;
            sink = &samples;
            // One group per instant: StackBlur composites each on its own, then
            // averages them.
            for (int k = SAMPLES - 1, j = 0; k >= 0; --k, ++j) {
                sinkGroup = j;
                const float PK = std::clamp(PROGRESS - (PROGRESS - g_popup->lastProgress) * sc<float>(k) / sc<float>(SAMPLES), 0.F, 1.F);
                layoutFan(*g_popup, monitor, layout.pill, PK);
                drawFanRows(g_popup->rows);
            }
            sink = nullptr;
            StackBlur::queue(std::move(samples), sinkBounds);
        } else
            drawFanRows(g_popup->rows);
        g_popup->lastProgress = PROGRESS;

        // THE FILTER FIELD (user, 2026-10-08): typing while on the fan filters
        // it, and the query shows in a pill beside the fan -- the overview's
        // search pill, its plate, its springs and its letters, without the
        // glyph. It grows out of the fan's side, right by default, left when
        // the right has no room, and only ever as wide as that side's screen;
        // a longer query scrolls inside it. Clearing the query folds it away.
        {
            auto&       P   = *g_popup;
            const auto  NOW = Time::steadyNow();
            P.letters.sync(P.query, NOW);
            const float H      = OVERVIEW_PILL_HL * SCALE;
            const float PAD    = OVERVIEW_PILL_TAILL * SCALE;
            const float GAP    = 12.F * SCALE;
            const float MARGIN = 8.F * SCALE;
            const float ROW0Y  = FANBASE - FAN_STEP * SCALE + FAN_ROW_H * SCALE / 2.F;
            const float RIGHTX = FANCX + ICONCLEAR + BARTHICK + GAP;
            float       leftX  = 1e9F; // the labels of the bottom rows
            for (const auto& R : P.rows) {
                const float U = R.vis >= 0 ? sc<float>(R.vis) - P.scrollPos : 0.F;
                if (R.e > 0.9F && R.drop < 0.5F && U < 1.5F)
                    leftX = std::min(leftX, sc<float>(R.box.x));
            }
            if (leftX > 1e8F)
                leftX = FANCX - FAN_ICON * SCALE;
            leftX -= GAP;
            const float ROOMR = sc<float>(monitor->m_size.x) * SCALE - MARGIN - RIGHTX;
            const float ROOML = leftX - MARGIN;

            if (P.query.empty() && P.filterW < 1.F)
                P.filterPlaced = false;
            bool warp = false;
            if (!P.query.empty() && !P.filterPlaced) {
                P.filterLeft   = ROOMR < 220.F * SCALE && ROOML > ROOMR;
                P.filterPlaced = true;
                warp           = true;
            }
            const float EDGEL = (P.filterLeft ? leftX : RIGHTX) / SCALE;
            if (warp) {
                P.filterEdge  = EDGEL;
                P.filterEdgeV = 0.F;
            } else if (qtSpringStep(P.filterEdge, P.filterEdgeV, EDGEL, SSearchFieldMotion::WIDTH_SPRING, SSearchFieldMotion::WIDTH_DAMPING, SSearchFieldMotion::WIDTH_MASS,
                                    SSearchFieldMotion::WIDTH_EPSILON, P.lastDT))
                P.filterMoving = true;
            P.filterMaxL = std::max(0.F, (P.filterLeft ? ROOML : ROOMR) / SCALE);

            // The letters, as the overview's field draws them: one texture per
            // codepoint, popped in and flung out by SearchLetters. Measured even
            // before the pill shows, so it grows straight to the query's width.
            const bool  NOMATCH = !P.query.empty() && P.visible.empty();
            const auto  FG      = NOMATCH ? Config::themeUrgent() : Config::themeForeground();
            const int   FONTPX  = std::max(1, sc<int>(std::round(OVERVIEW_PILL_TEXTPX * SCALE)));
            const auto  LIVE    = P.letters.liveGlyphs();
            const float SPACING = std::max(1.F, SCALE);
            std::vector<float> widths;
            widths.reserve(LIVE.size());
            for (const auto& G : LIVE) {
                const auto TEX = OverviewRender::text(G, FG, FONTPX, false, "", 0, 600);
                widths.push_back(TEX && TEX->ok() ? sc<float>(TEX->m_size.x) : sc<float>(FONTPX) * 0.5F);
            }
            P.filterContentL = P.letters.liveEndX(widths, SPACING) / SCALE;

            const float WPX = std::max(0.F, P.filterW) * SCALE;
            P.filterPill    = {};
            if (WPX >= 2.F && P.filterPlaced) {
                const float EDGE = P.filterEdge * SCALE;
                CBox        pillBox{P.filterLeft ? EDGE - WPX : EDGE, ROW0Y - H / 2.F, WPX, H};
                pillBox.round();
                P.filterPill   = pillBox;
                const float FA = ALPHA * std::clamp(WPX / H, 0.F, 1.F);
                const int   RND = std::min({sc<int>(std::round(Config::themeRounding() * SCALE)), sc<int>(pillBox.height / 2.0), sc<int>(pillBox.width / 2.0)});
                OverviewRender::themePlate(pillBox, RND, Config::themeRoundingPower(), FA, false, true);

                const CBox  FIELD{pillBox.x + PAD, pillBox.y, std::max(0.0, pillBox.width - 2.0 * PAD), pillBox.height};
                const float SCROLLPX = std::round(P.filterScroll * SCALE);
                const float FADEPX   = std::max(1.F, OVERVIEW_PILL_FADEL * SCALE);
                const float FADEON   = std::clamp(SCROLLPX / FADEPX, 0.F, 1.F);
                for (const auto& PL : P.letters.place(widths, SPACING, SCALE, NOW, sc<int>(monitor->m_id))) {
                    const auto TEX = OverviewRender::text(PL.glyph, FG, FONTPX, false, "", 0, 600);
                    if (!TEX || !TEX->ok() || PL.opacity <= 0.F)
                        continue;
                    const float GW = sc<float>(TEX->m_size.x), GH = sc<float>(TEX->m_size.y);
                    const float CX = sc<float>(FIELD.x) - SCROLLPX + PL.x + GW * 0.5F;
                    const float CY = sc<float>(pillBox.y) + std::round((sc<float>(pillBox.height) - GH) / 2.F) + GH * 0.5F + PL.y;
                    float       ea = 1.F;
                    if (FADEON > 0.F) {
                        const float T = std::clamp((CX - sc<float>(FIELD.x)) / FADEPX, 0.F, 1.F);
                        ea            = 1.F - FADEON * (1.F - T * T * (3.F - 2.F * T));
                    }
                    CTexPassElement::SRenderData d;
                    d.tex     = TEX;
                    d.box     = CBox{CX - GW * PL.scale * 0.5F, CY - GH * PL.scale * 0.5F, GW * PL.scale, GH * PL.scale};
                    d.a       = FA * PL.opacity * ea;
                    d.clipBox = pillBox;
                    if (drawableBox(d.box))
                        g_pHyprRenderer->addPassElement(makeUnique<CTexPassElement>(d));
                }
            }
        }

        drawGhost();
    }

}
