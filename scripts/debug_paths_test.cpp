// Host test for DebugPaths.hpp: debug switches and logs only in a private
// runtime directory of ours, switches only when ours, logs always 0600
// (marketplace review, #11392).
#include "../DebugPaths.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

using namespace ScrollOverview;

static int g_checks = 0, g_failed = 0;
#define CHECK(c)                                                                                                                                                                   \
    do {                                                                                                                                                                           \
        ++g_checks;                                                                                                                                                                \
        if (!(c)) {                                                                                                                                                                \
            ++g_failed;                                                                                                                                                            \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);                                                                                                               \
        }                                                                                                                                                                          \
    } while (0)

static mode_t modeOf(const std::string& p) {
    struct stat st{};
    return ::lstat(p.c_str(), &st) == 0 ? (st.st_mode & 0777) : 0;
}

int main() {
    const uid_t ME = ::getuid(), OTHER = ME + 1;
    char        tmpl[] = "/tmp/debug_paths_test.XXXXXX";
    const auto  DIR    = std::string{::mkdtemp(tmpl)}; // 0700, ours
    CHECK(!DIR.empty() && modeOf(DIR) == 0700);

    // The directory: ours and private, or none -- never /tmp.
    CHECK(DebugPaths::dirFor(DIR.c_str()) == DIR);
    CHECK(DebugPaths::dirFor((DIR + "/").c_str()) == DIR);
    CHECK(DebugPaths::dirFor(nullptr).empty());
    CHECK(DebugPaths::dirFor("").empty());
    CHECK(DebugPaths::dirFor("relative/dir").empty());
    CHECK(DebugPaths::dirFor((DIR + "/missing").c_str()).empty());
    CHECK(DebugPaths::dirFor("/tmp").empty()); // shared: others can create in it
    CHECK(DebugPaths::dirFor(DIR.c_str(), OTHER).empty()); // another account's directory
    {
        const auto OPEN = DIR + "/open";
        std::filesystem::create_directory(OPEN);
        ::chmod(OPEN.c_str(), 0755);
        CHECK(DebugPaths::dirFor(OPEN.c_str()).empty()); // others can enter it
        const auto LINK = DIR + "/link";
        std::filesystem::create_directory_symlink(DIR, LINK);
        CHECK(DebugPaths::dirFor(LINK.c_str()).empty()); // a symlink is not the directory
        const auto FILE = DIR + "/a-file";
        std::fclose(std::fopen(FILE.c_str(), "w"));
        CHECK(DebugPaths::dirFor(FILE.c_str()).empty());
    }
    CHECK(DebugPaths::pathIn("", "x.log").empty());
    CHECK(DebugPaths::openLogIn("", "x.log") == nullptr); // no directory: no log at all

    // A switch: a regular file of ours nobody else can write. A flag created by
    // ANOTHER account (its uid in the stat) never counts.
    struct stat st{};
    st.st_mode = S_IFREG | 0644;
    st.st_uid  = ME;
    CHECK(DebugPaths::ownFlag(st, ME));
    st.st_uid = OTHER;
    CHECK(!DebugPaths::ownFlag(st, ME));
    st.st_uid  = ME;
    st.st_mode = S_IFREG | 0666;
    CHECK(!DebugPaths::ownFlag(st, ME)); // others could swap its meaning
    st.st_mode = S_IFLNK | 0777;
    CHECK(!DebugPaths::ownFlag(st, ME));
    st.st_mode = S_IFDIR | 0700;
    CHECK(DebugPaths::privateDir(st, ME) && !DebugPaths::privateDir(st, OTHER));

    ::umask(022);
    const auto FLAG = DIR + "/hyprdrive-test.on";
    CHECK(!DebugPaths::flagOnIn(DIR, "hyprdrive-test.on"));
    std::fclose(std::fopen(FLAG.c_str(), "w")); // `touch`: 0644 under umask 022
    CHECK(DebugPaths::flagOnIn(DIR, "hyprdrive-test.on"));
    CHECK(!DebugPaths::flagOnIn(DIR, "hyprdrive-test.on", OTHER)); // as seen by another uid's check
    ::chmod(FLAG.c_str(), 0666);
    CHECK(!DebugPaths::flagOnIn(DIR, "hyprdrive-test.on"));
    ::unlink(FLAG.c_str());
    ::symlink((DIR + "/a-file").c_str(), FLAG.c_str());
    CHECK(!DebugPaths::flagOnIn(DIR, "hyprdrive-test.on")); // a symlink is not a switch

    // Logs: created 0600 even under umask 022; an existing wider log narrowed
    // before anything is written; never through a symlink.
    if (FILE* F = DebugPaths::openLogIn(DIR, "fresh.log")) {
        std::fputs("x\n", F);
        std::fclose(F);
    }
    CHECK(modeOf(DIR + "/fresh.log") == 0600);
    {
        const auto OLD = DIR + "/old.log";
        std::fclose(std::fopen(OLD.c_str(), "w"));
        ::chmod(OLD.c_str(), 0644);
        FILE* F = DebugPaths::openLogIn(DIR, "old.log");
        CHECK(F != nullptr && modeOf(OLD) == 0600);
        if (F)
            std::fclose(F);
        FILE* T = DebugPaths::openLogIn(DIR, "old.log", false);
        CHECK(T != nullptr && modeOf(OLD) == 0600);
        if (T)
            std::fclose(T);
    }
    {
        const auto TARGET = DIR + "/target";
        std::fclose(std::fopen(TARGET.c_str(), "w"));
        ::chmod(TARGET.c_str(), 0644);
        ::symlink(TARGET.c_str(), (DIR + "/link.log").c_str());
        CHECK(DebugPaths::openLogIn(DIR, "link.log") == nullptr);
        CHECK(modeOf(TARGET) == 0644); // untouched
    }

    // The old /tmp files: ours (regular) are removed -- an old 0644 log with
    // query text in it above all; a symlink by the same name, and its target,
    // are left alone; so is a directory; another account's never counts.
    {
        const auto OLDDIR = DIR + "/oldtmp";
        std::filesystem::create_directory(OLDDIR);
        const auto PROF = OLDDIR + "/hyprdrive-prof.log", STACK = OLDDIR + "/hyprdrive-stack.log";
        for (const auto& P : {PROF, STACK}) {
            FILE* F = std::fopen(P.c_str(), "w");
            std::fputs("q=secret query\n", F);
            std::fclose(F);
            ::chmod(P.c_str(), 0644);
        }
        const auto TARGET = DIR + "/precious";
        std::fclose(std::fopen(TARGET.c_str(), "w"));
        ::chmod(TARGET.c_str(), 0644);
        ::symlink(TARGET.c_str(), (OLDDIR + "/ovdbg.log").c_str());
        std::filesystem::create_directory(OLDDIR + "/hyprdrive-click.log");
        CHECK(DebugPaths::removeLegacyIn(OLDDIR) == 2);
        CHECK(!std::filesystem::exists(PROF) && !std::filesystem::exists(STACK));
        CHECK(std::filesystem::is_symlink(OLDDIR + "/ovdbg.log") && modeOf(TARGET) == 0644 && std::filesystem::exists(TARGET));
        CHECK(std::filesystem::is_directory(OLDDIR + "/hyprdrive-click.log"));
        // Another account's old log: its stat never qualifies.
        struct stat other{};
        other.st_mode = S_IFREG | 0644;
        other.st_uid  = OTHER;
        CHECK(!DebugPaths::legacyRemovable(other, ME));
        other.st_uid = ME;
        CHECK(DebugPaths::legacyRemovable(other, ME));
        other.st_mode = S_IFLNK | 0777;
        CHECK(!DebugPaths::legacyRemovable(other, ME));
    }

    // The cached one follows the environment it was first asked under.
    ::setenv("XDG_RUNTIME_DIR", DIR.c_str(), 1);
    CHECK(DebugPaths::path("hyprdrive-compat.log") == DIR + "/hyprdrive-compat.log");

    std::filesystem::remove_all(DIR);
    std::printf("debug_paths_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
