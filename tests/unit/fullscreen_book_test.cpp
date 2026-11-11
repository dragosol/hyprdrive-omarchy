// Host test for FullscreenBook.hpp: the fullscreen desktop's places are kept by
// number (a debt never has place -1, the old "paying back place -1" that ran
// shift(0, -1)), and the payback decision.
#include "FullscreenBook.hpp"
#include "DesktopPlan.hpp"

#include <algorithm>
#include <cstdio>
#include <random>
#include <set>
#include <string>
#include <vector>

using namespace Hyprdrive::FullscreenBook;

static int g_checks = 0, g_failed = 0;
#define CHECK(c)                                                                                                                                                                   \
    do {                                                                                                                                                                           \
        ++g_checks;                                                                                                                                                                \
        if (!(c)) {                                                                                                                                                                \
            ++g_failed;                                                                                                                                                            \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);                                                                                                               \
        }                                                                                                                                                                          \
    } while (0)

static void places() {
    // a live desktop's number wins; a gone one's number is the one kept
    static_assert(placeOf(true, 5, 2) == 5);
    static_assert(placeOf(false, 5, 2) == 2);
    static_assert(placeOf(true, -1, 2) == 2);
    // THE OLD BUG: a trip's desktop gone when its app closed (not on screen):
    // the debt took its number from the desktop alone -> -1. Now the trip keeps
    // its number from the start, and the debt takes that.
    {
        Id   tripAt = 2;                                // startTrip: the place it was given
        bool alive  = false;                            // the app closed, its desktop went
        Id   debtAt = placeOf(alive, NO_PLACE, tripAt); // endTrip
        CHECK(debtAt == 2);
        CHECK(decide(EKind::Trip, {.at = debtAt}) == EPay::Pay);
    }
    // kept numbers move with a shift from at or below them, not above
    static_assert(movedPlace(3, 3, +1) == 4);
    static_assert(movedPlace(3, 4, +1) == 3);
    static_assert(movedPlace(5, 3, -1) == 4);
    // a debt with no place is refused, never paid
    CHECK(decide(EKind::Trip, {.at = NO_PLACE}) == EPay::Refuse);
    CHECK(decide(EKind::Beside, {.at = 0}) == EPay::Refuse);
    CHECK(decide(EKind::Trip, {.at = -1, .exists = true}) == EPay::Refuse);
}

static void decisions() {
    // Trip: a foreign window makes it a desktop of its own; own leftovers do not
    CHECK(decide(EKind::Trip, {.at = 2, .exists = true, .foreign = true, .any = true}) == EPay::NothingOwed);
    CHECK(decide(EKind::Trip, {.at = 2, .exists = true, .foreign = false, .any = true}) == EPay::Pay);
    // Beside: its own windows there wait (the window it was made for still
    // lives); another app's make it a desktop of its own
    CHECK(decide(EKind::Beside, {.at = 3, .exists = true, .foreign = false, .any = true}) == EPay::WaitWindows);
    CHECK(decide(EKind::Beside, {.at = 3, .exists = true, .foreign = true, .any = true}) == EPay::NothingOwed);
    CHECK(decide(EKind::Beside, {.at = 3, .exists = true, .onScreen = true}) == EPay::WaitScreen);
    CHECK(decide(EKind::Beside, {.at = 3}) == EPay::Pay);
    // on screen or sliding: wait, either kind
    CHECK(decide(EKind::Trip, {.at = 2, .exists = true, .onScreen = true}) == EPay::WaitScreen);
    CHECK(decide(EKind::Trip, {.at = 2, .exists = true, .driving = true}) == EPay::WaitScreen);
    CHECK(decide(EKind::Trip, {.at = 2, .driving = true}) == EPay::WaitScreen);
    // a fullscreen app in the place: its trip owes it now
    CHECK(decide(EKind::Trip, {.at = 2, .exists = true, .any = true, .trip = true}) == EPay::ToTrip);
    CHECK(decide(EKind::Beside, {.at = 2, .exists = true, .any = true, .trip = true}) == EPay::ToTrip);
    // start: alone or stay settles; next makes room only when not free; free walks
    const auto NONE_FREE = [](Id) { return false; };
    const auto ALL_FREE  = [](Id) { return true; };
    CHECK(start("next", 1, 0, ALL_FREE).settle);
    CHECK(start("stay", 1, 3, ALL_FREE).settle);
    CHECK(!start("next", 1, 2, ALL_FREE).makeRoom && start("next", 1, 2, ALL_FREE).at == 2);
    CHECK(start("next", 1, 2, NONE_FREE).makeRoom && start("next", 1, 2, NONE_FREE).at == 2);
    CHECK(start("free", 1, 2, [](Id i) { return i == 5; }).at == 5);
    CHECK(!start("free", 1, 2, [](Id i) { return i == 5; }).makeRoom);
    CHECK(beside(2, NONE_FREE).at == 3 && beside(2, NONE_FREE).makeRoom);
    CHECK(!beside(2, ALL_FREE).makeRoom);
    // ON A ROW (block 1, 11..20 + overflow): every slot's trip has a place
    // after it in the row -- the 10th's is the first overflow place, never
    // 21 (the next block's); a visitor (outside the row) settles
    {
        const Hyprdrive::Row B1 = Hyprdrive::Row::block(1, 10);
        for (Id s = 1; s <= 12; ++s) {
            const Id HOME = B1.idOf(s), AT = start("next", B1, HOME, 1, NONE_FREE).at;
            CHECK(AT == B1.idOf(s + 1) && B1.contains(AT) && start("next", B1, HOME, 1, NONE_FREE).makeRoom);
        }
        CHECK(start("next", B1, 20, 1, ALL_FREE).at == 1'001'001);
        CHECK(start("next", B1, 25, 1, ALL_FREE).settle);
        CHECK(start("free", B1, 19, 1, [](Id i) { return i == 1'001'002; }).at == 1'001'002);
        CHECK(beside(B1, 20, NONE_FREE).at == 1'001'001);
        CHECK(beside(B1, 5, NONE_FREE).settle);
        // kept numbers move with their own row only
        CHECK(movedPlace(B1, 15, 13, +1) == 16 && movedPlace(B1, 20, 13, +1) == 1'001'001);
        CHECK(movedPlace(B1, 1'001'001, 16, -1) == 20 && movedPlace(B1, 12, 13, +1) == 12);
        CHECK(movedPlace(B1, 21, 13, +1) == 21 && movedPlace(B1, 1'002'001, 13, +1) == 1'002'001);
        CHECK(movedPlace(Hyprdrive::Row{}, 21, 13, +1) == 22); // the implicit row: the number line
    }
}


