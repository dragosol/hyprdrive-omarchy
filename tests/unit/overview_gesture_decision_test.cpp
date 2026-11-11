// Host test for OverviewGestureDecision.hpp: every (bound, taken) direction
// pair against the expression COverviewGesture::begin used inline before it
// moved, written out here verbatim over Hyprland's numeric values, plus the
// cases that matter named one by one.
#include "OverviewGestureDecision.hpp"

#include <cstdint>
#include <cstdio>

using namespace Hyprdrive::OverviewGestureDecision;

static int g_checks = 0, g_failed = 0;
#define CHECK(c)                                                                                                                                                                   \
    do {                                                                                                                                                                           \
        ++g_checks;                                                                                                                                                                \
        if (!(c)) {                                                                                                                                                                \
            ++g_failed;                                                                                                                                                            \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);                                                                                                               \
        }                                                                                                                                                                          \
    } while (0)

// Hyprland 0.56's eTrackpadGestureDirection: DOWN = 5, VERTICAL = 6.
static bool before(uint8_t direction, uint8_t taken) {
    constexpr uint8_t TRACKPAD_GESTURE_DIR_DOWN = 5, TRACKPAD_GESTURE_DIR_VERTICAL = 6;
    return direction == TRACKPAD_GESTURE_DIR_DOWN || (direction == TRACKPAD_GESTURE_DIR_VERTICAL && taken == TRACKPAD_GESTURE_DIR_DOWN);
}

int main() {
    constexpr int COUNT = static_cast<int>(EDir::PinchIn) + 1;
    CHECK(COUNT == 11);
    for (int b = 0; b < COUNT; ++b)
        for (int t = 0; t < COUNT; ++t) {
            const auto BOUND = static_cast<EDir>(b), TAKEN = static_cast<EDir>(t);
            CHECK(wantsClose(BOUND, TAKEN) == before(b, t));
            CHECK(wantsOpen(BOUND, TAKEN) == !wantsClose(BOUND, TAKEN));
        }

    // Bound "down": closes whatever begin is told.
    for (int t = 0; t < COUNT; ++t)
        CHECK(wantsClose(EDir::Down, static_cast<EDir>(t)));
    // Bound "vertical": down closes, up (and anything else) opens.
    CHECK(wantsClose(EDir::Vertical, EDir::Down));
    CHECK(wantsOpen(EDir::Vertical, EDir::Up));
    CHECK(wantsOpen(EDir::Vertical, EDir::Vertical));
    // Bound "up", and every other binding: opens, even when the fingers went down.
    CHECK(wantsOpen(EDir::Up, EDir::Up));
    CHECK(wantsOpen(EDir::Up, EDir::Down));
    CHECK(wantsOpen(EDir::Horizontal, EDir::Down));
    CHECK(wantsOpen(EDir::Pinch, EDir::Down));
    // Usable where a constant is needed.
    static_assert(wantsClose(EDir::Down, EDir::None) && !wantsClose(EDir::Up, EDir::Down));

    std::printf("overview_gesture_decision_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
