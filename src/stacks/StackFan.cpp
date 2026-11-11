// Dock stacks: icon naming, and the fan and pane geometry and motion -- the popup's timeline,
// scroll spring and layout. Split out of DockStacks.cpp; the state is still there
// (StackParts.hpp).
#include "StackParts.hpp"
#include "MotionCurves.hpp"
#include "MotionClock.hpp"

using namespace StacksDetail;

namespace StacksDetail {

    // GIcon -> the first themed name, which is what OverviewRender::appIcon
    // resolves. Locations.cpp has the same four lines in its own anonymous
    // namespace; copying them is cheaper than exporting a GIcon* across a
    // header that no other module should need gio for.
    std::string themedName(GIcon* icon) {
        if (!icon || !G_IS_THEMED_ICON(icon))
            return {};
        const char* const* NAMES = g_themed_icon_get_names(G_THEMED_ICON(icon));
        return NAMES && NAMES[0] ? std::string{NAMES[0]} : std::string{};
    }

    // THE MIME LOOKUP IS NAME-ONLY, ON PURPOSE. g_content_type_guess with no
    // data buffer decides from the filename alone: no open(), no read(), no
    // stat beyond what the directory walk already did. Locations.cpp records
    // what a synchronous gio query on this thread costs -- a `trash:///`
    // query_info wedged the compositor -- and a fan of 200 files would be 200
    // of them.
    std::string iconNameForFile(const std::string& path, bool isDir) {
        if (isDir)
            return "folder";

        gboolean uncertain = FALSE;
        char*    ct        = g_content_type_guess(path.c_str(), nullptr, 0, &uncertain);
        if (!ct)
            return "text-x-generic";

        GIcon*     icon = g_content_type_get_icon(ct);
        const auto NAME = themedName(icon);
        if (icon)
            g_object_unref(icon);
        g_free(ct);

        return NAME.empty() ? std::string{"text-x-generic"} : NAME;
    }

    // The reference's special-directory table, which is the one part of its
    // icon story worth copying exactly: a Downloads stack that draws a generic
    // folder is indistinguishable from every other folder on the dock.
    std::string folderIcon(const std::string& path) {
        static const std::array<std::pair<GUserDirectory, const char*>, 8> TABLE = {{
            {G_USER_DIRECTORY_DOWNLOAD, "folder-download"},
            {G_USER_DIRECTORY_DOCUMENTS, "folder-documents"},
            {G_USER_DIRECTORY_MUSIC, "folder-music"},
            {G_USER_DIRECTORY_PICTURES, "folder-pictures"},
            {G_USER_DIRECTORY_VIDEOS, "folder-videos"},
            {G_USER_DIRECTORY_DESKTOP, "user-desktop"},
            {G_USER_DIRECTORY_TEMPLATES, "folder-templates"},
            {G_USER_DIRECTORY_PUBLIC_SHARE, "folder-publicshare"},
        }};

        for (const auto& [DIR, ICON] : TABLE) {
            const char* SPECIAL = g_get_user_special_dir(DIR);
            if (SPECIAL && canonical(SPECIAL) == path)
                return ICON;
        }
        if (const char* HOME = g_get_home_dir(); HOME && canonical(HOME) == path)
            return "user-home";
        return "folder";
    }

    // The popup's entries: the reader's listing (StackReader, off the compositor
    // thread), sorted by the stack's key. Empty and `ready=false` until the
    // first read lands; onReaderReady() fills the popup when it does.
    // The reader hands every candidate icon name ('|'-joined, most specific
    // first); the first one the user's theme has wins. Memoized per list.
    std::string pickIconImpl(const std::string& names) {
        static std::unordered_map<std::string, std::string> memo;
        if (const auto IT = memo.find(names); IT != memo.end())
            return IT->second;
        std::string pick = "text-x-generic";
        size_t      at   = 0;
        while (at <= names.size()) {
            const auto BAR  = names.find('|', at);
            const auto NAME = names.substr(at, BAR == std::string::npos ? std::string::npos : BAR - at);
            const auto HAS  = AppSearch::iconExistsNow(NAME);
            if (!HAS)
                return pick; // the index is busy (startup scan): answer generic, do NOT memoize, ask next time
            if (*HAS) {
                pick = NAME;
                break;
            }
            if (BAR == std::string::npos)
                break;
            at = BAR + 1;
        }
        return memo[names] = pick;
    }

