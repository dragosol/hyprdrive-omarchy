#pragma once

#include <string>

namespace ScrollOverview::TextInput {
    // The text is UTF-8, so Backspace removes a CODEPOINT: pop_back() would
    // leave a severed lead byte.
    inline void popCodepoint(std::string& s) {
        size_t n = s.size();
        while (n > 0 && (static_cast<unsigned char>(s[n - 1]) & 0xC0) == 0x80)
            --n;
        s.resize(n > 0 ? n - 1 : 0);
    }
}