// ---------------------------------------------------------------------------
// THE SIMULATOR. A small world of desktops (number, monitor, windows) that
// behaves like Hyprland where it matters -- an empty desktop goes the moment
// no monitor shows it; a monitor plugged in shows a new empty desktop at the
// lowest free number; a monitor unplugged hands its desktops to the other --
// and FullscreenDesktop.cpp's trips, debts and reconcile pass, step for step,
// through the same FullscreenBook / DesktopPlan decisions. A sequence that
// ends with no fullscreen windows, nothing sliding and nothing owed on screen
// must give back exactly the desktop numbers it started with, and no two
// desktops may share a number at any step. With display changes in the mix
// (the monitor config changed, a monitor came or went: compact()), the
// desktops with windows are 1..N right after each compaction and still 1..N
// once everything is over -- no hole comes back from a stale debt.
namespace sim {
    using Hyprdrive::DesktopPlan::Key;
    namespace Plan = Hyprdrive::DesktopPlan;

    struct SDesk {
        Key key;
        Id  id;
        int mon;
    };
    struct SWin {
        int  id;
        Key  desk;
        int  cls;
        bool fs = false, alive = true, original = false;
    };
    struct SMonRef {
        int mon = -1, gen = 0;
    };
    struct STrip {
        int     win;
        Key     home, space;
        SMonRef mon;
        Id      at;
        bool    owes;
        Id      homeAt;
    };
    struct SDebt {
        Key     space;
        Id      at;
        SMonRef mon;
        Key     home;
        int     cls;
        bool    beside;
    };

    struct World {
        std::vector<SDesk> desks;
        std::vector<SWin>  wins;
        Key                active[2] = {0, 0};
        bool               up[2]     = {true, true};
        int                gen[2]    = {0, 0};
        bool               driving   = false;
        Key                nextKey   = 1;
        std::vector<STrip> trips;
        std::vector<SDebt> debts;
        std::vector<SDebt> given; // beside places given while free (FullscreenDesktop's g_given)
        std::vector<int>   settled;
        bool               compactDue = false; // a display change, waiting for the strip
        int                compactions = 0;
        bool               homeLost    = false; // a trip's home had gone and the compaction closed its place
        bool               displayCompacts = false; // a monitor coming or going compacts too (the plugin's hooks)
        std::string        mode = "next";
        bool               broke = false; // an invariant failed (reported once)
        std::vector<std::string> log;     // what happened, for a failing round
        std::string state() {
            std::string o;
            for (const auto& D : desks) {
                o += " " + std::to_string(D.id) + "[m" + std::to_string(D.mon) + (onScreen(D.key) ? "*" : "") + (isTripSpace(D.key) ? "T" : "") + ":";
                for (const auto& W : wins)
                    if (W.alive && W.desk == D.key)
                        o += std::to_string(W.id) + (W.fs ? "f" : "") + ",";
                o += "]";
            }
            o += " debts:";
            for (const auto& D : debts)
                o += " " + std::to_string(placeOf(D)) + (D.beside ? "b" : "t");
            return o;
        }
        void note(const std::string& what) {
            log.push_back(what + " ->" + state());
        }

        SDesk* desk(Key k) {
            for (auto& D : desks)
                if (D.key == k)
                    return &D;
            return nullptr;
        }
        SDesk* byId(Id id) {
            for (auto& D : desks)
                if (D.id == id)
                    return &D;
            return nullptr;
        }
        SWin* win(int id) {
            for (auto& W : wins)
                if (W.id == id && W.alive)
                    return &W;
            return nullptr;
        }
        bool monOk(const SMonRef& r) const {
            return r.mon >= 0 && up[r.mon] && gen[r.mon] == r.gen;
        }
        SMonRef ref(int mon) const {
            return {mon, gen[mon]};
        }
        int othersOn(Key d, int except) {
            int n = 0;
            for (const auto& W : wins)
                if (W.alive && W.desk == d && W.id != except)
                    ++n;
            return n;
        }
        bool onScreen(Key d) const {
            return (up[0] && active[0] == d) || (up[1] && active[1] == d);
        }
        Key create(Id id, int mon) {
            desks.push_back({nextKey, id, mon});
            return nextKey++;
        }
        // Hyprland: an empty desktop nothing shows is gone.
        void gc() {
            std::erase_if(desks, [&](const SDesk& D) { return othersOn(D.key, -1) == 0 && !onScreen(D.key); });
        }
        void checkUnique(const char* when) {
            std::set<Id> seen;
            for (const auto& D : desks)
                if (D.id < 1 || !seen.insert(D.id).second) {
                    if (!broke)
                        std::printf("  two desktops on %lld (or below 1) after %s\n", D.id, when);
                    broke = true;
                }
        }