    StackReader::SListing sortedListing(const std::string& dir, StackModel::ESort sort) {
        auto L = StackReader::listing(dir);
        for (auto& E : L.entries)
            E.icon = E.dir ? folderIcon(canonical(E.path)) : pickIconImpl(E.icon);
        StackModel::sortEntries(L.entries, sort);
        return L;
    }

    const SFanMotion& fanMotion() {
        static const SFanMotion EXT{140.F, 140.F, 11.F, 1e9F, 84.F, 120.F, false};
        // Serpantinum's numbers x0.8 (user, 2026-09-24: "the curve is nice,
        // maybe 20% faster").
        static const SFanMotion SERP{224.F, 208.F, 32.F, 240.F, 120.F, 176.F, true};
        // A hot read (Config.hpp): every row of every frame, so the handle,
        // compared in place -- no hash, no copy.
        if (!Hyprdrive::Config::g_pluginKeysRegistered)
            return SERP;
        static auto& VAL = Hyprdrive::Config::valueRef<std::string>("plugin:hyprdrive:dock:stack_motion");
        return *VAL == "extension" ? EXT : SERP;
    }
    std::string popupState() {
        if (!g_popup)
            return "none";
        return std::string(g_popup->leaving ? "closing" : "opening/open") + " p=" + std::to_string(g_popup->tl);
    }



    // A row is only hit where it is SHOWN: the pane's cells scroll under a
    // viewport, and the part of a cell past it (over the dock, 2026-09-24 bug)
    // is not there to click.
    bool rowHit(const SRow& R, const Vector2D& point) {
        if (!R.box.containsPoint(point))
            return false;
        // A fan row in flight is not a target yet: only once it has (nearly)
        // landed -- a click aimed at the stack must not catch a passing paper.
        if (R.e < 0.9F)
            return false;
        // Nor is one dropping into the pile or leaving past the top.
        if (R.drop > 0.35F || R.top > 0.5F)
            return false;
        return !g_popup || !g_popup->grid || g_popup->gridBox.empty() || g_popup->gridBox.containsPoint(point);
    }

    // Hops and shakes that have run their course. Only Stacks::render() used
    // to retire them, and the HIDDEN dock never reaches it: a file landing
    // while the dock was away kept animating() true, and every monitor
    // repainted at full rate until the dock was next shown (review
    // 2026-11-09). Retired before every answer about frames now. The hop is
    // kept for the hidden dock's longer peek (Dock::STACK_PEEK_MS); on the
    // dock it is over at HOP_MS and draws nothing after.
    void retireHops() {
        std::erase_if(g_bounces, [](const SHop& h) { return Hyprdrive::Motion::Clock::animMs(h.startedAt) > std::max(Dock::HOP_MS, Dock::STACK_PEEK_MS); });
        std::erase_if(g_shakes, [](const SHop& h) { return Hyprdrive::Motion::Clock::animMs(h.startedAt) > SHAKE_MS; });
    }

    // Rows the fan shows at once: every visible entry, up to its slots -- and
    // one row ("Loading", "Empty folder", "No matches") when there is none.
    size_t fanRows(const SPopup& p) {
        const size_t N = p.visible.size();
        return std::max<size_t>(1, p.slots > 0 ? std::min(N, p.slots) : N);
    }
    // How far the fan can scroll: 0 when everything shows.
    float fanMaxScroll(const SPopup& p) {
        return p.slots > 0 && p.visible.size() > p.slots ? sc<float>(p.visible.size() - p.slots) : 0.F;
    }

    // ...plus the "N more in Files" row on top of a fan that scrolls.
    size_t fanRowsDrawn(const SPopup& p) {
        return fanRows(p) + (fanMaxScroll(p) > 0.F ? 1 : 0);
    }

    float fanStagger(const SPopup& p) {
        const auto&  M    = fanMotion();
        const size_t ROWS = std::min(fanRowsDrawn(p), STAGGER_ROWS);
        return ROWS > 1 ? std::min(M.staggerMs, M.staggerCapMs / sc<float>(ROWS - 1)) : M.staggerMs;
    }

    // A THROWN PACK, NOT A DEALT ONE (user, 2026-09-24: "like I just threw a
    // pack of papers on the ground and they happened to land in a fan"). In
    // the serpantinum motion each paper gets its own small, repeatable
    // randomness -- seeded by the stack's path and the row, so a stack always
    // lands the same way: when it leaves (roughly together, not strictly one
    // after another), how far its path bows to one side, and how far off its
    // final tilt it starts spinning from.
    float rowJitter(const SPopup& p, size_t i, int salt) {
        const double SEED = sc<double>(std::hash<std::string>{}(p.path) % 997) * 0.37 + sc<double>(i) * 12.9898 + salt * 78.233;
        const double F    = std::sin(SEED) * 43758.5453;
        return sc<float>((F - std::floor(F)) * 2.0 - 1.0); // -1..1
    }

