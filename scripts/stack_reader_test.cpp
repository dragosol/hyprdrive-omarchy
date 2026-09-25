// StackReader's directory read: caps, hidden files, directories, birth time.
#include "../StackReader.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

static int g_fail = 0, g_checks = 0;
static void check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        ++g_fail;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

int main() {
    namespace fs = std::filesystem;
    const auto DIR = fs::temp_directory_path() / ("stack_reader_test_" + std::to_string(::getpid()));
    fs::create_directories(DIR / "sub");
    std::ofstream(DIR / ".hidden") << "x";
    std::ofstream(DIR / "a.png") << "x";
    std::ofstream(DIR / "b.txt") << "x";

    auto L = ScrollOverview::StackReader::readNow(DIR.string());
    check(L.ready && !L.truncated && !L.missing, "small dir ready");
    check(L.entries.size() == 3, "hidden skipped: " + std::to_string(L.entries.size()));
    bool sawDir = false, sawPng = false;
    for (const auto& E : L.entries) {
        sawDir |= E.name == "sub" && E.dir && E.kind == "inode/directory";
        sawPng |= E.name == "a.png" && E.kind == "image/png" && E.btime > 0 && E.mtime > 0;
    }
    check(sawDir, "sub is a dir");
    check(sawPng, "a.png has kind + times");

    // Past the cap: 6 000 files -> 5 000 entries, truncated.
    const auto BIG = DIR / "big";
    fs::create_directories(BIG);
    for (int i = 0; i < 6000; ++i)
        std::ofstream(BIG / ("f" + std::to_string(i))) << "";
    auto B = ScrollOverview::StackReader::readNow(BIG.string());
    check(B.entries.size() == ScrollOverview::StackReader::MAX_ENTRIES && B.truncated, "cap at 5000");

    auto M = ScrollOverview::StackReader::readNow((DIR / "nope").string());
    check(M.ready && M.missing && M.entries.empty(), "missing dir");

    // Dock stacks B: move into a folder, never overwriting, never into itself.
    const auto DEST = DIR / "dest";
    fs::create_directories(DEST);
    std::ofstream(DEST / "one.txt") << "old";
    std::ofstream(DIR / "one.txt") << "new";
    std::ofstream(DIR / "two.txt") << "two";
    auto R = ScrollOverview::StackReader::moveOrCopyNow(DEST.string(), {(DIR / "one.txt").string(), (DIR / "two.txt").string()});
    check(R.error.empty() && R.moved == 2, "moved 2: " + R.error);
    check(fs::exists(DEST / "one (2).txt") && fs::exists(DEST / "two.txt") && !fs::exists(DIR / "one.txt"), "conflict renamed, sources gone");
    std::ifstream kept(DEST / "one.txt");
    std::string   keptText;
    kept >> keptText;
    check(keptText == "old", "the existing file was not overwritten");
    auto SELF = ScrollOverview::StackReader::moveOrCopyNow((DEST).string(), {DIR.string()});
    check(!SELF.error.empty() && SELF.moved == 0, "a folder into its own child refused");

    fs::remove_all(DIR);
    std::printf("stack_reader_test: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