        // DesktopOrder
        std::vector<Plan::Desk> plan() const {
            std::vector<Plan::Desk> out;
            for (const auto& D : desks)
                out.push_back({D.key, D.id, 0});
            return out;
        }
        bool renumber(const std::vector<Plan::Move>& moves) {
            if (Plan::check(plan(), moves) != Plan::Verdict::Ok)
                return false;
            for (const auto& M : moves)
                desk(M.key)->id = M.to;
            syncPlaces(); // the after-renumber hook
            return true;
        }
        bool shift(Id from, int delta) {
            return renumber(Plan::shiftMoves(plan(), from, delta));
        }

        // FullscreenDesktop.cpp
        template <class T>
        Id placeOf(const T& t) {
            const auto* S = desk(t.space);
            return Hyprdrive::FullscreenBook::placeOf(!!S, S ? S->id : NO_PLACE, t.at);
        }
        void syncPlaces() {
            for (auto& T : trips) {
                T.at                = placeOf(T);
                const auto* H       = desk(T.home);
                T.homeAt            = Hyprdrive::FullscreenBook::placeOf(!!H, H ? H->id : NO_PLACE, T.homeAt);
            }
            for (auto& D : debts)
                D.at = placeOf(D);
        }
        void moved(Id from, int delta) {
            for (auto& T : trips) {
                if (!desk(T.space))
                    T.at = movedPlace(T.at, from, delta);
                if (!desk(T.home))
                    T.homeAt = movedPlace(T.homeAt, from, delta);
            }
            for (auto& D : debts)
                if (!desk(D.space))
                    D.at = movedPlace(D.at, from, delta);
        }
        bool isTripSpace(Key d) {
            return d && std::ranges::any_of(trips, [&](const STrip& t) { return t.space == d; });
        }
        bool freePlace(Id id, int mon) {
            if (std::ranges::any_of(trips, [&](const STrip& t) {
                    const auto* H = desk(t.home);
                    return Hyprdrive::FullscreenBook::placeOf(!!H, H ? H->id : NO_PLACE, t.homeAt) == id;
                }))
                return false;
            const auto* E = byId(id);
            return !E || (E->mon == mon && othersOn(E->key, -1) == 0 && !isTripSpace(E->key));
        }
        STrip* tripOf(int w) {
            for (auto& T : trips)
                if (T.win == w)
                    return &T;
            return nullptr;
        }
        bool isSettled(int w) {
            return std::ranges::find(settled, w) != settled.end();
        }
        void show(const SMonRef& m, Key d) {
            if (monOk(m) && desk(d))
                active[m.mon] = d;
        }