    float rowDelay(const SPopup& p, size_t i) {
        const float STAGGER = fanStagger(p);
        const float I       = sc<float>(std::min(i, STAGGER_ROWS));
        if (!fanMotion().serp)
            return STAGGER * I;
        // Bottom papers still tend to land first -- they have least far to go --
        // but a pack does not leave in file order.
        // Shifted so the earliest paper leaves at once: any lead-in before
        // the first movement reads as the click lagging.
        float lead = 1e9F;
        const size_t ROWS = std::min(fanRowsDrawn(p), STAGGER_ROWS);
        for (size_t k = 0; k < ROWS; ++k)
            lead = std::min(lead, STAGGER * (0.55F * sc<float>(k) + 0.9F * (rowJitter(p, k, 1) + 1.F) / 2.F));
        return STAGGER * (0.55F * I + 0.9F * (rowJitter(p, i, 1) + 1.F) / 2.F) - lead;
    }

    // How long the opening timeline is: the pane's grow, or the fan's last
    // row landing.
    float timelineSpan(const SPopup& p) {
        if (p.grid)
            return PANE_OPEN_MS;
        const size_t ROWS = std::min(fanRowsDrawn(p), STAGGER_ROWS);
        float        last = 0.F;
        for (size_t i = 0; i < ROWS; ++i)
            last = std::max(last, rowDelay(p, i));
        return fanMotion().rowMs + last;
    }

    bool scrollMoving(const SPopup& p) {
        return p.scrollPos != p.scrollGoal || p.scrollVel != 0.F;
    }

    void stepScroll(SPopup& p, float dtMs) {
        const float MAXS = fanMaxScroll(p);
        p.scrollGoal     = std::clamp(p.scrollGoal, 0.F, MAXS);
        for (float left = std::min(dtMs, 1000.F); left > 0.F;) {
            const float H = std::min(4.F, left) / 1000.F;
            left -= 4.F;
            const float ACC = -SCROLL_OMEGA * SCROLL_OMEGA * (p.scrollPos - p.scrollGoal) - 2.F * SCROLL_OMEGA * p.scrollVel;
            p.scrollVel += ACC * H;
            p.scrollPos += p.scrollVel * H;
        }
        if (std::abs(p.scrollPos - p.scrollGoal) < 0.002F && std::abs(p.scrollVel) < 0.02F) {
            p.scrollPos = p.scrollGoal;
            p.scrollVel = 0.F;
        }
        p.scrollPos = std::clamp(p.scrollPos, -0.25F, MAXS + 0.25F);
    }

