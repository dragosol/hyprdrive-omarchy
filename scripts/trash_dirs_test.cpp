// TrashDirs.hpp: /proc/mounts decoding, and the trash walk Empty Trash uses
// when there is no Nautilus (review 2026-11-09), against real directories.
//
//   - a label with a backslash (`foo\134bar`) threw std::invalid_argument on
//     the render thread, and `a\134123` was decoded twice;
//   - a disk whose `.Trash-UID/files` is a symlink had Empty Trash delete what
//     it pointed to. Every canary here must survive emptying.
#include "../src/TrashDirs.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;
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

static void touch(const fs::path& p) {
    std::ofstream(p) << "x";
}

static size_t entries(const fs::path& dir) {
    std::error_code ec;
    size_t          n = 0;
    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
        ++n;
    return n;
}

// Empty every trash on the "disk" mounted at `top` exactly the way
// Locations::emptyTrash does: discover, then empty each one found.
static void emptyVolume(const fs::path& top, uid_t uid) {
    for (const auto& T : volumeTrashes(top.string(), uid))
        empty(T, false, uid);
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

    // ---- the trash walk, on real directories ------------------------------
    const uid_t UID  = getuid();
    const auto  ID   = std::to_string(UID);
    std::string made = (fs::temp_directory_path() / "trash_dirs_test.XXXXXX").string();
    if (!mkdtemp(made.data())) {
        std::printf("FAIL mkdtemp\n");
        return 1;
    }
    const fs::path R = made;

    // The canary: a folder of the user's that a hostile disk points at.
    const auto CANARY = R / "home";
    fs::create_directories(CANARY / "Documents");
    touch(CANARY / "Documents" / "thesis.txt");
    touch(CANARY / "notes.txt");
    const auto canaryWhole = [&] { return fs::exists(CANARY / "Documents" / "thesis.txt") && fs::exists(CANARY / "notes.txt"); };

    // 1. files/ IS the link (the review's case): not a trash at all, and even
    //    handed to empty() directly nothing behind the link is touched.
    {
        const auto TOP = R / "disk1";
        const auto T   = TOP / (".Trash-" + ID);
        fs::create_directories(T / "info");
        touch(T / "info" / "x.trashinfo");
        fs::create_symlink(CANARY, T / "files");
        expect(volumeTrashes(TOP.string(), UID).empty(), "files/ a symlink: not a trash");
        emptyVolume(TOP, UID);
        expect(canaryWhole(), "files/ a symlink: emptying the disk leaves its target alone");
        empty(T.string(), false, UID);
        expect(canaryWhole(), "files/ a symlink: empty() itself never goes through it");
        expect(fs::is_symlink(T / "files"), "files/ a symlink: the link itself is left too");
    }

    // 2. The trash directory itself is the link.
    {
        const auto TOP  = R / "disk2";
        const auto REAL = R / "elsewhere";
        fs::create_directories(TOP);
        fs::create_directories(REAL / "files");
        touch(REAL / "files" / "keep.txt");
        fs::create_symlink(REAL, TOP / (".Trash-" + ID));
        expect(volumeTrashes(TOP.string(), UID).empty(), ".Trash-UID a symlink: not a trash");
        empty((TOP / (".Trash-" + ID)).string(), false, UID);
        expect(fs::exists(REAL / "files" / "keep.txt"), ".Trash-UID a symlink: what it points to is untouched");
    }

    // 3. A real trash holding links and folders of links: every entry goes,
    //    every target stays.
    {
        const auto TOP = R / "disk3";
        const auto T   = TOP / (".Trash-" + ID);
        fs::create_directories(T / "files" / "folder" / "nested");
        fs::create_directories(T / "info");
        fs::create_directories(T / "expunged" / "old");
        touch(T / "files" / "plain.txt");
        touch(T / "files" / "folder" / "nested" / "deep.txt");
        fs::create_symlink(CANARY, T / "files" / "dirlink");
        fs::create_symlink(CANARY / "notes.txt", T / "files" / "filelink");
        fs::create_symlink(CANARY, T / "files" / "folder" / "inner");
        fs::create_symlink(CANARY / "Documents", T / "files" / "folder" / "nested" / "docs");
        touch(T / "info" / "plain.txt.trashinfo");
        touch(T / "expunged" / "old" / "gone");
        // A folder deeper than anything real: removed all the way down.
        fs::path deep = T / "files" / "deep";
        for (int i = 0; i < 100; ++i)
            deep /= "d";
        fs::create_directories(deep);
        touch(deep / "bottom");

        const auto FOUND = volumeTrashes(TOP.string(), UID);
        expect(FOUND.size() == 1 && FOUND[0] == T.string(), "a real .Trash-UID is found");
        emptyVolume(TOP, UID);
        expect(entries(T / "files") == 0, "a real trash: files/ emptied");
        expect(entries(T / "info") == 0, "a real trash: info/ emptied");
        expect(entries(T / "expunged") == 0, "a real trash: expunged/ emptied");
        expect(canaryWhole(), "a real trash: no link inside it was followed");
        expect(volumeTrashes(TOP.string(), UID).size() == 1, "a real trash: still a trash after emptying");
    }

    // 4. Someone else's (here: asked for another uid) is never this user's.
    {
        const auto TOP = R / "disk4";
        fs::create_directories(TOP / (".Trash-" + ID) / "files");
        touch(TOP / (".Trash-" + ID) / "files" / "theirs");
        expect(volumeTrashes(TOP.string(), UID + 1).empty(), "a trash owned by another user is not ours");
        empty((TOP / (".Trash-" + ID)).string(), false, UID + 1);
        expect(fs::exists(TOP / (".Trash-" + ID) / "files" / "theirs"), "and empty() leaves it alone");
    }

    // 5. The shared $top/.Trash counts only with its sticky bit.
    {
        const auto TOP = R / "disk5";
        const auto T   = TOP / ".Trash" / ID;
        fs::create_directories(T / "files");
        ::chmod((TOP / ".Trash").c_str(), 0777);
        expect(volumeTrashes(TOP.string(), UID).empty(), "$top/.Trash without the sticky bit: not used");
        ::chmod((TOP / ".Trash").c_str(), 01777);
        const auto FOUND = volumeTrashes(TOP.string(), UID);
        expect(FOUND.size() == 1 && FOUND[0] == T.string(), "$top/.Trash with the sticky bit: $top/.Trash/UID used");
    }

    // 6. The home trash is the user's own: behind a symlink (moved to another
    //    disk) it still empties -- and links inside it are still not followed.
    {
        const auto REAL = R / "bigdisk" / "Trash";
        fs::create_directories(REAL / "files");
        fs::create_directories(REAL / "info");
        touch(REAL / "files" / "old.txt");
        fs::create_symlink(CANARY, REAL / "files" / "homelink");
        fs::create_directories(R / "share");
        fs::create_symlink(REAL, R / "share" / "Trash");
        empty((R / "share" / "Trash").string(), true, UID);
        expect(entries(REAL / "files") == 0, "home trash behind a symlink: emptied");
        expect(canaryWhole(), "home trash: a link inside it was not followed");
    }

    std::error_code ec;
    fs::remove_all(R, ec);

    std::printf("trash_dirs_test: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