        void startTrip(SWin& w) {
            const Key  HOME = w.desk;
            const int  MON  = desk(HOME)->mon;
            const auto PLAN = start(mode, desk(HOME)->id, othersOn(HOME, w.id), [&](Id id) { return freePlace(id, MON); });
            if (PLAN.settle) {
                settled.push_back(w.id);
                return;
            }
            const Id at   = PLAN.at;
            bool     owes = false;
            if (PLAN.makeRoom) {
                if (!shift(at, +1)) {
                    settled.push_back(w.id);
                    return;
                }
                moved(at, +1);
                owes = true;
            }
            if (mode != "free")
                if (const auto IT = std::ranges::find_if(debts, [&](const SDebt& d) { return placeOf(d) == at; }); IT != debts.end()) {
                    owes = true;
                    debts.erase(IT);
                }
            Key space = byId(at) ? byId(at)->key : create(at, MON);
            trips.push_back({w.id, HOME, space, ref(MON), at, owes, desk(HOME)->id});
            show(ref(MON), space);
            w.desk = space; // carry, fullscreen kept
        }
        void endTrip(const STrip& t, int cls) {
            if (t.owes)
                debts.push_back({t.space, placeOf(t), t.mon, t.home, cls, false});
        }
        void goHome(const STrip& t, SWin& w) {
            Key home = desk(t.home) ? t.home : 0;
            if (!home) {
                const int HMON = monOk(t.mon) ? t.mon.mon : (desk(t.space) ? desk(t.space)->mon : -1);
                if (t.homeAt >= 1 && HMON >= 0) {
                    if (auto* E = byId(t.homeAt); !E)
                        home = create(t.homeAt, HMON);
                    else if (othersOn(E->key, -1) == 0 && !isTripSpace(E->key))
                        home = E->key;
                }
                if (!home)
                    return;
                for (auto& T : trips)
                    if (!desk(T.home) && T.homeAt == t.homeAt)
                        T.home = home;
            }
            if (monOk(t.mon) && active[t.mon.mon] == t.space && desk(home)->mon == t.mon.mon)
                show(t.mon, home);
            w.desk = home;
        }
        enum class EPaid { Done, WaitScreen, WaitWindows };
        EPaid pay(const SDebt& d) {
            const Id AT = placeOf(d);
            auto*    E  = AT >= 1 ? byId(AT) : nullptr;
            SFacts   f{.at = AT, .exists = !!E, .driving = driving};
            bool     own = false;
            if (E) {
                for (const auto& W : wins)
                    if (W.alive && W.desk == E->key) {
                        f.any = true;
                        (W.cls == d.cls ? own : f.foreign) = true;
                    }
                f.trip     = isTripSpace(E->key);
                f.onScreen = onScreen(E->key);
                f.homeGone = !desk(d.home);
            }
            switch (decide(d.beside ? EKind::Beside : EKind::Trip, f)) {
                case EPay::Refuse:
                case EPay::NothingOwed: return EPaid::Done;
                case EPay::ToTrip:
                    for (auto& T : trips)
                        if (T.space == E->key)
                            T.owes = true;
                    return EPaid::Done;
                case EPay::WaitScreen: return EPaid::WaitScreen;
                case EPay::WaitWindows: return EPaid::WaitWindows;
                case EPay::Pay: break;
            }
            if (E) {
                const Key EK = E->key;
                if (own && desk(d.home) && d.home != EK)
                    for (auto& W : wins)
                        if (W.alive && W.desk == EK)
                            W.desk = d.home;
                if (!renumber(Plan::payMoves(plan(), AT, EK)))
                    return EPaid::Done;
            } else if (!shift(AT + 1, -1))
                return EPaid::Done;
            moved(AT + 1, -1);
            return EPaid::Done;
        }
        void besideFullscreen(SWin& w) {
            const Key WS  = w.desk;
            int       app = -1;
            for (const auto& W : wins)
                if (W.alive && W.fs && W.desk == WS && W.id != w.id)
                    app = W.id;
            if (app < 0 || !(tripOf(app) || isSettled(app)) || mode == "stay")
                return;
            const int  MON  = desk(WS)->mon;
            const auto PLAN = beside(desk(WS)->id, [&](Id id) { return freePlace(id, MON); });
            if (PLAN.settle)
                return;
            const Id at   = PLAN.at;
            bool     owes = false;
            if (PLAN.makeRoom) {
                if (!shift(at, +1))
                    return;
                moved(at, +1);
                owes = true;
            }
            if (const auto IT = std::ranges::find_if(debts, [&](const SDebt& d) { return placeOf(d) == at; }); IT != debts.end()) {
                owes = true;
                debts.erase(IT);
            }
            const Key space = byId(at) ? byId(at)->key : create(at, MON);
            if (owes)
                debts.push_back({space, at, ref(MON), WS, w.cls, true});
            else {
                std::erase_if(given, [&](const SDebt& g) { return g.space == space; }); // the latest window there
                given.push_back({space, at, ref(MON), WS, w.cls, true});
            }
            w.desk = space;
            show(ref(MON), space);
        }
        // FullscreenDesktop's compactIfDue, step for step.
        void compact() {
            if (!compactDue || driving)
                return;
            compactDue = false;
            renumber(Plan::healMoves(plan()));
            std::erase_if(given, [&](const SDebt& g) { return !desk(g.space); });
            syncPlaces();
            std::vector<Id> debtWas, tripWas, homeWas, givenWas;
            for (const auto& D : debts)
                debtWas.push_back(placeOf(D));
            for (const auto& T : trips) {
                tripWas.push_back(placeOf(T));
                const auto* H = desk(T.home);
                homeWas.push_back(Hyprdrive::FullscreenBook::placeOf(!!H, H ? H->id : NO_PLACE, T.homeAt));
            }
            for (const auto& G : given)
                givenWas.push_back(placeOf(G));
            std::vector<Plan::Room> rooms;
            for (const auto& D : desks)
                rooms.push_back({D.key, D.id, othersOn(D.key, -1) > 0, onScreen(D.key)});
            const auto MOVES = Plan::compactMoves(rooms);
            if (MOVES.empty())
                return;
            std::vector<std::pair<Id, Id>> row;
            for (const auto& R : rooms)
                if (R.populated) {
                    Id to = R.id;
                    for (const auto& M : MOVES)
                        if (M.key == R.key)
                            to = M.to;
                    row.emplace_back(R.id, to);
                }
            if (!renumber(MOVES)) {
                if (!broke)
                    std::printf("  a compaction was refused\n");
                broke = true;
                return;
            }
            ++compactions;
            note("  (compacted)");
            std::vector<SDebt> kept;
            for (size_t i = 0; i < debts.size(); ++i)
                if (const Id NOW = compactedPlace(debtWas[i], row); NOW != NO_PLACE) {
                    kept.push_back(debts[i]);
                    kept.back().at = NOW;
                }
            debts = std::move(kept);
            for (size_t i = 0; i < trips.size(); ++i) {
                const Id NOW  = compactedPlace(tripWas[i], row);
                trips[i].owes = NOW != NO_PLACE;
                trips[i].at   = NOW;
                if (!desk(trips[i].home)) {
                    trips[i].homeAt = compactedPlace(homeWas[i], row);
                    homeLost |= trips[i].homeAt == NO_PLACE;
                }
            }
            for (size_t i = 0; i < given.size(); ++i)
                if (const Id NOW = compactedPlace(givenWas[i], row); NOW != NO_PLACE && std::ranges::none_of(debts, [&](const SDebt& d) { return d.at == NOW; })) {
                    debts.push_back(given[i]);
                    debts.back().at = NOW;
                }
            given.clear();
            syncPlaces();
            // Right after: the desktops with windows are exactly 1..N, the
            // shown empty ones above them.
            std::set<Id> pop, want;
            Id           n = 0;
            for (const auto& D : desks)
                if (othersOn(D.key, -1) > 0)
                    pop.insert(D.id), want.insert(++n);
            for (const auto& D : desks)
                if (othersOn(D.key, -1) == 0 && D.id <= n) {
                    if (!broke)
                        std::printf("  an empty desktop at %lld below the top after a compaction\n", D.id);
                    broke = true;
                }
            if (pop != want) {
                if (!broke)
                    std::printf("  the desktops with windows are not 1..N after a compaction\n");
                broke = true;
            }
            checkUnique("compact");
        }
        void reconcile() {
            std::erase_if(settled, [&](int id) { const auto* W = win(id); return !W || !W->fs; });
            for (auto it = trips.begin(); it != trips.end();) {
                const STrip T = *it;
                SWin*       W = win(T.win);
                if (W && desk(T.space) && W->desk == T.space && W->fs) {
                    ++it;
                    continue;
                }
                it        = trips.erase(it);
                int cls   = -1;
                if (!W) {
                    if (monOk(T.mon) && desk(T.home) && active[T.mon.mon] == T.space && desk(T.home)->mon == T.mon.mon)
                        show(T.mon, T.home);
                } else if (desk(T.space) && W->desk == T.space)
                    goHome(T, *W);
                else if (desk(W->desk)) {
                    if (W->fs && othersOn(W->desk, W->id) > 0)
                        W->fs = false;
                    if (W->fs)
                        settled.push_back(W->id);
                    if (monOk(T.mon) && active[T.mon.mon] == T.space && desk(W->desk)->mon == T.mon.mon)
                        show(T.mon, W->desk);
                }
                if (W)
                    cls = W->cls;
                else
                    for (const auto& X : wins)
                        if (X.id == T.win)
                            cls = X.cls;
                endTrip(T, cls);
            }
            for (size_t i = 0; i < wins.size(); ++i) {
                auto& W = wins[i];
                if (W.alive && W.fs && desk(W.desk) && !tripOf(W.id) && !isSettled(W.id))
                    startTrip(W);
            }
            compact();
            syncPlaces();
            for (auto it = debts.begin(); it != debts.end();)
                it = pay(*it) == EPaid::Done ? debts.erase(it) : std::next(it);
            gc();
            checkUnique("reconcile");
            // In `next`, a fullscreen app on a trip is alone on its desktop.
            if (mode == "next")
                for (const auto& T : trips)
                    if (othersOn(T.space, T.win) > 0) {
                        if (!broke)
                            std::printf("  a fullscreen app shares desktop %lld\n", desk(T.space) ? desk(T.space)->id : -1);
                        broke = true;
                    }
        }