    void tick(SPopup& p) {
        const auto NOW = Time::steadyNow();
        // The animation clock's step (MotionClock.hpp), like every other
        // hyprdrive animation: 10 = ten times slower. A snapping clock hands out
        // SNAP_MS from the very first tick, so the popup is open (or gone) on
        // the frame it was asked for; the springs below settle on it.
        const bool SNAP = !Hyprdrive::Motion::Clock::enabled();
        float      DT   = SNAP ? Hyprdrive::Motion::SNAP_MS : Hyprdrive::Motion::Clock::animDt(p.ticked ? std::min(Hyprdrive::Motion::msBetween(NOW, p.lastTick), MAX_TICK_MS) : 0.F);
        p.lastTick     = NOW;
        p.ticked       = true;
        p.lastDT       = DT;
        // Only a FIRST read holds it: a folder re-reading after a change still
        // has its previous entries to show, and shows them.
        if (!p.leaving && p.loading && p.entries.empty() && Hyprdrive::Config::realElapsedMs(p.since) < OPEN_WAIT_MS)
            DT = 0.F;
        // The hover blow-ups ride the same frame clock.
        p.hoverMoving = false;
        for (auto& [KEY, amt] : p.hoverAmt) {
            const bool ON = KEY == p.hoverKey;
            amt           = std::clamp(amt + (ON ? 1.F : -1.F) * DT / fanMotion().hoverMs, 0.F, 1.F);
            p.hoverMoving |= ON ? amt < 1.F : amt > 0.F;
        }
        std::erase_if(p.hoverAmt, [&](const auto& kv) { return kv.second <= 0.F && kv.first != p.hoverKey; });

        // The fan's scroll, and the scrollbar that shows while it moves.
        if (p.fingerScroll && Hyprdrive::Config::realElapsedMs(p.lastScrollAt) > FINGER_REST_MS) {
            p.scrollGoal   = std::round(p.scrollGoal);
            p.fingerScroll = false;
        }
        if (SNAP) {
            p.scrollGoal = std::clamp(p.scrollGoal, 0.F, fanMaxScroll(p));
            p.scrollPos  = p.scrollGoal;
            p.scrollVel  = 0.F;
        } else
            stepScroll(p, DT);
        const bool BARON = !p.grid && fanMaxScroll(p) > 0.F && (scrollMoving(p) || p.fingerScroll || Hyprdrive::Config::realElapsedMs(p.lastScrollAt) < BAR_HOLD_MS);
        p.barAlpha       = std::clamp(p.barAlpha + (BARON ? DT / BAR_IN_MS : -DT / BAR_OUT_MS), 0.F, 1.F);

        // The filter field: the overview pill's springs (SSearchFieldMotion's
        // PondFluid numbers), so the two fields move alike.
        if (!p.grid) {
            using M          = SSearchFieldMotion;
            const float PADL = OVERVIEW_PILL_TAILL;
            const float WANT = p.query.empty() ? 0.F : std::min(std::ceil(2.F * PADL + p.filterContentL + OVERVIEW_PILL_ENDROOML), std::max(2.F * PADL, p.filterMaxL));
            const float OVER = p.query.empty() ? 0.F : std::max(0.F, std::ceil(2.F * PADL + p.filterContentL + OVERVIEW_PILL_ENDROOML) - WANT);
            bool        mv   = qtSpringStep(p.filterW, p.filterV, WANT, M::WIDTH_SPRING, M::WIDTH_DAMPING, M::WIDTH_MASS, M::WIDTH_EPSILON, DT);
            mv |= qtSpringStep(p.filterScroll, p.filterScrollV, OVER, M::SLIDE_SPRING, M::SLIDE_DAMPING, M::SLIDE_MASS, M::SLIDE_EPSILON, DT);
            p.filterMoving = mv;
        }

        const float SPAN = timelineSpan(p);
        if (p.leaving)
            p.tl = std::max(0.F, p.tl - DT * SPAN / (p.grid ? PANE_CLOSE_MS : fanMotion().closeMs));
        else
            p.tl = std::min(SPAN, p.tl + DT);
    }

    float timelineNow(const SPopup& p) {
        return std::clamp(p.tl, 0.F, timelineSpan(p));
    }

    // 0 closed .. 1 open, on the way in or out alike.
    float popupProgress(const SPopup& p) {
        return std::clamp(timelineNow(p) / std::max(1.F, timelineSpan(p)), 0.F, 1.F);
    }

    // Turn round: whatever the popup is doing, it now heads the other way from
    // exactly where it is.

    void setDirection(SPopup& p, bool leaving) {
        p.leaving = leaving; // the clock simply runs the other way from here
        // The clock only runs on DRAWN frames, and a fold on the press (its
        // release swallowed) or a turn-round starts no other animation that
        // would ask for one -- so the popup sat frozen at p=0 until something
        // unrelated repainted (2026-09-24 trace). Every change of direction
        // asks for its frame.
        damagePopupMonitor();
    }

    // Row i's three channels on the fan's timeline: where it is (0 in the
    // cell, 1 in place), how big (may pass 1 -- the OutBack pop), how opaque.
    struct SRowEase {
        float pos = 0.F, scale = 0.F, alpha = 0.F, spin = 1.F; // spin: 0 thrown, 1 settled at its tilt
    };
    SRowEase fanRowEase(const SPopup& p, size_t i, float progress) {
        const auto& M  = fanMotion();
        const float T  = progress * timelineSpan(p) - rowDelay(p, i);
        const float TR = std::clamp(T / M.rowMs, 0.F, 1.F);
        const float TA = std::clamp(T / M.alphaMs, 0.F, 1.F);
        if (!M.serp)
            return {Hyprdrive::Motion::outQuad(TR), Hyprdrive::Motion::outQuad(TR), Hyprdrive::Motion::outQuad(TR), 1.F};
        // The spin settles with a small wobble past its tilt, as paper does.
        // Qt's OutQuint / OutBack / OutCubic, through pow as Qt writes them.
        return {Hyprdrive::Motion::outQuintPow(TR), Hyprdrive::Motion::outBack(TR, 1.1F), Hyprdrive::Motion::outCubicPow(TA), Hyprdrive::Motion::outBack(TR, 1.4F)};
    }

