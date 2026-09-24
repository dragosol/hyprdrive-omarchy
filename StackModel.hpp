#pragma once

// THE STACKS' PURE CORE (dock stacks A, 2026-09-24): sorting, filtering, the
// store's text format, the fan's fit decision and relative ages. No Hyprland,
// no GLib -- scripts/stack_model_test.cpp builds it on the host.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ScrollOverview::StackModel {

    enum class ESort : uint8_t { Name, Added, Modified, Kind };
    enum class EDisplay : uint8_t { Stack, Folder };
    enum class EView : uint8_t { Auto, Grid };

    // One item of a folder. Times are unix seconds. `kind` is a content type
    // ("image/png", "inode/directory"); `icon` a themed icon name.
    struct SEntry {
        std::string path, name, kind, icon;
        bool        dir   = false;
        int64_t     mtime = 0;
        int64_t     btime = 0; // birth time, or the change time where the filesystem has none
    };

    // Digit runs compare as numbers, everything else ASCII-case-insensitively.
    bool naturalLess(const std::string& a, const std::string& b);

    // Directories first, then: Name (natural), Added (btime, newest first),
    // Modified (mtime, newest first), Kind (content type, then name). Stable.
    void sortEntries(std::vector<SEntry>& entries, ESort sort);

    // Indices of the entries whose NAME contains `query`: ASCII letters
    // case-insensitively, every other byte exactly. Empty query = all.
    std::vector<size_t> filter(const std::vector<SEntry>& entries, const std::string& query);

    const char*          sortName(ESort s); // "name" | "added" | "modified" | "kind"
    std::optional<ESort> sortFromName(const std::string& s);

    // ---- the store -------------------------------------------------------
    //   # hyprdrive stacks v2
    //   seeded<TAB>1
    //   stack<TAB>sort<TAB>display<TAB>view<TAB>label<TAB>path      (path LAST: it may hold tabs)
    // v1 lines are `fan|grid<TAB>name|mtime<TAB>path`. Order in the file is dock order.
    struct SRecord {
        std::string path;
        ESort       sort    = ESort::Name;
        EDisplay    display = EDisplay::Stack;
        EView       view    = EView::Auto;
        std::string label; // empty = the folder's basename
    };
    struct SStore {
        int                  version  = 2;
        bool                 seeded   = false;
        bool                 readOnly = false; // written by a newer build: never write back
        std::vector<SRecord> records;
    };
    SStore      parseStore(const std::string& text);
    std::string formatStore(const SStore& store);

    // First run (no file) seeds ~/Downloads; anything else -- a v1 file, or a
    // v2 file that already seeded -- does not.
    bool shouldSeed(bool fileExists, const SStore& store);

    // ---- the fan's fit ----------------------------------------------------
    // All in LOGICAL px, y growing downward. The fan's top is
    // pillTopL - gapL - rows * stepL; it fits when that is at least one step
    // below topLimitL (the bar's bottom, or the monitor top).
    struct SFanFit {
        float pillTopL  = 0.F;
        float topLimitL = 0.F;
        float stepL     = 56.F;
        float gapL      = 10.F;
    };
    bool fanFits(size_t rows, const SFanFit& fit);

    // "just now" (<60s), "5m ago", "2h ago", "3d ago", "10w ago".
    std::string relativeAge(int64_t nowSec, int64_t thenSec);
}
