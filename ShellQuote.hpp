#pragma once

#include <string>

// One argv word as /bin/sh will read it back. Single quotes, because inside them
// the shell expands nothing at all -- no $(), no backticks, no $VAR -- and the
// only escape needed is for a single quote itself, which closes, escapes and
// reopens. Text the user typed that ends up in a `bash -c` string goes through
// here (marketplace review, #11012: agent prompts). Pure, host-tested.
namespace ScrollOverview {
    inline std::string shellQuote(const std::string& word) {
        std::string out = "'";
        for (const char C : word) {
            if (C == '\'')
                out += "'\\''";
            else
                out += C;
        }
        return out + "'";
    }
}
