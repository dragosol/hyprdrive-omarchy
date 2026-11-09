// TrashDirs.hpp: /proc/mounts decoding (review 2026-11-09). A label with a
// backslash (`foo\134bar`) threw std::invalid_argument on the render thread,
// and `a\134123` was decoded twice.
#include "../src/TrashDirs.hpp"

#include <cstdio>
#include <string>

using namespace ScrollOverview::TrashDirs;

static int checks = 0, failed = 0;

static void expect(bool ok, const std::string& what) {
    ++checks;
    if (!ok) {
        ++failed;
        std::printf("FAIL %s\n", what.c_str());
    }
}

static void expectUnescape(const std::string& in, const std::string& want) {
    std::string got;
    try {
        got = unescapeMountField(in);
    } catch (const std::exception& e) {
        ++checks;
        ++failed;
        std::printf("FAIL unescape [%s] threw %s\n", in.c_str(), e.what());
        return;
    }
    expect(got == want, "unescape [" + in + "] = [" + got + "], want [" + want + "]");
}

int main() {
    // ---- /proc/mounts decoding ------------------------------------------
    expectUnescape(R"(foo\134bar)", R"(foo\bar)");          // the crash: the decoded \ was decoded again
    expectUnescape(R"(a\134123)", R"(a\123)");              // and this came out "aS"
    expectUnescape(R"(/mnt/a\134b)", R"(/mnt/a\b)");
    expectUnescape(R"(/run/media/u/My\040Disk)", "/run/media/u/My Disk");
    expectUnescape(R"(a\011b\012c)", "a\tb\nc");
    expectUnescape(R"(\134\134)", R"(\\)");                 // two escaped backslashes stay two
    expectUnescape(R"(x\134)", R"(x\)");                    // an escape at the very end
    expectUnescape(R"(x\)", R"(x\)");                       // a lone backslash is not an escape
    expectUnescape(R"(x\13)", R"(x\13)");                   // nor two digits
    expectUnescape(R"(x\999)", R"(x\999)");                 // nor non-octal digits
    expectUnescape(R"(x\400)", R"(x\400)");                 // nor a value past one byte
    expectUnescape("/plain/path", "/plain/path");

    {
        const auto M = parseMounts("/dev/sdb1 /run/media/u/foo\\134bar vfat rw,nosuid 0 0\n"
                                   "proc /proc proc rw 0 0\n"
                                   "short line\n"
                                   "\n"
                                   "/dev/sdc1 /run/media/u/My\\040Disk ext4 rw 0 0"); // no final newline
        expect(M.size() == 3, "parseMounts: three rows (a short line and a blank one skipped)");
        expect(M.size() == 3 && M[0].device == "/dev/sdb1" && M[0].dir == R"(/run/media/u/foo\bar)" && M[0].type == "vfat", "parseMounts: row 1 decoded");
        expect(M.size() == 3 && M[2].dir == "/run/media/u/My Disk" && M[2].type == "ext4", "parseMounts: the last row without a newline");
    }

    std::printf("trash_dirs_test: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
