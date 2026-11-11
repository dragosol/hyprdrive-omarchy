// Host test for PlacementModel.hpp: where a new window goes.
//   * every fact combination against the owner's rulings, one statement each;
//   * a property test: random open / float toggle / view switch / close
//     sequences over a fullscreen app, played through decide() and
//     FullscreenBook's beside / link / decide the way Placement and
//     FullscreenDesktop play them. Invariants after every step:
//       - no window floats where the enforcer is on without the user's
//         intent, a rule, the tag, or a Leave (or the budget's logged give-up);
//       - the fullscreen app never shares its desktop;
//     and once everything opened has closed: the desktops are the ones it
//     started with, and the view -- unless the user moved it -- is back on
//     the fullscreen app.
//   * the enforcement budget.
#include "PlacementModel.hpp"
#include "FullscreenBook.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <optional>
#include <random>
#include <vector>

using namespace Hyprdrive::PlacementModel;
namespace Book = Hyprdrive::FullscreenBook;

static int g_checks = 0, g_failed = 0;
#define CHECK(c)                                                                                                                                                                   \
    do {                                                                                                                                                                           \
        ++g_checks;                                                                                                                                                                \
        if (!(c)) {                                                                                                                                                                \
            ++g_failed;                                                                                                                                                            \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);                                                                                                               \
        }                                                                                                                                                                          \
    } while (0)

static SWindowFacts factsOf(unsigned m) {
    return {.placeable = !!(m & 1), .passing = !!(m & 2), .floating = !!(m & 4), .ruleFloat = !!(m & 8), .tagged = !!(m & 16), .fixedSize = !!(m & 32),
            .child = !!(m & 64), .modal = !!(m & 128)};
}

// Every combination, each ruling stated on its own.
static void rulings() {
    for (unsigned m = 0; m < 256; ++m)
        for (int i = 0; i < 3; ++i)
            for (unsigned p = 0; p < 16; ++p)
                for (int pr = 0; pr < 2; ++pr) {
                    const SWindowFacts W  = factsOf(m);
                    const EIntent      IN = EIntent(i);
                    const SPlaceFacts  P{.enforce = !!(p & 1), .fullscreenApp = !!(p & 2), .sameApp = !!(p & 4), .stay = !!(p & 8)};
                    const SOutcome     O = decide(W, IN, P, pr);
                    // Not placeable: never touched.
                    if (!W.placeable)
                        CHECK(O.kind == EOutcome::Leave);
                    // Beside over a fullscreen app, whatever the option, floats and dialogs included...
                    if (W.placeable && P.fullscreenApp && !P.stay && !(W.passing && P.sameApp))
                        CHECK(O.kind == EOutcome::Beside && O.prompted == !!pr);
                    // ...never under fullscreen_desktop = stay, never without a fullscreen app.
                    if (P.stay || !P.fullscreenApp)
                        CHECK(O.kind != EOutcome::Beside);
                    if (O.kind == EOutcome::Beside)
                        continue;
                    // Splash / palettes: as Hyprland places them.
                    if (W.placeable && W.passing)
                        CHECK(O.kind == EOutcome::Leave);
                    if (!W.placeable || W.passing)
                        continue;
                    // The user's float and the tag float, everywhere.
                    if (IN == EIntent::Float || W.tagged)
                        CHECK(O.kind == EOutcome::Float && O.why == EWhy::Intent);
                    // A float rule is honoured.
                    else if (IN == EIntent::None && W.ruleFloat)
                        CHECK(O.kind == EOutcome::Float && O.why == EWhy::Rule);
                    // An app's own float: tiled where the enforcer is on, left where it is off.
                    else if (IN == EIntent::None && W.floating)
                        CHECK(O.kind == (P.enforce ? EOutcome::Tile : EOutcome::Float));
                    // Tile: at its own size exactly when it fixes its size.
                    if (O.kind == EOutcome::Tile)
                        CHECK(O.ownSize == W.fixedSize);
                    // Dialog-ness (a parent, modal) decides nothing.
                    const SWindowFacts PLAIN = {.placeable = W.placeable, .passing = W.passing, .floating = W.floating, .ruleFloat = W.ruleFloat,
                                                .tagged = W.tagged, .fixedSize = W.fixedSize};
                    CHECK(decide(PLAIN, IN, P, pr) == O);
                    // The option off changes nothing but an app's float.
                    if (!(IN == EIntent::None && !W.tagged && !W.ruleFloat && W.floating))
                        CHECK(decide(W, IN, {.enforce = !P.enforce, .fullscreenApp = P.fullscreenApp, .sameApp = P.sameApp, .stay = P.stay}, pr) == O);
                }
}

