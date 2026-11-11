// StoreFile.hpp: the stacks store is replaced whole or not at all (review
// 2026-11-09). The old write let a full disk rename an empty temp file over
// `stacks`. Here the disk "fills" two ways -- a file size limit that cuts the
// write short (EFBIG), and the temp file landing on /dev/full (ENOSPC) -- and
// the old file must come through byte for byte, with no temp file left.
#include "StoreFile.hpp"

#include <csignal>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;
using Hyprdrive::StoreFile::replace;

static int checks = 0, failed = 0;

static void expect(bool ok, const std::string& what) {
    ++checks;
    if (!ok) {
        ++failed;
        std::printf("FAIL %s\n", what.c_str());
    }
}

// The file's bytes, or "<not a regular file>" -- never read through a link
// (a renamed /dev/full link would read zeros forever).
static std::string contents(const fs::path& p) {
    std::error_code ec;
    if (!fs::is_regular_file(fs::symlink_status(p, ec)))
        return "<not a regular file>";
    std::ifstream     f(p, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

int main() {
    std::string made = (fs::temp_directory_path() / "store_file_test.XXXXXX").string();
    if (!mkdtemp(made.data())) {
        std::printf("FAIL mkdtemp\n");
        return 1;
    }
    const fs::path    DIR  = made;
    const fs::path    PATH = DIR / "stacks";
    const fs::path    TMP  = DIR / "stacks.tmp";
    const std::string OLD  = "# hyprdrive stacks v2\nseeded\nfan\tname\tauto\t\t/home/u/Downloads\nfan\tname\tauto\t\t/home/u/Projects\n";
    const std::string NEW  = OLD + "fan\tname\tauto\t\t/home/u/Pictures\n";

    // The ordinary save.
    expect(replace(PATH.string(), OLD) == 0, "a save on a healthy disk succeeds");
    expect(contents(PATH) == OLD, "and the file holds the text");
    expect(!fs::exists(fs::symlink_status(TMP)), "and no temp file is left");

    // The write cut short: past 16 bytes every write fails, as on a full disk.
    {
        std::signal(SIGXFSZ, SIG_IGN);
        rlimit was{};
        getrlimit(RLIMIT_FSIZE, &was);
        rlimit cap = was;
        cap.rlim_cur = 16;
        setrlimit(RLIMIT_FSIZE, &cap);
        const int ERR = replace(PATH.string(), NEW);
        setrlimit(RLIMIT_FSIZE, &was);
        expect(ERR != 0, "a short write is reported");
        expect(contents(PATH) == OLD, "a short write leaves the old file exactly as it was");
        expect(!fs::exists(fs::symlink_status(TMP)), "and removes its temp file");
    }

    // ENOSPC itself: the temp file is /dev/full.
    if (fs::exists("/dev/full")) {
        fs::create_symlink("/dev/full", TMP);
        const int ERR = replace(PATH.string(), NEW);
        expect(ERR == ENOSPC, "a full disk is reported as ENOSPC");
        expect(contents(PATH) == OLD, "a full disk leaves the old file exactly as it was");
        expect(!fs::exists(fs::symlink_status(TMP)), "and removes its temp file");
    }

    // A state directory that cannot be written: nothing is touched.
    if (geteuid() != 0) {
        ::chmod(DIR.c_str(), 0500);
        const int ERR = replace(PATH.string(), NEW);
        ::chmod(DIR.c_str(), 0700);
        expect(ERR == EACCES, "an unwritable folder is reported");
        expect(contents(PATH) == OLD, "and the old file stays");
    }

    // And the disk has room again: the next save goes through.
    expect(replace(PATH.string(), NEW) == 0 && contents(PATH) == NEW, "the next save after a failure succeeds");

    std::error_code ec;
    fs::remove_all(DIR, ec);
    std::printf("store_file_test: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