    // ------------------------------------------------------------ fan layout --
    // Laid out into `rows`, in DRAWN coordinates, so the hit test and the paint
    // can never disagree: the same function fills both, exactly as renderMenu
    // does for the context plate.
    void layoutFan(SPopup& p, PHLMONITOR monitor, const CBox& pill, float progress) {
        const float SCALE = std::max(monitor->m_scale, 0.01F);
        const float CX    = sc<float>(p.anchor.x) + sc<float>(p.anchor.width) / 2.F;
        const float BASE  = sc<float>(pill.y) - FAN_GAP * SCALE;

        // One row per item in view. An empty folder -- or one whose first
        // listing has not landed, or a filter nothing matches -- still shows ONE
        // row saying so, so a click always shows something.
        const size_t N        = p.visible.size();
        const bool   EMPTYROW = N == 0;
        const size_t SLOTS    = fanRows(p);
        const bool   SCROLLS  = fanMaxScroll(p) > 0.F;
        const float  SP       = EMPTYROW ? 0.F : p.scrollPos;
        const float  TOPSLOT  = sc<float>(SLOTS) - 1.F;

        // THE WINDOW: entry j sits at slot u = j - SP. u in [0, SLOTS-1] is the
        // fan; (-1, 0) is on its way down into the pile; (SLOTS-1, SLOTS) on
        // its way out past the top.
        int J0 = 0, J1 = 0;
        if (!EMPTYROW) {
            J0 = std::max(0, sc<int>(std::floor(SP)));
            J1 = std::min(sc<int>(N) - 1, sc<int>(std::ceil(SP + sc<float>(SLOTS))) - 1);
        }

        // "N MORE IN FILES" IS BACK (user, 2026-10-08), as a row of its own
        // above the top slot that stays put while the rest scroll: N is what
        // is still above the window, so it counts down as the fan scrolls up,
        // and at the end it just opens the folder. Rows leaving past the top
        // fade into it.
        const bool MOREROW = SCROLLS;
        const int  MOREJ   = J1 + 1;

        p.rows.clear();
        p.rows.reserve(sc<size_t>(std::max(1, J1 - J0 + 2)));
        double minLeft = 1e9; // the settled fan's leftmost point, labels turned
        for (int j = J0; j <= J1 + (MOREROW ? 1 : 0); ++j) {
            const bool  MORE = MOREROW && j == MOREJ;
            const float U    = MORE ? sc<float>(SLOTS) : EMPTYROW ? 0.F : sc<float>(j) - SP;
            if (!MORE && !EMPTYROW && (U <= -1.F || U >= sc<float>(SLOTS)))
                continue;
            const float DROP = MORE ? 0.F : std::clamp(-U, 0.F, 1.F);
            const float TOP  = MORE ? 0.F : std::clamp(U - TOPSLOT, 0.F, 1.F);
            // A row on its way into the pile keeps the bottom slot's place; the
            // paint flies its icon on from there.
            const float  G  = std::max(U, 0.F);
            const size_t SI = MORE ? SLOTS : sc<size_t>(std::clamp(std::round(G), 0.F, TOPSLOT)); // its slot, for the opening's stagger and its throw
            const float  SHIFT = FAN_SHIFT_K * std::pow(G, FAN_SHIFT_P) * SCALE;

            // The row's own eased progress on the ONE timeline (fanRowEase):
            // the stagger and the reversal both come from there.
            const auto  EZ = fanRowEase(p, SI, progress);
            const float E  = EZ.pos;

            const float ROWH  = FAN_ROW_H * SCALE;
            const float RESTY = BASE - FAN_STEP * SCALE * (G + 1.F);

            std::string LABEL, ICON;
            if (MORE) {
                const int ABOVE = std::max(0, sc<int>(N) - sc<int>(SLOTS) - sc<int>(std::round(SP)));
                LABEL           = ABOVE > 0 ? std::to_string(ABOVE) + " more in Files" : std::string{"Open in Files"};
                ICON            = "folder-open";
            } else if (!EMPTYROW) {
                LABEL = p.entries[p.visible[sc<size_t>(j)]].name;
                ICON  = p.entries[p.visible[sc<size_t>(j)]].icon;
            } else {
                LABEL = p.loading && p.entries.empty() ? std::string{"Loading\xe2\x80\xa6"} : !p.query.empty() ? std::string{"No matches"} : std::string{"Empty folder"};
                ICON  = "folder-open";
            }

            // The row's width is its label plus its icon; measured through the
            // cached text path, which is the only legal way to ask inside a
            // pass and the only affordable way anywhere.
            float labelW = 0.F;
            if (const auto TEX = OverviewRender::text(LABEL, CHyprColor{1.F, 1.F, 1.F, 0.95F}, sc<int>(std::round(FAN_FONT * SCALE)), false, "",
                                                      sc<int>(std::round(FAN_LABEL_MAX * SCALE)), 500);
                TEX && TEX->ok())
                labelW = sc<float>(TEX->m_size.x);

            const float ICONW = FAN_ICON * SCALE;
            const float W     = FAN_PAD_X * SCALE * 2.F + labelW + (labelW > 0.F ? FAN_PAD_X * SCALE : 0.F) + ICONW;

            // Right-aligned on the anchor's centre plus the shift, so the ICON
            // column tracks the fan's arc and the labels ragged out to the left
            // -- which is the reference's shape.
            CBox rest{std::round(CX + SHIFT - W + ICONW / 2.F + FAN_PAD_X * SCALE), std::round(RESTY), std::round(W), std::round(ROWH)};
            rest.x = std::max(0.0, std::min(rest.x, sc<double>(monitor->m_size.x * SCALE) - rest.width));

            // The extension's flight: the row starts on the cell's centre at
            // 0.1x and flies to its place growing to full size, scaled about its
            // ICON (the pivot the extension sets), so the icons trace the arc.
            // Starts at 0.35x in the thrown motion so the first frame already
            // shows paper leaving (0.1x read as a delay); 0.1x in the extension's.
            const double K0 = fanMotion().serp ? 0.35 : 0.1;
            const double K  = K0 + (1.0 - K0) * EZ.scale;
            const double C1X = rest.x + rest.width - FAN_PAD_X * SCALE - ICONW / 2.0, C1Y = rest.y + rest.height / 2.0;
            const double C0X = p.anchor.x + p.anchor.width / 2.0, C0Y = p.anchor.y + p.anchor.height / 2.0;
            // The path: a straight line for the extension; for the thrown pack
            // a curve that bows to one side by up to a third of the distance
            // (quadratic Bezier, control point off the midpoint, square to
            // the line), so each paper arrives on its own arc.
            double CXN = C0X + (C1X - C0X) * E, CYN = C0Y + (C1Y - C0Y) * E;
            if (fanMotion().serp) {
                const double DX = C1X - C0X, DY = C1Y - C0Y, LEN = std::max(1.0, std::hypot(DX, DY));
                const double BOW = LEN * 0.33 * rowJitter(p, SI, 2);
                const double MX = (C0X + C1X) / 2.0 - DY / LEN * BOW, MY = (C0Y + C1Y) / 2.0 + DX / LEN * BOW;
                const double V = 1.0 - E;
                CXN            = V * V * C0X + 2.0 * V * E * MX + E * E * C1X;
                CYN            = V * V * C0Y + 2.0 * V * E * MY + E * E * C1Y;
            }
            // ...and it turns as it flies: from up to 40 degrees off its final
            // tilt, settling onto it. A scrolling row turns with its slot.
            const float TILT  = FAN_TILT_DEG * G * 3.14159265F / 180.F;
            const float THROW = fanMotion().serp ? rowJitter(p, SI, 3) * 40.F * 3.14159265F / 180.F : 0.F;
            const float RROT  = TILT + THROW * (1.F - EZ.spin);
            // Where this row's label ends up once settled and turned: its far
            // end swings up and left about the icon by the row's tilt.
            // (The label is drawn from the ICON leftwards -- gap, then its pill
            // -- not from the row box, which is clamped.)
            if (!SCROLLS) {
                const double REACH = (ICONW / 2.0 + 8.0 * SCALE + labelW + 20.0 * SCALE) * HOVER_SCALE; // room for the hover blow-up
                minLeft            = std::min(minLeft, C1X - REACH * std::cos(TILT) - rest.height / 2.0 * std::sin(TILT));
            }
            CBox box{CXN - (C1X - rest.x) * K, CYN - (C1Y - rest.y) * K, rest.width * K, rest.height * K};

            p.rows.emplace_back(SRow{
                .box    = box,
                .path   = EMPTYROW || MORE ? std::string{} : p.entries[p.visible[sc<size_t>(j)]].path,
                .name   = LABEL,
                .icon   = ICON,
                .action = EMPTYROW || MORE,
                .vis    = EMPTYROW || MORE ? -1 : j,
                .k      = sc<float>(K),
                .e      = E,
                .a      = EZ.alpha,
                .rot    = RROT,
                .drop   = DROP,
                .top    = TOP,
            });
        }

        // A scrolling fan's rows change under it, and a clamp measured off
        // whichever labels are in view would slide the whole fan sideways at
        // every step: it is measured off every slot at the widest label the
        // folder has instead, so it holds still.
        if (SCROLLS) {
            const double ICONW = FAN_ICON * SCALE;
            for (size_t k = 0; k <= SLOTS; ++k) { // the "more" row's slot too
                const double TILT  = FAN_TILT_DEG * sc<double>(k) * 3.14159265 / 180.0;
                const double C1X   = CX + FAN_SHIFT_K * std::pow(sc<double>(k), FAN_SHIFT_P) * SCALE;
                const double REACH = (ICONW / 2.0 + 8.0 * SCALE + p.maxLabelW + 20.0 * SCALE) * HOVER_SCALE;
                minLeft            = std::min(minLeft, C1X - REACH * std::cos(TILT) - FAN_ROW_H * SCALE / 2.0 * std::sin(TILT));
            }
        }

        // A fan near the screen's left edge would put its turned labels off it
        // (2026-09-24): the whole settled fan moves right by the shortfall,
        // arc intact, each row by as much of it as it has flown.
        const double MARGIN = 8.0 * SCALE;
        p.edgeShift         = minLeft < MARGIN ? sc<float>(MARGIN - minLeft) : 0.F;
        if (p.edgeShift > 0.F)
            for (auto& R : p.rows)
                R.box.x += p.edgeShift * R.e;

        // Where the "more" row's folder icon is, for the rows flying into it
        // (the same pivot drawFanRows turns that row about).
        p.moreIconK = 0.F;
        if (MOREROW && !p.rows.empty() && p.rows.back().action) {
            const auto& R = p.rows.back();
            p.moreIcon    = Vector2D{R.box.x + R.box.width - FAN_PAD_X * SCALE * R.k - FAN_ICON * SCALE * R.k / 2.0, R.box.y + R.box.height / 2.0};
            p.moreIconK   = R.k;
            p.moreIconRot = R.rot;
        }

        // Where the wheel and the keyboard count as "on the fan": every row it
        // has, the gaps between them, and the scrollbar's gutter on the right.
        CBox B;
        for (const auto& R : p.rows) {
            if (R.e < 0.5F)
                continue;
            if (B.width < 1)
                B = R.box;
            else {
                const double X0 = std::min(B.x, R.box.x), Y0 = std::min(B.y, R.box.y);
                const double X1 = std::max(B.x + B.width, R.box.x + R.box.width), Y1 = std::max(B.y + B.height, R.box.y + R.box.height);
                B               = CBox{X0, Y0, X1 - X0, Y1 - Y0};
            }
        }
        if (B.width >= 1) {
            B.width += FAN_ICON * SCALE;
            B.expand(10.0 * SCALE);
        }
        p.fanBounds = B;
    }