        // Events
        int  nextWin = 1;
        void on(int id) {
            if (auto* W = win(id); W && !W->fs) {
                W->fs = true;
                reconcile();
                note("on " + std::to_string(id));
            }
        }
        void off(int id) {
            if (auto* W = win(id); W && W->fs) {
                W->fs = false;
                reconcile();
                note("off " + std::to_string(id));
            }
        }
        int open(Key d, int cls) {
            if (!desk(d))
                return -1;
            wins.push_back({nextWin++, d, cls});
            besideFullscreen(wins.back());
            gc();
            reconcile();
            note("open " + std::to_string(wins.back().id));
            return wins.back().id;
        }
        void close(int id) {
            if (auto* W = win(id); W && !W->original) {
                W->alive = false;
                gc();
                reconcile();
                note("close " + std::to_string(id));
            }
        }
        void send(int id, Key d) {
            if (auto* W = win(id); W && desk(d) && d != W->desk) {
                W->desk = d;
                gc();
                reconcile();
                note("send " + std::to_string(id));
            }
        }
        void view(Key d) {
            if (const auto* D = desk(d); D && up[D->mon]) {
                active[D->mon] = d;
                gc();
                reconcile();
                note("view");
            }
        }
        void unplug(int m) {
            const int other = 1 - m;
            if (!up[m] || !up[other])
                return;
            for (auto& D : desks)
                if (D.mon == m)
                    D.mon = other;
            up[m]     = false;
            active[m] = 0;
            compactDue |= displayCompacts;
            gc();
            reconcile();
            note("unplug");
        }
        void plug(int m) {
            if (up[m])
                return;
            up[m] = true;
            ++gen[m];
            Id id = 1;
            while (byId(id))
                ++id;
            active[m] = create(id, m); // findAvailableDefaultWS
            compactDue |= displayCompacts;
            reconcile();
            note("plug");
        }
        // A monitor's mode / scale / layout changed (nothing else moves).
        void display() {
            compactDue = true;
            reconcile();
            note("display");
        }
        void drive(bool on) {
            driving = on;
            reconcile();
            note(on ? "drive" : "rest");
        }

        std::set<Id> populated() {
            std::set<Id> out;
            for (const auto& D : desks)
                if (othersOn(D.key, -1) > 0)
                    out.insert(D.id);
            return out;
        }
        // Done: nothing fullscreen, extras closed, the strip at rest, the
        // second monitor (if it came back) unplugged, each monitor on a
        // populated desktop -- then the lazy paybacks are due.
        void settle() {
            for (auto& W : wins)
                if (W.alive && W.fs)
                    off(W.id);
            for (auto& W : wins)
                if (W.alive && !W.original)
                    close(W.id);
            drive(false);
            for (int m = 0; m < 2; ++m)
                for (const auto& D : desks)
                    if (D.mon == m && up[m] && othersOn(D.key, -1) > 0) {
                        view(D.key);
                        break;
                    }
            for (int i = 0; i < 4; ++i)
                reconcile();
        }
    };

    // Desktop 1 with `onOne` windows, the given others populated, monitor 0
    // showing desktop 1; desktop 9 on monitor 1 when `two`.
    World make(int onOne, std::vector<Id> others, bool two) {
        World w;
        const Key ONE = w.create(1, 0);
        for (int i = 0; i < onOne; ++i)
            w.wins.push_back({w.nextWin++, ONE, 100 + i, false, true, true});
        for (const Id I : others) {
            const Key K = w.create(I, 0);
            w.wins.push_back({w.nextWin++, K, 200 + int(I), false, true, true});
        }
        w.active[0] = ONE;
        if (two) {
            const Key K = w.create(9, 1);
            w.wins.push_back({w.nextWin++, K, 900, false, true, true});
            w.active[1] = K;
        } else {
            w.up[1] = false;
        }
        return w;
    }
}