// A small world: one monitor, the implicit row, a fullscreen app on a trip.
struct SWin {
    int          id;
    long long    desk;
    SWindowFacts facts;
    EIntent      intent = EIntent::None;
    bool         floating = false;
    bool         member   = false; // of the beside link
    bool         overApp  = false; // left on the app's desktop: a menu, the app's own splash
};
struct SLink {
    long long space;
    bool      wanted = false, owes = false;
};
struct SWorld {
    std::map<long long, bool> desks;     // id -> enforce there (Mosaic or the option)
    std::vector<SWin>         wins;      // the app is not one of them
    long long                 appDesk = 0;
    long long                 view    = 0;
    std::optional<SLink>      link;
    std::vector<long long>    debts; // beside debts' places
    std::map<long long, int>  base;  // other windows at the start, per desktop
    bool                      userMoved = false;
    int                       nextId    = 1;

    int on(long long d) const {
        int n = base.contains(d) ? base.at(d) : 0;
        for (const auto& W : wins)
            n += W.desk == d;
        return n;
    }
    bool freeAt(long long d) const {
        return !desks.contains(d) || (on(d) == 0 && d != appDesk);
    }
    void shift(long long from, int delta) {
        auto move = [&](long long& id) {
            if (id >= from)
                id += delta;
        };
        std::map<long long, bool> nd;
        for (auto [ID, E] : desks) {
            long long id = ID;
            move(id);
            nd[id] = E;
        }
        desks = nd;
        std::map<long long, int> nb;
        for (auto [ID, N] : base) {
            long long id = ID;
            move(id);
            nb[id] = N;
        }
        base = nb;
        for (auto& W : wins)
            move(W.desk);
        move(appDesk);
        move(view);
        if (link)
            move(link->space);
        for (auto& D : debts)
            move(D);
    }
    // Hyprland drops an empty desktop nobody shows.
    void dropEmpty() {
        std::erase_if(desks, [&](const auto& kv) { return kv.first != view && kv.first != appDesk && on(kv.first) == 0; });
    }
};

static void place(SWorld& w, std::mt19937& rng, bool prompted) {
    SWin win{.id = w.nextId++, .desk = w.view, .facts = factsOf(rng())};
    win.facts.placeable = rng() % 8 != 0;
    win.floating        = win.facts.floating || win.facts.ruleFloat || win.facts.tagged;
    const SPlaceFacts P{.enforce = w.desks[w.view], .fullscreenApp = w.view == w.appDesk, .sameApp = rng() % 2 == 0};
    SOutcome          O = decide(win.facts, win.intent, P, prompted);
    if (O.kind == EOutcome::Beside) {
        if (w.link && w.desks.contains(w.link->space))
            win.desk = w.link->space; // joins the first one's desktop
        else {
            const auto PLAN = Book::beside(Hyprdrive::Row{}, w.appDesk, [&](long long d) { return w.freeAt(d); });
            CHECK(!PLAN.settle);
            bool owes = false;
            if (PLAN.makeRoom) {
                w.shift(PLAN.at, +1);
                owes = true;
            }
            if (const auto IT = std::ranges::find(w.debts, PLAN.at); IT != w.debts.end()) {
                w.debts.erase(IT);
                owes = true;
            }
            w.desks[PLAN.at] = rng() % 2 == 0;
            w.link           = SLink{.space = PLAN.at, .owes = owes};
            win.desk         = PLAN.at;
        }
        win.member = true;
        if (O.prompted) {
            w.view         = win.desk;
            w.link->wanted = true;
        }
        // ...and decided again there: no fullscreen app on it.
        O = decide(win.facts, win.intent, {.enforce = w.desks[win.desk]}, prompted);
    }
    if (O.kind == EOutcome::Tile)
        win.floating = false;
    win.overApp = win.desk == w.appDesk;
    // (Only what may stay over it: never placeable, or the app's own passing window.)
    CHECK(!win.overApp || O.kind == EOutcome::Leave);
    w.wins.push_back(win);
}

