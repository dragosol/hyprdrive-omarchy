#pragma once

// THE TYPING LOGIC, one copy (dock stacks A): the overview's search field and
// the stack pane both type through this. Compose and dead keys, UTF-8,
// control keys refused.

#include "TextInputUtf8.hpp"

#include <cstdint>
#include <string>
#include <xkbcommon/xkbcommon.h>

namespace ScrollOverview::TextInput {
    struct SKey {
        bool        consume = false; // the key belongs to the field
        bool        starts  = false; // counts as typing, even with no text yet (compose mid-flight)
        std::string text;            // what to append; empty while a compose sequence is open
    };

    SKey keyFor(xkb_state* state, uint32_t keycode);
    void resetCompose();
}
