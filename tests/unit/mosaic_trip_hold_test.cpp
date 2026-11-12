// Host test for MosaicTripHold.hpp: a window home from its fullscreen trip goes
// back to its place only when nothing changed on its desktop meanwhile.
#include "MosaicTripHold.hpp"

#include <cstdio>
#include <optional>

using namespace Hyprdrive::MosaicTripHold;

static int g_checks = 0, g_failed = 0;
#define CHECK(c)                                                                                                                                                                   \
    do {                                                                                                                                                                           \
        ++g_checks;                                                                                                                                                                \
        if (!(c)) {                                                                                                                                                                \
            ++g_failed;                                                                                                                                                            \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);                                                                                                               \
        }                                                                                                                                                                          \
    } while (0)

// The desktop as Mosaic keeps it (MosaicParts.hpp holdForTrip / backFromTrip /
// resizeTarget), reduced to the order, the edit count and the hold.
struct SDesk {
    std::vector<SKey>    order;
    std::uint64_t        edits = 0;
    std::optional<SHold> hold;
    std::size_t          at(const void* who) const {
        for (std::size_t i = 0; i < order.size(); ++i)
            if (order[i].who == who)
                return i;
        return order.size();
    }
    // A window leaves: `trip` -- on its fullscreen trip.
    void leave(const SKey& k, bool trip) {
        if (isEdit(hold ? &*hold : nullptr, k.app))
            ++edits;
        const std::size_t I = at(k.who);
        if (I == order.size())
            return;
        std::vector<SKey> before(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(I));
        order.erase(order.begin() + static_cast<std::ptrdiff_t>(I));
        if (!trip)
            return;
        ++edits;
        SHold h{.traveller = k.who, .app = k.app, .rest = order, .edits = edits};
        h.index = placeOf(h, before);
        hold    = h;
    }
    // A window arrives (opened, moved, carried home): what the hold said.
    EHome arrive(const SKey& k, bool fromTrip) {
        EHome e = EHome::Other;
        if (hold) {
            e = decide(*hold, k.who, k.app, fromTrip, order, edits, false);
            if (e == EHome::Restore) {
                order.insert(order.begin() + static_cast<std::ptrdiff_t>(homeIndex(*hold, order)), k);
                hold.reset();
                ++edits;
                return e;
            }
            if (e != EHome::Sibling) {
                hold.reset();
                ++edits;
            }
        }
        order.push_back(k); // the end of the arrival order: the other side
        return e;
    }
    void resize(const SKey& k) {
        if (isEdit(hold ? &*hold : nullptr, k.app))
            ++edits;
    }
};