static void fiftyCycles() {
    // 50 fullscreen on/off cycles on desktop 1 (two windows), 2 and 3 populated:
    // at rest after each (~1 Hz), and spammed (the strip still sliding the whole
    // time, so every payback waits and each new trip takes the old debt over).
    for (const bool SPAM : {false, true})
        for (const bool HOLES : {false, true}) {
            auto       w     = sim::make(2, HOLES ? std::vector<Id>{4, 7} : std::vector<Id>{2, 3}, false);
            const auto START = w.populated();
            const int  APP   = w.wins[0].id;
            if (SPAM)
                w.drive(true);
            for (int i = 0; i < 50; ++i) {
                w.on(APP);
                w.off(APP);
                if (!SPAM)
                    w.reconcile();
            }
            w.settle();
            CHECK(!w.broke);
            CHECK(w.populated() == START);
            CHECK(w.debts.empty() && w.trips.empty());
        }
}

static void closedWhileAway() {
    // The "-1" case: an app on its trip is closed while its desktop is not on
    // screen (its desktop goes at once); the debt still knows its place.
    auto       w     = sim::make(3, {2, 3}, false);
    const auto START = w.populated();
    const int  APP   = w.open(w.wins[0].desk, 555); // an extra window on 1
    w.on(APP);
    CHECK(w.trips.size() == 1 && w.byId(2) && w.byId(2)->key == w.trips[0].space);
    w.view(w.byId(1)->key); // look away: the trip's desktop is not on screen
    w.close(APP);           // ...and close it from there
    CHECK(w.desks.size() == 3);
    w.settle();
    CHECK(!w.broke);
    CHECK(w.populated() == START);
}

static void besideThenClosed() {
    // A window opened over a fullscreen app gets the next place (made: 3 is
    // populated), then closes: its place is paid back.
    auto       w     = sim::make(2, {2, 3}, false);
    const auto START = w.populated();
    const int  APP   = w.wins[0].id;
    w.on(APP); // to 2, 2 and 3 -> 3 and 4
    const int B = w.open(w.win(APP)->desk, 777);
    CHECK(w.win(B) && w.desk(w.win(B)->desk)->id == 3);
    CHECK(w.byId(5)); // the old 3 moved twice
    w.close(B);
    w.off(APP);
    w.settle();
    CHECK(!w.broke);
    CHECK(w.populated() == START);
}

static void monitorGoneMidTrip() {
    // A trip on monitor 1; monitor 1 unplugged; out of fullscreen on monitor 0;
    // monitor 1 back. The old code dropped the debt (its monitor gone).
    auto       w = sim::make(1, {2, 3}, true);
    const auto NINE = w.byId(9)->key;
    const int  A    = w.open(NINE, 901); // desktop 9 holds two windows
    const auto START = w.populated();
    w.on(A); // 9 -> trip at 10
    CHECK(w.trips.size() == 1);
    w.unplug(1);
    w.off(A);
    w.plug(1);
    w.settle();
    CHECK(!w.broke);
    CHECK(w.populated() == START);
    (void)NINE;
}

