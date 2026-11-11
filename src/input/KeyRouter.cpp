#include "PluginParts.hpp"
using namespace PluginDetail;

// Double-tap SHIFT in the search field launches a NEW instance of the selected
// app. Same argument as tap-SUPER above: a bind on SHIFT_L cannot tell a tap
// from the SHIFT that is holding down a capital letter, and this is a field the
// user types capitals into. A tap is a press and release with no other real key
// between them, so typing "Zen" cancels on the 'z' and never reaches here.
static constexpr uint32_t EVDEV_KEY_LEFTSHIFT  = 42;
static constexpr uint32_t EVDEV_KEY_RIGHTSHIFT = 54;
// Ctrl x2 (set the browsed AI agent as Omarchy's default) is the same gesture
// with the same timing -- a tap is a tap -- so it shares both constants below.
static constexpr uint32_t EVDEV_KEY_LEFTCTRL   = 29;
static constexpr uint32_t EVDEV_KEY_RIGHTCTRL  = 97;
// A tap must RELEASE within this, or it was a hold.
static constexpr uint32_t TAP_SHIFT_HOLD_MS = 250;
// ...and the second tap must land within this of the first, or it is the start
// of a new pair rather than the end of one.
static constexpr uint32_t TAP_SHIFT_GAP_MS = 450;

// Only a real key cancels the tap. Modifier and lock keycodes must be ignored:
// this compositor emits a synthetic CAPSLOCK (58) press/release pair around
// every key event, which would otherwise cancel every tap before it completed.
static bool isModifierKeycode(uint32_t keycode) {
    switch (keycode) {
        case 29:  // LEFTCTRL
        case 42:  // LEFTSHIFT
        case 54:  // RIGHTSHIFT
        case 56:  // LEFTALT
        case 58:  // CAPSLOCK
        case 69:  // NUMLOCK
        case 70:  // SCROLLLOCK
        case 97:  // RIGHTCTRL
        case 100: // RIGHTALT
        case EVDEV_KEY_LEFTMETA:
        case EVDEV_KEY_RIGHTMETA: return true;
        default: return false;
    }
}

static constexpr uint32_t EVDEV_KEY_SPACE     = 57;   // V5, the sushi preview

