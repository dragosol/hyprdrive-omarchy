// Host test for HotCornerModel.hpp: corner names, the in-corner band at each
// corner, and the probes that decide whether a corner is an outer one.
#include "../HotCornerModel.hpp"

#include <cstdio>

using namespace ScrollOverview::HotCorner;

static int g_checks = 0, g_failed = 0;
#define CHECK(c)                                                                                                                                                                   \
    do {                                                                                                                                                                           \
        ++g_checks;                                                                                                                                                                \
        if (!(c)) {                                                                                                                                                                \
            ++g_failed;                                                                                                                                                            \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);                                                                                                               \
        }                                                                                                                                                                          \
    } while (0)

static bool contains(const SRect& r, const SPoint& p) {
    return p.x >= r.x && p.y >= r.y && p.x < r.x + r.w && p.y < r.y + r.h;
}

int main() {
    static_assert(cornerFromName("top-left") == ECorner::TopLeft);
    static_assert(cornerFromName("bottom-right") == ECorner::BottomRight);
    CHECK(cornerFromName("top-right") == ECorner::TopRight);
    CHECK(cornerFromName("bottom-left") == ECorner::BottomLeft);
    CHECK(cornerFromName("") == ECorner::TopLeft);
    CHECK(cornerFromName("nonsense") == ECorner::TopLeft);

    // A 1440x900 output at (0,0); the pointer is clamped to [0,1439]x[0,899].
    const SRect M{0, 0, 1440, 900};
    CHECK(inCorner(0, 0, M, ECorner::TopLeft));
    CHECK(inCorner(2, 2, M, ECorner::TopLeft));
    CHECK(!inCorner(3, 0, M, ECorner::TopLeft));
    CHECK(!inCorner(0, 3, M, ECorner::TopLeft));
    CHECK(!inCorner(0, 0, M, ECorner::TopRight));
    CHECK(inCorner(1439, 0, M, ECorner::TopRight));
    CHECK(inCorner(1437, 2, M, ECorner::TopRight));
    CHECK(!inCorner(1436, 0, M, ECorner::TopRight));
    CHECK(inCorner(0, 899, M, ECorner::BottomLeft));
    CHECK(inCorner(1439, 899, M, ECorner::BottomRight));
    CHECK(!inCorner(720, 450, M, ECorner::BottomRight));
    // Outside the output entirely is no corner of it.
    CHECK(!inCorner(-1, 0, M, ECorner::TopLeft));
    CHECK(!inCorner(1441, 0, M, ECorner::TopRight));
    CHECK(!inCorner(0, 0, SRect{0, 0, 0, 0}, ECorner::TopLeft));

    // An output that does not start at the origin (second monitor at x=1440).
    const SRect R{1440, 0, 1920, 1080};
    CHECK(inCorner(1440, 0, R, ECorner::TopLeft));
    CHECK(inCorner(3359, 1079, R, ECorner::BottomRight));

    // Probes: side by side, the left monitor's top-RIGHT corner touches the
    // right monitor, so its sideways probe lands inside it; its top-LEFT does not.
    {
        const auto P = probes(M, ECorner::TopRight);
        CHECK(contains(R, P[0]));
        const auto Q = probes(M, ECorner::TopLeft);
        CHECK(!contains(R, Q[0]) && !contains(R, Q[1]) && !contains(R, Q[2]));
        // The right monitor's top-left touches the left monitor.
        const auto S = probes(R, ECorner::TopLeft);
        CHECK(contains(M, S[0]));
    }
    // Stacked: a monitor above the 1440x900 one makes its top corners inner.
    {
        const SRect ABOVE{0, -900, 1440, 900};
        const auto  P = probes(M, ECorner::TopLeft);
        CHECK(contains(ABOVE, P[1]));
        const auto Q = probes(M, ECorner::BottomLeft);
        CHECK(!contains(ABOVE, Q[0]) && !contains(ABOVE, Q[1]) && !contains(ABOVE, Q[2]));
    }

    std::printf("hot_corner_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