    // THE PANE (dock stacks A): a searchable grid of the VISIBLE entries,
    // scrolled, cells outside the viewport neither drawn nor hit.
    void layoutPane(SPopup& p, PHLMONITOR monitor, const CBox& pill, float progress) {
        // Sized by the whole FOLDER, not the matches: a pane that shrank with
        // every letter typed would jump about under the pointer.
        const auto L  = Stacks::Pane::layout(monitor, p.anchor, pill, p.entries.size(), progress, p.leaving);
        p.plate       = L.plate;
        p.cols        = L.cols;
        p.fieldBox    = L.field;
        p.gridBox     = L.grid;
        p.headerBox   = L.header;
        const float S = std::max(monitor->m_scale, 0.01F);
        const float SP = Stacks::Pane::SPACING * S;
        const int   ROWS = std::max(1, sc<int>((p.visible.size() + L.cols - 1) / L.cols));
        const float MAXSCROLL = std::max(0.F, ROWS * (L.cellH + SP) - SP - sc<float>(L.grid.height));
        // Keep the keyboard's selection in view.
        if (p.keyboard && p.hover >= 0) {
            const float ROWY = sc<float>(p.hover / L.cols) * (L.cellH + SP);
            if (ROWY < p.scroll)
                p.scroll = ROWY;
            else if (ROWY + L.cellH > p.scroll + L.grid.height)
                p.scroll = ROWY + L.cellH - sc<float>(L.grid.height);
        }
        p.scroll = std::clamp(p.scroll, 0.F, MAXSCROLL);

        p.rows.clear();
        for (size_t k = 0; k < p.visible.size(); ++k) {
            const auto& E = p.entries[p.visible[k]];
            CBox box{std::round(L.grid.x + (k % L.cols) * (L.cellW + SP)), std::round(L.grid.y + (k / L.cols) * (L.cellH + SP) - p.scroll), std::round(L.cellW),
                     std::round(L.cellH)};
            if (box.y + box.height < L.grid.y || box.y > L.grid.y + L.grid.height)
                continue;
            p.rows.emplace_back(SRow{.box = box, .path = E.path, .name = E.name, .icon = E.icon, .vis = sc<int>(k)});
        }
    }

