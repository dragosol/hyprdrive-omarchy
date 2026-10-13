// IconOverrides::parse -- the per-app dock icon store's line format.
#include "../IconOverrides.hpp"

#include <iostream>

static int s_checks = 0, s_failed = 0;
#define CHECK(c)                                                                                                                                                              \
    do {                                                                                                                                                                       \
        ++s_checks;                                                                                                                                                            \
        if (!(c)) {                                                                                                                                                            \
            ++s_failed;                                                                                                                                                        \
            std::cerr << "FAILED line " << __LINE__ << ": " #c "\n";                                                                                                          \
        }                                                                                                                                                                      \
    } while (0)

using ScrollOverview::IconOverrides::parse;

int main() {
    {
        const auto M = parse("org.gnome.Nautilus\t/home/u/.local/share/hyprdrive/icons/files-3f9a.svg\nomamail\tmail-client\n");
        CHECK(M.size() == 2);
        CHECK(M.at("org.gnome.Nautilus") == "/home/u/.local/share/hyprdrive/icons/files-3f9a.svg");
        CHECK(M.at("omamail") == "mail-client");
    }
    {
        // comments, blanks, CRLF, trailing blanks, malformed lines
        const auto M = parse("# header\n\nzen\tzen-browser \r\nnotab line\n\tno-id\nno-icon\t\nspaces in id\t/p/a b.png\n");
        CHECK(M.size() == 2);
        CHECK(M.at("zen") == "zen-browser");
        CHECK(M.at("spaces in id") == "/p/a b.png");
        CHECK(!M.contains("notab line"));
        CHECK(!M.contains("no-icon"));
    }
    {
        // last line wins; no trailing newline
        const auto M = parse("a\tone\na\ttwo");
        CHECK(M.size() == 1);
        CHECK(M.at("a") == "two");
    }
    CHECK(parse("").empty());
    CHECK(parse("\n\n").empty());

    std::cout << "icon_overrides_test: " << s_checks << " checks, " << s_failed << " failed\n";
    return s_failed ? 1 : 0;
}