static void reconcile(SWorld& w) {
    if (w.link) {
        const long long S = w.link->space;
        Book::SLinkFacts F{.appAlive = true, .appFullscreen = true, .spaceAlive = w.desks.contains(S), .any = w.on(S) > 0,
                           .wanted = w.link->wanted, .viewOn = w.view == S};
        const auto L = Book::link(F);
        if (L != Book::ELink::Hold) {
            if (L == Book::ELink::ReturnToApp)
                w.view = w.appDesk;
            if (w.link->owes)
                w.debts.push_back(S);
            w.link.reset();
        }
    }
    w.dropEmpty();
    for (auto it = w.debts.begin(); it != w.debts.end();) {
        const long long AT = *it;
        const bool      E  = w.desks.contains(AT);
        const auto      P  = Book::decide(Book::EKind::Beside, {.at = AT, .exists = E, .any = E && w.on(AT) > 0, .trip = AT == w.appDesk, .onScreen = w.view == AT});
        if (P == Book::EPay::Pay) {
            w.desks.erase(AT);
            w.shift(AT + 1, -1);
        }
        it = P == Book::EPay::WaitScreen || P == Book::EPay::WaitWindows ? std::next(it) : w.debts.erase(it);
    }
}

static void invariants(const SWorld& w) {
    for (const auto& W : w.wins) {
        CHECK(W.desk != w.appDesk || W.overApp); // the app alone on its desktop
        if (W.floating && w.desks.contains(W.desk) && w.desks.at(W.desk) && W.facts.placeable && !W.facts.passing)
            CHECK(W.intent == EIntent::Float || W.facts.tagged || W.facts.ruleFloat);
    }
}

static void properties() {
    std::mt19937 rng(20261111);
    for (int run = 0; run < 4000; ++run) {
        SWorld w;
        // 1: the app's home (with a window), 2: the app, maybe 3: another desktop.
        w.desks   = {{1, rng() % 2 == 0}, {2, rng() % 2 == 0}};
        w.base    = {{1, 1}};
        if (rng() % 2) {
            w.desks[3] = rng() % 2 == 0;
            w.base[3]  = 1;
        }
        w.appDesk         = 2;
        w.view            = 2;
        const auto DESKS0 = w.desks;
        const int  STEPS  = 1 + rng() % 24;
        for (int s = 0; s < STEPS; ++s) {
            switch (rng() % 6) {
                case 0:
                case 1: place(w, rng, rng() % 2 == 0); break;
                case 2:
                    if (!w.wins.empty())
                        w.wins.erase(w.wins.begin() + rng() % w.wins.size());
                    break;
                case 3:
                    // The user floats or tiles a window: his intent from now on.
                    if (!w.wins.empty()) {
                        auto& W    = w.wins[rng() % w.wins.size()];
                        W.floating = !W.floating;
                        W.intent   = W.floating ? EIntent::Float : EIntent::Tile;
                    }
                    break;
                case 4:
                    // The user goes to a desktop (the link's: he wants it now).
                    if (rng() % 3 == 0) {
                        auto it = w.desks.begin();
                        std::advance(it, rng() % w.desks.size());
                        w.view      = it->first;
                        w.userMoved = w.view != w.appDesk;
                        if (w.link && w.view == w.link->space)
                            w.link->wanted = true;
                    }
                    break;
                default: place(w, rng, false); break;
            }
            reconcile(w);
            invariants(w);
        }
        // Everything opened closes, one at a time.
        while (!w.wins.empty()) {
            w.wins.erase(w.wins.begin() + rng() % w.wins.size());
            reconcile(w);
            invariants(w);
        }
        // The view leaves wherever it is (debts wait for the screen), then:
        const long long VIEW = w.view;
        w.view               = w.appDesk;
        reconcile(w);
        std::vector<long long> got, want;
        for (const auto& [D, E] : w.desks)
            got.push_back(D);
        for (const auto& [D, E] : DESKS0)
            want.push_back(D);
        CHECK(got == want);
        CHECK(!w.link && w.debts.empty());
        if (!w.userMoved)
            CHECK(VIEW == w.appDesk);
    }
}

static void budget() {
    CBudget b;
    CHECK(b.spend(0) && b.spend(100));
    CHECK(!b.spend(200) && b.spent()); // a third within 3 s: given up for good
    CHECK(!b.spend(10000));
    CBudget slow;
    for (int i = 0; i < 10; ++i)
        CHECK(slow.spend(i * 3000)); // one every 3 s is never a loop
    // retile(): only an enforced app float, never the user's or a rule's.
    const SWindowFacts W{.floating = true};
    CHECK(retile(W, EIntent::None, {.enforce = true}));
    CHECK(!retile(W, EIntent::None, {.enforce = false}));
    CHECK(!retile(W, EIntent::Float, {.enforce = true}));
    CHECK(!retile({.floating = true, .ruleFloat = true}, EIntent::None, {.enforce = true}));
    CHECK(!retile({.passing = true, .floating = true}, EIntent::None, {.enforce = true}));
}

int main() {
    rulings();
    properties();
    budget();
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