namespace PluginDetail {
// ONE raw-key listener, and it owns the guard-modifier count, V5's SPACE
// preview, the search keys, and the SUPER tap. The order is load-bearing at
// every step (each branch says why), and they are one listener because they
// read one key stream and each has to decide whether the key survives to the
// next -- two listeners could not.
void                registerRawKeyHooks() {
    g_pRawKeyHook = Event::bus()->m_events.input.keyboard.key.listen([](IKeyboard::SKeyEvent event, Event::SCallbackInfo& info) {
        const bool PRESSED = event.state == WL_KEYBOARD_KEY_STATE_PRESSED;
        const bool ISSUPER = event.keycode == EVDEV_KEY_LEFTMETA || event.keycode == EVDEV_KEY_RIGHTMETA;

        if (isGuardModifierKeycode(event.keycode)) {
            if (PRESSED)
                g_heldGuardKeys.insert(event.keycode);
            else
                g_heldGuardKeys.erase(event.keycode);
        }

        // Locked: every key is the lock screen's. Nothing below runs, nothing
        // is cancelled, no search key repeats (hyprdriveSessionLocked).
        if (hyprdriveSessionLocked()) {
            stopSearchRepeat();
            return;
        }

        // The zoomed-out canvas: the user's screenshot key (Print) shoots it,
        // though Super is held for the zoom (Mosaic::onZoomKey).
        if (Hyprdrive::Mosaic::onZoomKey(event.keycode, PRESSED, event.timeMs)) {
            info.cancelled = true;
            return;
        }

        // A REAL KEY BREAKS EVERY TAP IN FLIGHT -- and it has to be recorded
        // HERE, above every consume path, not after them.
        //
        // The same bookkeeping lives further down inside the !ISSUPER branch,
        // where it only ever ran for keys that fell all the way through. A chord
        // whose letter one of the branches below swallowed returned first, so
        // the SUPER tap candidate was still standing when SUPER came back up and
        // the release was read as a tap: SUPER+W closed the window and then
        // toggled the overview shut on the way out (user, 2026-09-23). Every
        // Super chord over an open overview had it; W is just the first one
        // bound to something.
        if (PRESSED && !isModifierKeycode(event.keycode)) {
            g_tapSuperCandidate = false;
            g_tapShiftCandidate = false;
            g_tapShiftLastTapMs = 0;
            g_tapCtrlCandidate  = false;
            g_tapCtrlLastTapMs  = 0;
        }

        // The overview's strip lifts the desktop under the pointer while Super
        // is held; Super+Shift+Left/Right moves the desktop shown -- in the
        // overview only (outside it the chord stays Omarchy's window swap).
        if (ISSUPER)
            overviewSuperChanged();
        if (PRESSED && (event.keycode == 105 || event.keycode == 106) && g_pInputManager &&
            (g_pInputManager->getModsFromAllKBs() & ~(HL_MODIFIER_CAPS | HL_MODIFIER_MOD2)) == (HL_MODIFIER_META | HL_MODIFIER_SHIFT) &&
            overviewMoveShownDesktop(event.keycode == 106 ? 1 : -1)) {
            g_consumedPresses.insert(event.keycode);
            info.cancelled = true;
            return;
        }

        if (!ISSUPER) {
            if (PRESSED) {
                // The easter egg owns the overview's keys while it is up: the
                // first ones are swallowed (the hands are still mid-spam), any
                // after that stops it, and none of them types into the search.
                if (overviewAcceptsSearch() && Hyprdrive::KeyboardCat::active() && !isGlobalPassthroughKeycode(event.keycode) &&
                    Hyprdrive::KeyboardCat::handleKey(event.keycode)) {
                    stopSearchRepeat();
                    g_consumedPresses.insert(event.keycode);
                    info.cancelled = true;
                    return;
                }
                // V11 SPEC V5. SPACE PREVIEWS THE HOVERED FAN/GRID ITEM.
                //
                // THE SCOPE IS THE FEATURE. previewHovered() claims the key
                // only when a stack popup is open AND the pointer is over one
                // of its file rows -- two conditions that are false in every
                // other state this compositor is ever in, including with the
                // dock right there and a stack pinned. Everywhere else the
                // call is a null check and a walk of an empty vector, and the
                // key falls through to the focused window exactly as before.
                // A space that went missing in a text editor would be a far
                // bigger bug than this is a feature, so the claim is decided
                // by the same read that decides what to preview: no item, no
                // claim (see DockStacks::previewHovered).
                //
                // AHEAD of handleSearchPress, and that ordering is not an
                // accident: while the overview is open handleSearchPress
                // consumes SPACE unconditionally -- G1's rule that whitespace
                // must never START a search still keeps the key rather than
                // typing it into a window the user cannot see. A fan opened
                // over the overview would otherwise never see the press.
                // Dock stacks A: an open fan/pane owns ITS keys (arrows, Enter,
                // Esc, Space; the pane also types). handleKey claims SPACE only
                // when there is an item to preview, so everywhere else the key
                // falls through untouched -- the scope rule above still holds.
                // Neither a stack popup nor the Rename field takes a CHORD (Super/
                // Ctrl/Alt held) or a desktop key (Print, volume, brightness):
                // those always reach their binds (review 2026-09-24).
                const bool PLAINKEY = g_heldGuardKeys.empty() && !isGlobalPassthroughKeycode(event.keycode);

                // The dock menu's Rename field, while it is up, owns the keyboard.
                if (PLAINKEY && Hyprdrive::Dock::menuWantsKeyboard() && Hyprdrive::Dock::menuHandleKey(event.keycode)) {
                    g_consumedPresses.insert(event.keycode);
                    info.cancelled = true;
                    return;
                }
                if (PLAINKEY && Hyprdrive::Stacks::keyboardActive() && Hyprdrive::Stacks::handleKey(event.keycode)) {
                    g_consumedPresses.insert(event.keycode);
                    info.cancelled = true;
                    return;
                }

                if (event.keycode == EVDEV_KEY_SPACE) {
                    if (overviewAcceptsSearch()) {
                        if (overviewHandleSearchSpace()) {
                            for (const auto& overview : scrollOverviews())
                                if (overview)
                                    overview->damage();
                            g_consumedPresses.insert(event.keycode);
                            info.cancelled = true;
                            return;
                        }
                        // Not a preview: fall through to handleSearchPress and
                        // type the space into the query.
                    } else if (OmarchySearch::isSushiActive()) {
                        OmarchySearch::closeSushi();
                        for (const auto& overview : scrollOverviews())
                            if (overview)
                                overview->damage();
                        g_consumedPresses.insert(event.keycode);
                        info.cancelled = true;
                        return;
                    }
                }

                if (handleSearchPress(event.keycode)) {
                    g_consumedPresses.insert(event.keycode);
                    if (event.keycode == EVDEV_KEY_BACKSPACE && !g_search.query.text.empty())
                        startSearchRepeat(event.keycode);
                    info.cancelled = true;
                    return;
                }

                // Overview owns the keyboard: a key that is not search input
                // still must not reach the window or a layer-shell bar.
                // Arrows/Tab/Return are left for CScrollOverview::onKeyboardKey.
                // PRINT and the XF86 keys are the exception: they are desktop
                // keys, not query keys, and Omarchy's binds must still see them.
                // So is a CHORD (Super/Ctrl/Alt held) the overview did not take
                // above or in onKeyboardKey: it is never query text, and the
                // user's chord binds are borrowed into the submap for exactly
                // this (SubmapBorrow.hpp) -- Super+F did nothing in the
                // overview here while native's worked (user, 2026-11-06).
                if (overviewAcceptsSearch() && g_heldGuardKeys.empty() && !isModifierKeycode(event.keycode) && !isGlobalPassthroughKeycode(event.keycode) &&
                    event.keycode != 15 && event.keycode != 28 && event.keycode != 96 &&
                    event.keycode != 103 && event.keycode != 105 && event.keycode != 106 &&
                    event.keycode != 108) {
                    g_consumedPresses.insert(event.keycode);
                    info.cancelled = true;
                    return;
                }
            }
            else if (g_consumedPresses.erase(event.keycode) > 0) {
                if (event.keycode == g_searchRepeatKey)
                    stopSearchRepeat();
                info.cancelled = true;
                return;
            }
        }

        // DOUBLE-TAP SHIFT -> new instance of the selected app. overviewHandleShiftTap
        // was written but never wired to a caller, so the gesture had no way to
        // fire; the timing half of it belongs here, where tap and hold are still
        // distinguishable.
        if (event.keycode == EVDEV_KEY_LEFTSHIFT || event.keycode == EVDEV_KEY_RIGHTSHIFT) {
            if (PRESSED) {
                g_tapShiftCandidate = true;
                g_tapShiftPressedAt = event.timeMs;
            } else if (g_tapShiftCandidate) {
                g_tapShiftCandidate  = false;
                const bool WASTAP    = event.timeMs - g_tapShiftPressedAt <= TAP_SHIFT_HOLD_MS;
                const bool PAIRED    = WASTAP && g_tapShiftLastTapMs != 0 && event.timeMs - g_tapShiftLastTapMs <= TAP_SHIFT_GAP_MS;
                g_tapShiftLastTapMs  = PAIRED || !WASTAP ? 0 : event.timeMs;
                if (PAIRED && overviewAcceptsSearch() && overviewHandleShiftTap()) {
                    info.cancelled = true;
                    return;
                }
            }
        }

        // DOUBLE-TAP CTRL -> the browsed AI agent becomes Omarchy's default. The
        // Shift pair's timing and its cancel rule (a real key between the taps
        // breaks it), so Ctrl+Backspace and every other chord stay chords.
        if (event.keycode == EVDEV_KEY_LEFTCTRL || event.keycode == EVDEV_KEY_RIGHTCTRL) {
            if (PRESSED) {
                g_tapCtrlCandidate = true;
                g_tapCtrlPressedAt = event.timeMs;
            } else if (g_tapCtrlCandidate) {
                g_tapCtrlCandidate = false;
                const bool WASTAP  = event.timeMs - g_tapCtrlPressedAt <= TAP_SHIFT_HOLD_MS;
                const bool PAIRED  = WASTAP && g_tapCtrlLastTapMs != 0 && event.timeMs - g_tapCtrlLastTapMs <= TAP_SHIFT_GAP_MS;
                g_tapCtrlLastTapMs = PAIRED || !WASTAP ? 0 : event.timeMs;
                if (PAIRED && overviewAcceptsSearch() && overviewHandleCtrlTap()) {
                    info.cancelled = true;
                    return;
                }
            }
        }

        if (!ISSUPER) {
            if (PRESSED && !isModifierKeycode(event.keycode)) {
                g_tapSuperCandidate = false; // SUPER is being held as a modifier
                // ...and so is SHIFT. A real key also breaks a pair in flight,
                // so tap, type, tap is not a double tap.
                g_tapShiftCandidate = false;
                g_tapShiftLastTapMs = 0;
                g_tapCtrlCandidate  = false;
                g_tapCtrlLastTapMs  = 0;
            }
            return;
        }

        // The infinite canvas's Super gesture (Mosaic.hpp): armed on the press,
        // over on the release -- zoomed out, the release zooms back in.
        Hyprdrive::Mosaic::onSuperKey(PRESSED);

        if (PRESSED) {
            g_tapSuperCandidate = true;
            g_tapSuperPressedAt = event.timeMs;
            return;
        }

        if (!g_tapSuperCandidate)
            return;
        g_tapSuperCandidate = false;

        if (!Hyprdrive::Config::getTapSuper())
            return;

        const auto TIMEOUT = Hyprdrive::Config::getTapSuperTimeoutMs();
        if (TIMEOUT > 0 && event.timeMs - g_tapSuperPressedAt > static_cast<uint32_t>(TIMEOUT))
            return;

        onOverviewDispatcher("toggle all");
    });

    // SUPER+click/drag (move or resize a window) must not count as a tap.
    g_pTapSuperButtonHook = Event::bus()->m_events.input.mouse.button.listen([](IPointer::SButtonEvent event, Event::SCallbackInfo& info) { g_tapSuperCandidate = false; });
}
} // namespace PluginDetail