static void randomSequences(bool display) {
    std::mt19937 rng(display ? 20261112 : 20261111);
    int          bad = 0;
    for (int round = 0; round < 10000; ++round) {
        std::vector<Id> others;
        for (Id i = 2; i <= 6; ++i)
            if (rng() % 2)
                others.push_back(i);
        auto       w     = sim::make(2 + rng() % 2, others, rng() % 2);
        w.mode           = (rng() % 5 == 0) ? "free" : "next";
        w.displayCompacts = display;
        const auto START = w.populated();
        const int  STEPS = 5 + rng() % 60;
        for (int s = 0; s < STEPS; ++s) {
            // (Windows are opened and sent only onto desktops an original
            // window populates: one put on an owed place makes it a desktop of
            // its own by design -- nothing owed -- and closing it at the end
            // would leave the hole that rule means to leave.)
            std::vector<int> alive, fs, extras, originals;
            for (const auto& W : w.wins)
                if (W.alive) {
                    alive.push_back(W.id);
                    if (W.original)
                        originals.push_back(W.id);
                    if (W.fs)
                        fs.push_back(W.id);
                    if (!W.original)
                        extras.push_back(W.id);
                }
            const auto pick = [&](const std::vector<int>& v) { return v[rng() % v.size()]; };
            switch (rng() % (display ? 14 : 12)) {
                case 12:
                case 13: w.display(); break;
                case 0:
                case 1:
                case 2: w.on(pick(alive)); break;
                case 3:
                case 4:
                    if (!fs.empty())
                        w.off(pick(fs));
                    break;
                case 5:
                    // a window opened over a fullscreen app (its class, or another)
                    if (!fs.empty()) {
                        const auto* A = w.win(pick(fs));
                        w.open(A->desk, rng() % 2 ? A->cls : 1000 + s);
                    }
                    break;
                case 6:
                    // a plain window opened on a populated desktop
                    {
                        const auto* A = w.win(pick(originals));
                        if (!w.isTripSpace(A->desk))
                            w.open(A->desk, 2000 + s);
                    }
                    break;
                case 7:
                    if (!extras.empty())
                        w.close(pick(extras));
                    break;
                case 8: w.drive(rng() % 2); break;
                case 9:
                    // look at some desktop
                    if (!w.desks.empty())
                        w.view(w.desks[rng() % w.desks.size()].key);
                    break;
                case 10:
                    // a fullscreen app (one opened during the run: moving an
                    // original window would change which desktops are
                    // populated by itself) sent to another populated desktop
                    if (!w.trips.empty()) {
                        const auto& T = w.trips[rng() % w.trips.size()];
                        const auto* A = w.win(T.win);
                        const auto* B = w.win(pick(originals));
                        if (A && !A->original && B && B->desk != T.space && w.othersOn(B->desk, -1) > 0 && !w.isTripSpace(B->desk))
                            w.send(T.win, B->desk);
                    }
                    break;
                case 11:
                    if (w.up[1] && w.up[0])
                        w.unplug(1);
                    else
                        w.plug(1);
                    break;
            }
            if (w.broke)
                break;
        }
        if (w.up[1] && w.up[0])
            w.unplug(1);
        w.settle();
        // A display change: the desktops with windows end as 1..N (no hole came
        // back below the highest because of a stale debt); else exactly where
        // they started.
        // (A trip whose home had gone -- every other window there closed --
        // and whose home's place the compaction closed stays on its own
        // desktop when it leaves fullscreen: that desktop is its own now, and
        // closing its last window leaves the hole any closed desktop leaves.
        // Only the step invariants hold there.)
        std::set<Id> want = START;
        if (w.compactions > 0) {
            want.clear();
            for (Id i = 1; i <= Id(START.size()); ++i)
                want.insert(i);
        }
        const bool OK = !w.broke && (w.homeLost || w.populated() == want) && w.trips.empty() && w.debts.empty();
        if (!OK && ++bad <= 3) {
            std::printf("  round %d (%s): started", round, w.mode.c_str());
            for (const Id I : START)
                std::printf(" %lld", I);
            std::printf(", ended");
            for (const Id I : w.populated())
                std::printf(" %lld", I);
            std::printf(", %zu debts\n", w.debts.size());
            if (bad == 1)
                for (const auto& L : w.log)
                    std::printf("    %s\n", L.c_str());
        }
        CHECK(OK);
    }
}

static void displayMidTrip() {
    // compactedPlace: a place in the row follows its desktop; a closed one is no place.
    CHECK(compactedPlace(28, {{21, 1}, {28, 3}}) == 3);
    CHECK(compactedPlace(23, {{21, 1}, {28, 3}}) == NO_PLACE);
    CHECK(compactedPlace(NO_PLACE, {{21, 1}}) == NO_PLACE);
    CHECK(compactedPlace(2, {}) == NO_PLACE);
    // The report: desktops at 21, 22, 28, 29, 30 -- then the display changes.
    {
        auto       w     = sim::make(1, {22, 28, 29, 30}, false);
        w.desk(w.byId(1)->key)->id = 21;
        w.display();
        CHECK(!w.broke && w.populated() == (std::set<Id>{1, 2, 3, 4, 5}));
        CHECK(w.onScreen(w.byId(1)->key));
    }
    // A trip in flight (owed): 1 has two windows, 2 and 7; the app on 2, 2 and 7 -> 3 and 8.
    // The display changes: 1, 2 (trip), 3, 4. Off: 2 is paid back -> 1, 2, 3.
    {
        auto      w   = sim::make(2, {2, 7}, false);
        const int APP = w.wins[0].id;
        w.on(APP);
        CHECK(w.trips.size() == 1 && w.byId(8));
        w.display();
        CHECK(!w.broke && w.populated() == (std::set<Id>{1, 2, 3, 4}) && w.trips.size() == 1 && w.trips[0].owes);
        w.off(APP);
        w.settle();
        CHECK(!w.broke && w.populated() == (std::set<Id>{1, 2, 3}) && w.debts.empty());
    }
    // A trip on a place that was free (nothing owed): 1 (two windows), 5. The app on 2.
    // The display changes: 1, 2 (trip), 3 -- the trip owes its place now, so
    // when it is done 5's desktop comes back to 2, not left at 3 with a hole.
    {
        auto      w   = sim::make(2, {5}, false);
        const int APP = w.wins[0].id;
        w.on(APP);
        CHECK(w.trips.size() == 1 && !w.trips[0].owes);
        w.display();
        CHECK(w.trips.size() == 1 && w.trips[0].owes);
        w.off(APP);
        w.settle();
        CHECK(!w.broke && w.populated() == (std::set<Id>{1, 2}) && w.debts.empty());
    }
    // A debt whose place is a hole the compaction closes is settled: closed while away.
    // 1 (three windows), 2, 3; the app on 2 (2, 3 -> 3, 4); look away, close it:
    // its desktop goes, place 2 is owed while the strip slides. The display
    // changes: 1, 2, 3 -- the debt is settled there, never paid again.
    {
        auto      w   = sim::make(3, {2, 3}, false);
        const int APP = w.open(w.wins[0].desk, 555);
        w.on(APP);
        w.drive(true);
        w.view(w.byId(1)->key);
        w.close(APP);
        CHECK(w.debts.size() == 1 && !w.byId(2));
        w.compactDue = true; // the display changed while the strip was sliding
        w.drive(false);      // it lands: the compaction first, then the debts
        CHECK(w.debts.empty() && w.compactions == 1);
        w.settle();
        CHECK(!w.broke && w.populated() == (std::set<Id>{1, 2, 3}));
    }
}