    // The popup no longer asks for a frame just for existing (Stacks::
    // needsFrames), so every edit that changes what it looks like WITHOUT
    // moving its entrance has to ask for one itself. All of these arrive on an
    // input event, never from inside the pass, so a plain damage is legal here.
    void damagePopupMonitor() {
        if (!g_popup || !g_pHyprRenderer)
            return;
        for (const auto& MONITOR : State::monitorState()->monitors()) {
            if (MONITOR && MONITOR->m_id == g_popup->monitor)
                g_pHyprRenderer->damageMonitor(MONITOR);
        }
    }


    // THE FIT (dock stacks A: "no fixed cap"). The fan is shown while every row
    // stays one step below the top bar; otherwise the pane. A stack set to
    // Always Grid always takes the pane.
    StackModel::SFanFit fanFit(const SPopup& p, PHLMONITOR monitor) {
        const float S = std::max(monitor->m_scale, 0.01F);
        return StackModel::SFanFit{
            .pillTopL  = sc<float>(p.anchor.y) / S,
            .topLimitL = OvInternal::overviewTopChromeBottomL(monitor) - sc<float>(monitor->m_position.y),
            .stepL     = FAN_STEP,
            .gapL      = FAN_GAP,
        };
    }

    // THE FAN'S REAL HEIGHT (2026-09-24: the top rows' labels ran off the
    // screen). Each row turns about its icon by FAN_TILT_DEG per row, so the
    // far (left) end of its label rises by width x sin(angle) -- 30 degrees by
    // the 12th row. How many rows, in order, fit under `roomL` (logical px
    // above the fan's base) when each label is as long as its name makes it?
    float labelSpanL(const std::string& name) {
        // The fan font is monospaced at FAN_FONT px: ~0.6 em per character,
        // capped where the label ellipsizes; plus the pill's padding and the gap.
        return std::min(sc<float>(name.size()) * FAN_FONT * 0.6F, sc<float>(FAN_LABEL_MAX)) + 20.F + 8.F + FAN_ICON / 2.F;
    }
    size_t rotatedFanCapacity(const SPopup& p, const std::vector<std::string>& names, float roomL) {
        constexpr float MARGIN = 8.F;
        size_t          n      = 0;
        for (size_t i = 0; i < names.size(); ++i) {
            const float TILT = FAN_TILT_DEG * sc<float>(i) * 3.14159265F / 180.F;
            const float UP   = FAN_STEP * sc<float>(i + 1) + labelSpanL(names[i]) * std::sin(TILT) + FAN_ROW_H * 0.15F;
            if (UP + MARGIN > roomL)
                break;
            n = i + 1;
        }
        return n;
    }
    std::vector<std::string> fanNames(const SPopup& p, size_t rows) {
        std::vector<std::string> out;
        for (size_t k = 0; k < std::min(rows, p.entries.size()); ++k)
            out.push_back(p.entries[k].name);
        return out;
    }

    bool chooseGrid(const SPopup& p, PHLMONITOR monitor) {
        if (p.view == StackModel::EView::Grid)
            return true;
        // View in Files: the popup is Super+click's, and it is the fan.
        if (p.view == StackModel::EView::Fan || p.view == StackModel::EView::Files || !monitor)
            return false;
        // Automatic, as the extension: the pane past 12 items or when the fan
        // would not fit the screen.
        if (p.entries.size() > FAN_MAX_ITEMS)
            return true;
        const auto  F    = fanFit(p, monitor);
        const float ROOM = F.pillTopL - F.gapL - F.topLimitL;
        return rotatedFanCapacity(p, fanNames(p, p.entries.size()), ROOM) < std::max<size_t>(1, p.entries.size());
    }

}

