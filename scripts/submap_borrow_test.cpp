// SubmapBorrow: which binds stay live in the overview's submap (O5/B19).
#include "../SubmapBorrow.hpp"

#include <cstdio>

using namespace ScrollOverview::SubmapBorrow;

static int checks = 0, failed = 0;
static void expect(bool ok, const char* what) {
    ++checks;
    if (!ok) {
        ++failed;
        std::printf("FAIL %s\n", what);
    }
}

int main() {
    // The README's suggested binds and the scratchpad chords.
    expect(borrows("TAB", MOD_META, false, false), "SUPER+TAB (README overview toggle) is borrowed");
    expect(borrows("H", MOD_META, false, false), "SUPER+H (README minimize) is borrowed");
    expect(borrows("S", MOD_META, false, false), "SUPER+S scratchpad is borrowed");
    expect(borrows("grave", MOD_META, false, false), "Omarchy SUPER+grave scratchpad is borrowed");
    expect(borrows("Return", MOD_META, false, false), "SUPER+RETURN is borrowed");
    expect(borrows("Delete", MOD_CTRL | MOD_ALT, false, false), "CTRL+ALT+DEL is borrowed");
    expect(borrows("T", MOD_META | MOD_SHIFT, false, false), "SUPER+SHIFT+T is borrowed");
    expect(borrows("F", MOD_ALT, false, false), "ALT+F is borrowed");

    // The search field's keys stay its own.
    expect(!borrows("A", 0, false, false), "a plain letter is query text");
    expect(!borrows("A", MOD_SHIFT, false, false), "a shifted letter is query text");
    expect(!borrows("TAB", 0, false, false), "plain Tab is the pane cycle");
    expect(!borrows("TAB", MOD_SHIFT, false, false), "Shift+Tab is the pane cycle");
    expect(!borrows("Escape", 0, false, false), "Escape is the overview's");

    // Desktop keys, with or without modifiers.
    expect(borrows("Print", 0, false, false), "Print is borrowed");
    expect(borrows("print", MOD_SHIFT, false, false), "Shift+Print is borrowed");
    expect(borrows("Sys_Req", MOD_ALT, false, false), "Alt+Print (Sys_Req) is borrowed");
    expect(borrows("XF86AudioRaiseVolume", 0, false, false), "volume keys are borrowed");
    expect(borrows("xf86monbrightnessup", 0, false, false), "brightness keys, any case");

    // Never the submap's own job, a catch-all, or a bind on a modifier key.
    expect(!borrows("mouse:272", MOD_META, true, false), "SUPER+drag stays shadowed (mouse flag)");
    expect(!borrows("mouse:272", MOD_META, false, false), "SUPER+mouse:272 stays shadowed (key name)");
    expect(!borrows("mouse_down", MOD_META, false, false), "SUPER+wheel stays shadowed");
    expect(!borrows("", MOD_META, false, true), "a catch-all is never borrowed");
    expect(!borrows("Q", MOD_META, false, true), "a catch-all flag wins over a chord");
    expect(!borrows("SUPER_L", MOD_META, false, false), "a SUPER_L bind is the plugin's tap");
    expect(!borrows("Super_R", MOD_META, false, false), "a Super_R bind likewise");
    expect(!borrows("Control_L", MOD_CTRL, false, false), "a Control_L bind is no chord");
    expect(!borrows("Alt_L", MOD_ALT, false, false), "an Alt_L bind is no chord");
    expect(!borrows("Shift_L", MOD_META | MOD_SHIFT, false, false), "a Shift_L bind is no chord");

    std::printf("submap_borrow_test: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