static void savedAcrossReload() {
    SSaved S;
    S.trips.push_back({.win = 0x55aa00, .home = 0x1000, .space = 0x2000, .mon = 0x3000, .at = 2, .homeAt = 1, .owes = true});
    S.trips.push_back({.win = 7, .at = 9, .homeAt = NO_PLACE});
    S.debts.push_back({.space = 0, .mon = 0x3000, .home = 0x1000, .at = 4, .pid = 4242, .beside = true, .cls = "org.herdr app\n\x01 \xff"});
    S.debts.push_back({.at = 3, .cls = ""});
    S.settled = {0x99, 0x98};
    const auto L = load(save(S));
    CHECK(L.has_value());
    if (L) {
        CHECK(L->trips.size() == 2 && L->trips[0].win == 0x55aa00 && L->trips[0].home == 0x1000 && L->trips[0].space == 0x2000 && L->trips[0].mon == 0x3000);
        CHECK(L->trips[0].at == 2 && L->trips[0].homeAt == 1 && L->trips[0].owes && !L->trips[1].owes && L->trips[1].homeAt == NO_PLACE);
        CHECK(L->debts.size() == 2 && L->debts[0].cls == S.debts[0].cls && L->debts[0].pid == 4242 && L->debts[0].beside && L->debts[0].at == 4);
        CHECK(L->debts[1].cls.empty() && !L->debts[1].beside);
        CHECK(L->settled == S.settled);
    }
    CHECK(load(save({})).has_value() && load(save({}))->trips.empty());
    CHECK(!load("").has_value());
    CHECK(!load("fsdesk0 T 1 2 3 4 5 6 1").has_value()); // another build's format
    CHECK(!load("fsdesk1 T 1 2").has_value());             // cut short
    CHECK(!load("fsdesk1 D 1 2 3 4 5 0 zz").has_value());  // bad class
    CHECK(!load("fsdesk1 X").has_value());
}

// THE BESIDE LINK (link()): every combination of its facts, against the rule
// in words -- and the way back only ever for a wanted link the user is
// looking at, emptied, with the app still fullscreen and nothing sliding.
static void besideLinks() {
    for (int m = 0; m < (1 << 9); ++m) {
        const SLinkFacts F{.appAlive = !!(m & 1), .appFullscreen = !!(m & 2), .spaceAlive = !!(m & 4), .any = !!(m & 8), .foreign = !!(m & 16),
                           .wanted = !!(m & 32), .viewOn = !!(m & 64), .driving = !!(m & 128), .settling = !!(m & 256)};
        const ELink L   = link(F);
        const bool  APP = F.appAlive && F.appFullscreen;
        ELink       want;
        if (!F.spaceAlive || F.foreign)
            want = ELink::Drop; // gone, or a desktop of its own now
        else if (F.any)
            want = APP ? ELink::Hold : ELink::Drop;
        else if (!APP)
            want = ELink::Drop; // nothing to go back to
        else if (F.settling || F.driving)
            want = ELink::Hold;
        else
            want = F.wanted && F.viewOn ? ELink::ReturnToApp : ELink::Drop;
        CHECK(L == want);
        if (L == ELink::ReturnToApp)
            CHECK(F.wanted && F.viewOn && !F.any && APP && !F.driving && !F.settling);
    }
    // In a row: the place after the app's, the 10th's next is the overflow,
    // never the next block; outside the row, none.
    const auto FREE = [](Id) { return true; };
    using Hyprdrive::Row;
    const Row  B1   = Row::block(1, 10);
    CHECK(beside(B1, 12, FREE).at == 13 && !beside(B1, 12, FREE).makeRoom);
    CHECK(beside(B1, 20, FREE).at == B1.overflow + 1);
    CHECK(beside(B1, 5, FREE).settle);
    CHECK(beside(Row{}, 3, FREE).at == 4);
    CHECK(beside(Row{}, 3, [](Id) { return false; }).makeRoom);
}

static void savedBesides() {
    SSaved S;
    S.besides.push_back({.app = 0x10, .appSpace = 0x20, .space = 0x30, .mon = 0x40, .at = 3, .owes = true, .wanted = false, .pid = 77, .cls = "a b"});
    S.besides.push_back({.app = 1, .at = 9, .wanted = true});
    const auto L = load(save(S));
    CHECK(L.has_value() && L->besides.size() == 2);
    if (L && L->besides.size() == 2) {
        CHECK(L->besides[0].app == 0x10 && L->besides[0].appSpace == 0x20 && L->besides[0].space == 0x30 && L->besides[0].mon == 0x40);
        CHECK(L->besides[0].at == 3 && L->besides[0].owes && !L->besides[0].wanted && L->besides[0].pid == 77 && L->besides[0].cls == "a b");
        CHECK(L->besides[1].wanted && !L->besides[1].owes && L->besides[1].cls.empty());
    }
    // The build before's handover still loads (no B lines in it)...
    const auto V1 = load("fsdesk1 T 1 2 3 4 5 6 1 S 9");
    CHECK(V1.has_value() && V1->trips.size() == 1 && V1->settled.size() == 1 && V1->besides.empty());
    // ...and a B line in an fsdesk1 one is malformed.
    CHECK(!load("fsdesk1 B 1 2 3 4 5 1 0 7 -").has_value());
    CHECK(!load("fsdesk2 B 1 2 3").has_value());
}

int main() {
    places();
    besideLinks();
    savedBesides();
    decisions();
    fiftyCycles();
    closedWhileAway();
    besideThenClosed();
    monitorGoneMidTrip();
    randomSequences(false);
    randomSequences(true);
    displayMidTrip();
    savedAcrossReload();
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