int main() {
    int a1 = 0, a2 = 0, a3 = 0, other = 0;
    const SKey K2{&a2, 596, 396, 2}, K3{&a3, 496, 346, 3};
    const SHold H{.traveller = &a1, .index = 0, .rest = {K2, K3}, .edits = 7};

    // the same window, carried home by its trip, to the same desktop: back in place
    CHECK(restorable(H, &a1, true, {K2, K3}, 7, false));
    CHECK(homeIndex(H, {K2, K3}) == 0);
    // moved home by hand, not by the trip: the normal rules
    CHECK(!restorable(H, &a1, false, {K2, K3}, 7, false));
    CHECK(decide(H, &a1, {}, false, {K2, K3}, 7, false) == EHome::ByHand);
    // another window arriving
    CHECK(!restorable(H, &other, true, {K2, K3}, 7, false));
    CHECK(!restorable(H, nullptr, true, {K2, K3}, 7, false));
    // something changed meanwhile: a window closed, opened, reordered, resized
    CHECK(!restorable(H, &a1, true, {K2}, 7, false));
    CHECK(!restorable(H, &a1, true, {K2, K3, SKey{&other, 100, 100, 9}}, 7, false));
    CHECK(!restorable(H, &a1, true, {K3, K2}, 7, false));
    CHECK(!restorable(H, &a1, true, {K2, SKey{&a3, 500, 346, 3}}, 7, false));
    CHECK(decide(H, &a1, {}, true, {K3, K2}, 7, false) == EHome::Changed);
    // the same windows, but an edit (a tile resized, a move) or a drop/swap set an arrangement
    CHECK(!restorable(H, &a1, true, {K2, K3}, 8, false));
    CHECK(!restorable(H, &a1, true, {K2, K3}, 7, true));
    CHECK(decide(H, &a1, {}, true, {K2, K3}, 8, false) == EHome::Edited);
    CHECK(decide(H, &a1, {}, true, {K2, K3}, 7, true) == EHome::Arranged);
    // its place, clamped to the order it goes back into
    const SHold LAST{.traveller = &a1, .index = 5, .rest = {K2}, .edits = 0};
    CHECK(homeIndex(LAST, {K2}) == 1);
    const SHold MID{.traveller = &a2, .index = 1, .rest = {SKey{&a1, 1, 1, 1}, K3}, .edits = 0};
    CHECK(homeIndex(MID, {SKey{&a1, 1, 1, 1}, K3}) == 1);

    // ONE APP: one process; a class only when a pid is not known
    const SApp BNET{.pid = 4242, .cls = "battle.net.exe"}, HELPER{.pid = 4242, .cls = "battle.net.exe"}, ZEN{.pid = 77, .cls = "zen"};
    CHECK(sameApp(BNET, HELPER));
    CHECK(!sameApp(BNET, ZEN));
    CHECK(!sameApp(SApp{.pid = 10, .cls = "kitty"}, SApp{.pid = 11, .cls = "kitty"})); // two terminals: two apps
    CHECK(sameApp(SApp{.pid = 0, .cls = "battle.net.exe"}, BNET));                       // unmapping: no pid any more
    CHECK(!sameApp(SApp{}, SApp{}));

    // THE RECORDING'S DESKTOP: the launcher L between the browser B and a
    // terminal T; a helper window h of the launcher's app comes and goes.
    int l = 0, b = 0, t = 0, h = 0, h2 = 0, n = 0;
    const SKey L{&l, 900, 620, 1, BNET}, B{&b, 1200, 900, 2, ZEN}, T{&t, 600, 400, 3, SApp{.pid = 90, .cls = "kitty"}};
    const SKey HK{&h, 360, 240, 4, HELPER}, HK2{&h2, 360, 240, 5, HELPER}, NEWK{&n, 500, 300, 6, SApp{.pid = 91, .cls = "kitty"}};
    const std::vector<SKey> START{B, L, T};
    const auto FRESH = [&] { return SDesk{.order = START}; };
    {   // the plain round trip
        auto D = FRESH();
        D.leave(L, true);
        CHECK(D.arrive(L, true) == EHome::Restore && D.order == START);
    }
    {   // the swap seen in the recording: a helper open at home closes while the launcher is away
        auto D = FRESH();
        D.arrive(HK, false);
        D.leave(L, true);
        D.leave(HK, false);
        CHECK(D.arrive(L, true) == EHome::Restore);
        CHECK(D.order == START);
    }
    {   // a helper maps at home while it is away, and is still there when it comes back
        auto D = FRESH();
        D.leave(L, true);
        CHECK(D.arrive(HK, false) == EHome::Sibling);
        CHECK(D.hold.has_value());
        CHECK(D.arrive(L, true) == EHome::Restore);
        CHECK(D.at(&l) == 1 && D.order[0].who == &b && D.at(&t) < D.order.size());
        D.leave(HK, false);
        CHECK(D.order == START);
    }
    {   // the app's leftovers carried home first (pay()), then the launcher; a helper resized meanwhile
        auto D = FRESH();
        D.leave(L, true);
        D.arrive(HK, false);
        D.arrive(HK2, false);
        D.resize(HK);
        D.leave(HK2, false);
        CHECK(D.arrive(L, true) == EHome::Restore);
        D.leave(HK, false);
        CHECK(D.order == START);
    }
    {   // quick toggles: six trips in a row, a helper mapping and unmapping in between
        auto D = FRESH();
        for (int k = 0; k < 6; ++k) {
            D.leave(L, true);
            if (k % 2)
                D.arrive(HK, false);
            else
                D.leave(HK, false);
            CHECK(D.arrive(L, true) == EHome::Restore);
        }
        D.leave(HK, false);
        CHECK(D.order == START);
    }
    {   // a trip begun while the last one's hold was never consumed (it went somewhere else first)
        auto D = FRESH();
        D.leave(L, true);
        D.leave(L, true); // not there: no-op order-wise, the hold stays the first's
        CHECK(D.arrive(L, true) == EHome::Restore && D.order == START);
    }
    // ...and what still voids it: a real change of the desktop
    {   // another app's window opened there
        auto D = FRESH();
        D.leave(L, true);
        CHECK(D.arrive(NEWK, false) == EHome::Other);
        CHECK(D.arrive(L, true) == EHome::Other && D.order.back().who == &l);
    }
    {   // another app's window closed there
        auto D = FRESH();
        D.leave(L, true);
        D.leave(T, false);
        CHECK(D.arrive(L, true) == EHome::Edited);
    }
    {   // another app's window resized there
        auto D = FRESH();
        D.leave(L, true);
        D.resize(B);
        CHECK(D.arrive(L, true) == EHome::Edited);
    }
    {   // the launcher moved home by hand
        auto D = FRESH();
        D.leave(L, true);
        CHECK(D.arrive(L, false) == EHome::ByHand);
    }
    {   // a swap meanwhile: the order changed
        auto D = FRESH();
        D.leave(L, true);
        std::swap(D.order[0], D.order[1]);
        CHECK(D.arrive(L, true) == EHome::Changed);
    }
    // in the tiles, the kept tree knows only the windows that were there
    {
        const SHold HT{.traveller = &l, .app = BNET, .index = 1, .rest = {B, T}, .edits = 1};
        CHECK(decide(HT, &l, BNET, true, {B, T, HK}, 1, false, true) == EHome::Tiles);
        CHECK(decide(HT, &l, BNET, true, {B, T, HK}, 1, false, false) == EHome::Restore);
        CHECK(decide(HT, &l, BNET, true, {B, T}, 1, false, true) == EHome::Restore);
    }
    for (auto e : {EHome::Restore, EHome::Sibling, EHome::Other, EHome::ByHand, EHome::Edited, EHome::Arranged, EHome::Changed, EHome::Tiles})
        CHECK(why(e)[0] != '?');

    std::printf("mosaic_trip_hold_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
