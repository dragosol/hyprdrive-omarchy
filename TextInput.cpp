#include "TextInput.hpp"

#include <cstdlib>
#include <xkbcommon/xkbcommon-compose.h>

namespace {
    // Built once from the locale; null when there is no Compose file, and every
    // path below then falls through to the plain utf8 one. Never freed: it lives
    // as long as the plugin, and freeing it at unload would race the key hook.
    xkb_compose_state* composeState() {
        static xkb_compose_state* STATE = []() -> xkb_compose_state* {
            const char* LOCALE = nullptr;
            for (const char* VAR : {"LC_ALL", "LC_CTYPE", "LANG"}) {
                if (const char* V = getenv(VAR); V && *V) {
                    LOCALE = V;
                    break;
                }
            }
            if (!LOCALE)
                LOCALE = "C";
            xkb_context* CTX = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
            if (!CTX)
                return nullptr;
            xkb_compose_table* TABLE = xkb_compose_table_new_from_locale(CTX, LOCALE, XKB_COMPOSE_COMPILE_NO_FLAGS);
            if (!TABLE)
                return nullptr;
            xkb_compose_state* S = xkb_compose_state_new(TABLE, XKB_COMPOSE_STATE_NO_FLAGS);
            xkb_compose_table_unref(TABLE);
            return S;
        }();
        return STATE;
    }
}

namespace ScrollOverview::TextInput {

    void resetCompose() {
        if (auto* C = composeState(); C)
            xkb_compose_state_reset(C);
    }

    SKey keyFor(xkb_state* state, uint32_t keycode) {
        if (!state)
            return {};

        const xkb_keysym_t SYM = xkb_state_key_get_one_sym(state, keycode + 8);

        if (auto* COMPOSE = composeState(); COMPOSE && SYM != XKB_KEY_NoSymbol && xkb_compose_state_feed(COMPOSE, SYM) == XKB_COMPOSE_FEED_ACCEPTED) {
            switch (xkb_compose_state_get_status(COMPOSE)) {
                case XKB_COMPOSE_COMPOSING: return {.consume = true, .starts = true};
                case XKB_COMPOSE_COMPOSED: {
                    char      buf[64] = {};
                    const int N       = xkb_compose_state_get_utf8(COMPOSE, buf, sizeof(buf));
                    resetCompose();
                    return {.consume = true, .starts = true, .text = N > 0 ? std::string(buf, N) : std::string()};
                }
                case XKB_COMPOSE_CANCELLED: resetCompose(); return {.consume = true, .starts = true};
                case XKB_COMPOSE_NOTHING: break;
            }
        }

        char      buf[8] = {};
        const int N      = xkb_state_key_get_utf8(state, keycode + 8, buf, sizeof(buf));
        if (N <= 0)
            return {};
        if (N == 1 && (buf[0] < 0x20 || buf[0] == 0x7F))
            return {};
        return {.consume = true, .starts = true, .text = std::string(buf, N)};
    }
}
