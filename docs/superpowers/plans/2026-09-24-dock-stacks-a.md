# Dock Stacks A — the stack experience — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Turn hyprdrive's dock stacks into macOS-style stacks: a fan that fits the screen, a searchable pane when it does not, Space previews with a highlighted item, a 4-item pile on the dock cell, a full context menu, stack order and a default Downloads stack.

**Architecture:** `DockStacks.cpp` stays the owner (store, open/close, hit-test, Sushi). New focused units: `StackModel` (pure: sort, filter, store v2 format, fit decision, relative age — host-tested), `StackReader` (worker thread: folder listing, trash jobs, inotify watches), `TextInput` (typing logic shared with the overview search), and pane/pile drawing inside `DockStacks.cpp` split into `StackPane.cpp` and `StackIcon.cpp`. Everything draws through the theme plate and runs off the compositor thread where it touches the disk.

**Tech Stack:** C++23 Hyprland 0.56.2 plugin, GIO/GLib, xkbcommon, Pango/Cairo, inotify, Hyprland render pass elements. Build: `make all` (plugin + symcheck + keycheck + host tests), `make install`.

**Spec:** `docs/superpowers/specs/2026-09-24-dock-stacks-a-design.md`

## Global Constraints

- Every plate: `Dock::frostedPlate` / `OverviewRender::themePlate` (frost only when `decoration:blur:enabled`, theme dock tint, Hyprland rounding). Text colours from `Config::themeForeground()`, `themeMutedText()`, `themeHeading()`, accent from `general:col.active_border`. No hard-coded palette.
- The compositor thread never reads a directory, stats a folder's children, or trashes files. Those run in `StackReader`'s worker.
- Nothing previews on hover alone; Sushi opens only on Space.
- Every timer, watch, thread and Sushi window a unit arms is removed in PLUGIN_EXIT (`main.cpp`, beside `ScratchpadExit::shutdown()`).
- Every symbol declared in a header is defined OUTSIDE anonymous namespaces (`scripts/symcheck.sh`).
- Fan: no fixed item cap; fits = every row below the top bar with one row-step margin.
- Pane: ≤ 6 columns, ≤ 60% monitor width, ≤ 55% monitor height. Background read cap: 5 000 entries.
- Pile: 4 newest items. Previewed item: 1.15× with an accent ring.
- Store: `~/.local/state/hyprdrive/stacks`, header `# hyprdrive stacks v2`; v1 read and upgraded; atomic write (temp + rename); a newer version is read-only.
- Dock order: apps | Scratchpad | stacks | removable drives | Trash.
- Trash operations use `g_file_trash`; nothing is ever deleted outright.
- Live testing: never inject Super+W/close keys; close the overview with `do overview close`; keep `down … up` in one `act` call; never type while a password prompt is on screen.

## Review Focus

1. **A huge or slow folder** (tens of thousands of files, a network mount): the dock and compositor must stay smooth; the pane shows "Loading…" then the first 5 000 with "showing first 5 000". → test in Task 4 (6 000-file temp dir).
2. **A pinned folder deleted or renamed** (while closed, and while its pane is open): the cell dims with "Folder not found", an open pane closes, nothing crashes, the inotify watch is dropped. → live check in Task 8.
3. **Odd filenames and paths** (non-ASCII, a tab in the path, 300-char names): the store round-trips them and the fan/pane elide rather than overflow. → store round-trip test in Task 1, render check in Task 7.
4. **Keyboard capture leaking** (pane open, then the overview opens, the pointer leaves, or the pane closes): keys return to the focused window the moment the pane closes; Space is never swallowed outside a fan/pane. → live check in Task 7.
5. **A store written by a newer build, or garbled**: nothing is written back, no stacks are lost, no crash. → parse test in Task 1.

---

### Task 1: StackModel — the pure core (sort, filter, store v2, fit, age)

**Files:**
- Create: `StackModel.hpp`, `StackModel.cpp`
- Create: `scripts/stack_model_test.cpp`
- Modify: `scripts/host_tests.sh` (add the suite)

**Interfaces:**
- Consumes: nothing (std only — no Hyprland, no GLib, so it builds in the host tests).
- Produces:
  ```cpp
  namespace ScrollOverview::StackModel {
      enum class ESort : uint8_t { Name, Added, Modified, Kind };
      enum class EDisplay : uint8_t { Stack, Folder };
      enum class EView : uint8_t { Auto, Grid };
      struct SEntry { std::string path, name, kind, icon; bool dir = false; int64_t mtime = 0, btime = 0; };
      bool                naturalLess(const std::string& a, const std::string& b);
      void                sortEntries(std::vector<SEntry>& entries, ESort sort);
      std::vector<size_t> filter(const std::vector<SEntry>& entries, const std::string& query);
      const char*         sortName(ESort s);
      std::optional<ESort> sortFromName(const std::string& s);
      struct SRecord { std::string path; ESort sort = ESort::Name; EDisplay display = EDisplay::Stack; EView view = EView::Auto; std::string label; };
      struct SStore  { int version = 2; bool seeded = false; bool readOnly = false; std::vector<SRecord> records; };
      SStore      parseStore(const std::string& text);
      std::string formatStore(const SStore& store);
      bool        shouldSeed(bool fileExists, const SStore& store);
      struct SFanFit { float pillTopL = 0, topLimitL = 0, stepL = 56, gapL = 10; };
      bool        fanFits(size_t rows, const SFanFit& fit);
      std::string relativeAge(int64_t nowSec, int64_t thenSec);
  }
  ```

- [ ] **Step 1: Write the failing test** — `scripts/stack_model_test.cpp`:

```cpp
// StackModel: the stacks' pure core -- sort, filter, the store format, the fan's
// fit decision, relative ages. No compositor, no GLib.
#include "../StackModel.hpp"

#include <cstdio>
#include <string>
#include <vector>

using namespace ScrollOverview::StackModel;

static int g_fail = 0, g_checks = 0;
static void check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        ++g_fail;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

static std::vector<SEntry> sample() {
    return {
        {.path = "/d/b10.txt", .name = "b10.txt", .kind = "text/plain", .mtime = 50, .btime = 5},
        {.path = "/d/B2.txt", .name = "B2.txt", .kind = "text/plain", .mtime = 70, .btime = 9},
        {.path = "/d/a.png", .name = "a.png", .kind = "image/png", .mtime = 60, .btime = 7},
        {.path = "/d/zdir", .name = "zdir", .kind = "inode/directory", .dir = true, .mtime = 10, .btime = 1},
        {.path = "/d/Café.md", .name = "Café.md", .kind = "text/markdown", .mtime = 20, .btime = 3},
    };
}

static std::string names(const std::vector<SEntry>& e) {
    std::string out;
    for (const auto& x : e)
        out += (out.empty() ? "" : ",") + x.name;
    return out;
}

int main() {
    // naturalLess: digits compare as numbers, letters case-insensitively.
    check(naturalLess("b2", "b10"), "b2 < b10");
    check(!naturalLess("b10", "b2"), "!(b10 < b2)");
    check(naturalLess("apple", "Zebra"), "apple < Zebra");
    check(!naturalLess("a", "a"), "irreflexive");

    // Sorts: directories first, then the key.
    auto e = sample();
    sortEntries(e, ESort::Name);
    check(names(e) == "zdir,a.png,B2.txt,b10.txt,Café.md", "name: " + names(e));
    sortEntries(e, ESort::Modified);
    check(names(e) == "zdir,B2.txt,a.png,b10.txt,Café.md", "modified: " + names(e));
    sortEntries(e, ESort::Added);
    check(names(e) == "zdir,B2.txt,a.png,b10.txt,Café.md", "added: " + names(e));
    sortEntries(e, ESort::Kind);
    check(names(e) == "zdir,a.png,Café.md,B2.txt,b10.txt", "kind: " + names(e));

    // Filter: ASCII case-insensitive substring on the name; other bytes exact.
    e = sample();
    check(filter(e, "").size() == 5, "empty query keeps all");
    check(filter(e, "TXT").size() == 2, "TXT matches 2");
    check(filter(e, "café").size() == 1, "café matches Café");
    check(filter(e, "é").size() == 1, "é matches");
    check(filter(e, "nothing").empty(), "no match");

    // Sort names round-trip.
    for (auto s : {ESort::Name, ESort::Added, ESort::Modified, ESort::Kind})
        check(sortFromName(sortName(s)) == s, std::string("sort name ") + sortName(s));
    check(!sortFromName("bogus"), "bogus sort name");

    // Store v1 upgrade.
    auto v1 = parseStore("# hyprdrive stacks v1\ngrid\tmtime\t/home/u/Downloads\nfan\tname\t/home/u/Docs\n");
    check(v1.records.size() == 2 && !v1.readOnly, "v1 two records");
    check(v1.records[0].view == EView::Grid && v1.records[0].sort == ESort::Modified, "v1 grid+mtime -> Grid+Modified");
    check(v1.records[1].view == EView::Auto && v1.records[1].sort == ESort::Name, "v1 fan+name -> Auto+Name");
    check(!v1.seeded, "v1 not seeded");

    // Store v2 round-trip, including a tab and non-ASCII in the path and label.
    SStore s;
    s.seeded = true;
    s.records.push_back({.path = "/home/u/Down\tloads", .sort = ESort::Added, .display = EDisplay::Folder, .view = EView::Grid, .label = "Téléchargements"});
    s.records.push_back({.path = "/home/u/Pictures", .sort = ESort::Kind});
    const auto OUT = formatStore(s);
    const auto BACK = parseStore(OUT);
    check(BACK.seeded && BACK.records.size() == 2, "v2 round-trip count+seeded");
    check(BACK.records[0].path == "/home/u/Down\tloads" && BACK.records[0].label == "Téléchargements", "v2 path/label with tab + unicode");
    check(BACK.records[0].display == EDisplay::Folder && BACK.records[0].view == EView::Grid && BACK.records[0].sort == ESort::Added, "v2 fields");
    check(BACK.records[1].sort == ESort::Kind && BACK.records[1].label.empty(), "v2 second record");

    // A newer version is read-only and yields nothing; garbage lines are skipped.
    auto v9 = parseStore("# hyprdrive stacks v9\nstack\tname\tstack\tauto\t\t/x\n");
    check(v9.readOnly && v9.records.empty(), "newer version read-only");
    auto junk = parseStore("# hyprdrive stacks v2\nnonsense line\nstack\tname\n\nstack\tname\tstack\tauto\t\t/ok\n");
    check(junk.records.size() == 1 && junk.records[0].path == "/ok", "junk skipped");

    // Seeding: only on a first run.
    check(shouldSeed(false, SStore{}), "no file -> seed");
    SStore seeded;
    seeded.seeded = true;
    check(!shouldSeed(true, seeded), "seeded -> no");
    check(!shouldSeed(true, v1), "existing v1 file -> no seed (user already has stacks)");

    // Fan fit: pill top at 900, top limit (bar bottom) at 30, step 56, gap 10.
    // Top of an N-row fan = 900 - 10 - N*56; needs >= 30 + 56 (one step margin).
    for (int h : {720, 900, 1080, 1440, 2160}) {
        SFanFit f{.pillTopL = float(h) - 70.F, .topLimitL = 30.F, .stepL = 56.F, .gapL = 10.F};
        size_t maxRows = 0;
        while (fanFits(maxRows + 1, f))
            ++maxRows;
        const float TOP = f.pillTopL - f.gapL - float(maxRows) * f.stepL;
        check(TOP >= f.topLimitL + f.stepL, "fit top clear at h=" + std::to_string(h));
        check(f.pillTopL - f.gapL - float(maxRows + 1) * f.stepL < f.topLimitL + f.stepL, "next row would not fit at h=" + std::to_string(h));
    }
    SFanFit noBar{.pillTopL = 830.F, .topLimitL = 0.F};
    SFanFit bar{.pillTopL = 830.F, .topLimitL = 30.F};
    size_t a = 0, b = 0;
    while (fanFits(a + 1, noBar))
        ++a;
    while (fanFits(b + 1, bar))
        ++b;
    check(a >= b, "a top bar never allows more rows");

    // Relative age.
    check(relativeAge(1000, 990) == "just now", "10s");
    check(relativeAge(10000, 10000 - 5 * 60) == "5m ago", "5m");
    check(relativeAge(100000, 100000 - 2 * 3600) == "2h ago", "2h");
    check(relativeAge(1000000, 1000000 - 3 * 86400) == "3d ago", "3d");
    check(relativeAge(100000000, 100000000 - 70 * 86400) == "10w ago", "10w");

    std::printf("stack_model_test: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
```

- [ ] **Step 2: Add the suite and run it to see it fail** — in `scripts/host_tests.sh`, after the `tool_search_test` line add:

```bash
run stack_model_test scripts/stack_model_test.cpp StackModel.cpp
```

Run: `bash scripts/host_tests.sh 2>&1 | grep stack_model`
Expected: `host_tests: stack_model_test FAILED TO BUILD` (StackModel.hpp missing).

- [ ] **Step 3: Write `StackModel.hpp`**

```cpp
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
```

- [ ] **Step 4: Write `StackModel.cpp`**

```cpp
#include "StackModel.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>

namespace ScrollOverview::StackModel {

    namespace {
        unsigned char lower(unsigned char c) {
            return (c >= 'A' && c <= 'Z') ? c + 32 : c;
        }

        std::vector<std::string> splitTabs(const std::string& line, size_t maxParts) {
            std::vector<std::string> out;
            size_t                   at = 0;
            while (out.size() + 1 < maxParts) {
                const auto NEXT = line.find('\t', at);
                if (NEXT == std::string::npos)
                    break;
                out.emplace_back(line.substr(at, NEXT - at));
                at = NEXT + 1;
            }
            out.emplace_back(line.substr(at));
            return out;
        }

        const char* displayName(EDisplay d) {
            return d == EDisplay::Folder ? "folder" : "stack";
        }
        const char* viewName(EView v) {
            return v == EView::Grid ? "grid" : "auto";
        }
    }

    bool naturalLess(const std::string& a, const std::string& b) {
        size_t i = 0, j = 0;
        while (i < a.size() && j < b.size()) {
            const unsigned char CA = a[i], CB = b[j];
            if (std::isdigit(CA) && std::isdigit(CB)) {
                size_t ie = i, je = j;
                while (ie < a.size() && std::isdigit(sc_uchar(a[ie])))
                    ++ie;
                while (je < b.size() && std::isdigit(sc_uchar(b[je])))
                    ++je;
                // Compare numerically without overflow: strip leading zeros, then length, then digits.
                size_t ia = i, jb = j;
                while (ia + 1 < ie && a[ia] == '0')
                    ++ia;
                while (jb + 1 < je && b[jb] == '0')
                    ++jb;
                const size_t LA = ie - ia, LB = je - jb;
                if (LA != LB)
                    return LA < LB;
                if (const int C = a.compare(ia, LA, b, jb, LB); C != 0)
                    return C < 0;
                i = ie;
                j = je;
                continue;
            }
            if (lower(CA) != lower(CB))
                return lower(CA) < lower(CB);
            ++i;
            ++j;
        }
        return (a.size() - i) < (b.size() - j);
    }

    void sortEntries(std::vector<SEntry>& entries, ESort sort) {
        std::ranges::stable_sort(entries, [sort](const SEntry& x, const SEntry& y) {
            if (x.dir != y.dir)
                return x.dir;
            switch (sort) {
                case ESort::Added:
                    if (x.btime != y.btime)
                        return x.btime > y.btime;
                    break;
                case ESort::Modified:
                    if (x.mtime != y.mtime)
                        return x.mtime > y.mtime;
                    break;
                case ESort::Kind:
                    if (x.kind != y.kind)
                        return x.kind < y.kind;
                    break;
                case ESort::Name: break;
            }
            return naturalLess(x.name, y.name);
        });
    }

    std::vector<size_t> filter(const std::vector<SEntry>& entries, const std::string& query) {
        std::vector<size_t> out;
        std::string         q;
        for (const unsigned char C : query)
            q += sc_char(lower(C));
        for (size_t i = 0; i < entries.size(); ++i) {
            if (q.empty()) {
                out.push_back(i);
                continue;
            }
            std::string n;
            for (const unsigned char C : entries[i].name)
                n += sc_char(lower(C));
            if (n.find(q) != std::string::npos)
                out.push_back(i);
        }
        return out;
    }

    const char* sortName(ESort s) {
        switch (s) {
            case ESort::Added: return "added";
            case ESort::Modified: return "modified";
            case ESort::Kind: return "kind";
            case ESort::Name: break;
        }
        return "name";
    }

    std::optional<ESort> sortFromName(const std::string& s) {
        for (auto v : {ESort::Name, ESort::Added, ESort::Modified, ESort::Kind})
            if (s == sortName(v))
                return v;
        return std::nullopt;
    }

    SStore parseStore(const std::string& text) {
        SStore             out;
        std::istringstream in(text);
        std::string        line;
        int                version = 1;
        bool               first   = true;
        while (std::getline(in, line)) {
            while (!line.empty() && (line.back() == '\r'))
                line.pop_back();
            if (line.empty())
                continue;
            if (first && line.starts_with("# hyprdrive stacks v")) {
                first   = false;
                version = std::atoi(line.c_str() + 20);
                if (version > 2) {
                    out.readOnly = true;
                    out.version  = version;
                    return out;
                }
                continue;
            }
            first = false;
            if (line[0] == '#')
                continue;

            if (version <= 1) {
                const auto P = splitTabs(line, 3);
                if (P.size() < 3 || P[2].empty())
                    continue;
                out.records.push_back(SRecord{.path = P[2], .sort = P[1] == "mtime" ? ESort::Modified : ESort::Name, .view = P[0] == "grid" ? EView::Grid : EView::Auto});
                continue;
            }

            if (line.starts_with("seeded\t")) {
                out.seeded = line.substr(7) == "1";
                continue;
            }
            if (!line.starts_with("stack\t"))
                continue;
            const auto P = splitTabs(line, 6);
            if (P.size() < 6 || P[5].empty())
                continue;
            out.records.push_back(SRecord{
                .path    = P[5],
                .sort    = sortFromName(P[1]).value_or(ESort::Name),
                .display = P[2] == "folder" ? EDisplay::Folder : EDisplay::Stack,
                .view    = P[3] == "grid" ? EView::Grid : EView::Auto,
                .label   = P[4],
            });
        }
        out.version = 2; // v1 is upgraded on the next write
        return out;
    }

    std::string formatStore(const SStore& store) {
        std::string out = "# hyprdrive stacks v2\n";
        out += std::string("seeded\t") + (store.seeded ? "1" : "0") + "\n";
        for (const auto& R : store.records) {
            // A tab or newline in the LABEL would break the record; the path may keep tabs (it is last).
            std::string label = R.label;
            std::ranges::replace(label, '\t', ' ');
            std::ranges::replace(label, '\n', ' ');
            out += std::string("stack\t") + sortName(R.sort) + "\t" + displayName(R.display) + "\t" + viewName(R.view) + "\t" + label + "\t" + R.path + "\n";
        }
        return out;
    }

    bool shouldSeed(bool fileExists, const SStore& store) {
        return !fileExists && !store.seeded;
    }

    bool fanFits(size_t rows, const SFanFit& fit) {
        const float TOP = fit.pillTopL - fit.gapL - static_cast<float>(rows) * fit.stepL;
        return TOP >= fit.topLimitL + fit.stepL;
    }

    std::string relativeAge(int64_t nowSec, int64_t thenSec) {
        const int64_t D = std::max<int64_t>(0, nowSec - thenSec);
        if (D < 60)
            return "just now";
        if (D < 3600)
            return std::to_string(D / 60) + "m ago";
        if (D < 86400)
            return std::to_string(D / 3600) + "h ago";
        if (D < 7 * 86400)
            return std::to_string(D / 86400) + "d ago";
        return std::to_string(D / (7 * 86400)) + "w ago";
    }
}
```

Add at the top of `StackModel.cpp`, after the includes, the two tiny casts it uses (the plugin's `sc<>` is a Hyprland macro and is not available on the host):

```cpp
namespace {
    inline unsigned char sc_uchar(char c) { return static_cast<unsigned char>(c); }
    inline char          sc_char(unsigned char c) { return static_cast<char>(c); }
}
```

(Place it before `namespace ScrollOverview::StackModel {` so the functions inside can use it.)

- [ ] **Step 5: Run the tests to verify they pass**

Run: `bash scripts/host_tests.sh 2>&1 | grep stack_model`
Expected: `host_tests: stack_model_test OK -- stack_model_test: 45 checks, 0 failed` (the count may differ by a few; 0 failed is the bar).

- [ ] **Step 6: Build the plugin to confirm the new file links cleanly**

Run: `make all 2>&1 | grep -E " error|FAIL|symcheck:"`
Expected: only `symcheck: OK …` (the Makefile globs `*.cpp`, so `StackModel.cpp` is compiled in; nothing calls it yet).

- [ ] **Step 7: Commit**

```bash
git add StackModel.hpp StackModel.cpp scripts/stack_model_test.cpp scripts/host_tests.sh
git commit -m "feat(stacks): StackModel -- sort, filter, store v2, fan fit, relative age (host-tested)"
```

---

### Task 2: TextInput — the typing logic, shared with the overview search

**Files:**
- Create: `TextInput.hpp`, `TextInput.cpp`, `TextInputUtf8.hpp`
- Modify: `main.cpp:602-705` (composeState / resetCompose / popCodepoint / searchKeyFor bodies)
- Test: `scripts/stack_model_test.cpp` (UTF-8 pop checks)

**Interfaces:**
- Consumes: xkbcommon.
- Produces:
  ```cpp
  namespace ScrollOverview::TextInput {
      struct SKey { bool consume = false; bool starts = false; std::string text; };
      SKey keyFor(xkb_state* state, uint32_t keycode);  // compose-aware; control keys -> {}
      void resetCompose();
  }
  // TextInputUtf8.hpp (header-only, host-testable):
  namespace ScrollOverview::TextInput { inline void popCodepoint(std::string& s); }
  ```

- [ ] **Step 1: Write the failing test** — append to `main()` in `scripts/stack_model_test.cpp`, before the final `printf`, and add `#include "../TextInputUtf8.hpp"` at the top:

```cpp
    // TextInput's Backspace removes one CODEPOINT, never half of one.
    {
        using ScrollOverview::TextInput::popCodepoint;
        std::string t = "caf\xc3\xa9";
        popCodepoint(t);
        check(t == "caf", "pop é");
        std::string emoji = "a\xf0\x9f\x98\x80";
        popCodepoint(emoji);
        check(emoji == "a", "pop emoji");
        std::string empty;
        popCodepoint(empty);
        check(empty.empty(), "pop empty");
    }
```

- [ ] **Step 2: Run to see it fail**

Run: `bash scripts/host_tests.sh 2>&1 | grep stack_model`
Expected: `FAILED TO BUILD` (TextInputUtf8.hpp missing).

- [ ] **Step 3: Create `TextInputUtf8.hpp`**

```cpp
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
```

- [ ] **Step 4: Create `TextInput.hpp` and `TextInput.cpp`** — moved verbatim from `main.cpp`'s `composeState`, `resetCompose` and the body of `searchKeyFor` (minus the guard-key check, which stays in main.cpp):

`TextInput.hpp`:
```cpp
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
```

`TextInput.cpp`:
```cpp
#include "TextInput.hpp"

#include <cstdlib>
#include <xkbcommon/xkbcommon-compose.h>

namespace {
    // Built once from the locale; null when there is no Compose file, and every
    // path below then falls through to the plain utf8 one. Never freed: it lives
    // as long as the plugin, and freeing it at unload would race the key hook.
    xkb_compose_state* composeState() {
        static xkb_compose_state* STATE = []() -> xkb_compose_state* {
            const char* LOCALE = nullptr;
            for (const char* VAR : {"LC_ALL", "LC_CTYPE", "LANG"}) {
                if (const char* V = getenv(VAR); V && *V) {
                    LOCALE = V;
                    break;
                }
            }
            if (!LOCALE)
                LOCALE = "C";
            xkb_context* CTX = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
            if (!CTX)
                return nullptr;
            xkb_compose_table* TABLE = xkb_compose_table_new_from_locale(CTX, LOCALE, XKB_COMPOSE_COMPILE_NO_FLAGS);
            if (!TABLE)
                return nullptr;
            xkb_compose_state* S = xkb_compose_state_new(TABLE, XKB_COMPOSE_STATE_NO_FLAGS);
            xkb_compose_table_unref(TABLE);
            return S;
        }();
        return STATE;
    }
}

namespace ScrollOverview::TextInput {

    void resetCompose() {
        if (auto* C = composeState(); C)
            xkb_compose_state_reset(C);
    }

    SKey keyFor(xkb_state* state, uint32_t keycode) {
        if (!state)
            return {};

        const xkb_keysym_t SYM = xkb_state_key_get_one_sym(state, keycode + 8);

        if (auto* COMPOSE = composeState(); COMPOSE && SYM != XKB_KEY_NoSymbol && xkb_compose_state_feed(COMPOSE, SYM) == XKB_COMPOSE_FEED_ACCEPTED) {
            switch (xkb_compose_state_get_status(COMPOSE)) {
                case XKB_COMPOSE_COMPOSING: return {.consume = true, .starts = true};
                case XKB_COMPOSE_COMPOSED: {
                    char      buf[64] = {};
                    const int N       = xkb_compose_state_get_utf8(COMPOSE, buf, sizeof(buf));
                    resetCompose();
                    return {.consume = true, .starts = true, .text = N > 0 ? std::string(buf, N) : std::string()};
                }
                case XKB_COMPOSE_CANCELLED: resetCompose(); return {.consume = true, .starts = true};
                case XKB_COMPOSE_NOTHING: break;
            }
        }

        char      buf[8] = {};
        const int N      = xkb_state_key_get_utf8(state, keycode + 8, buf, sizeof(buf));
        if (N <= 0)
            return {};
        if (N == 1 && (buf[0] < 0x20 || buf[0] == 0x7F))
            return {};
        return {.consume = true, .starts = true, .text = std::string(buf, N)};
    }
}
```

- [ ] **Step 5: Make `main.cpp` use it** — add `#include "TextInput.hpp"` beside the other plugin includes, then replace the old helpers:

Delete `static xkb_compose_state* composeState()` (main.cpp:602-630). Replace `resetCompose` and `popCodepoint` bodies with:
```cpp
static void resetCompose() {
    ScrollOverview::TextInput::resetCompose();
}

static void popCodepoint(std::string& s) {
    ScrollOverview::TextInput::popCodepoint(s);
}
```
Replace the body of `searchKeyFor` (keep `struct SSearchKey` as is) with:
```cpp
static SSearchKey searchKeyFor(uint32_t keycode) {
    if (!g_heldGuardKeys.empty())
        return {};
    const auto K = ScrollOverview::TextInput::keyFor(activeXkbState(), keycode);
    return {.consume = K.consume, .starts = K.starts, .text = K.text};
}
```

- [ ] **Step 6: Build and test**

Run: `make all 2>&1 | grep -E " error|FAIL|stack_model|symcheck:"`
Expected: `symcheck: OK …` and `stack_model_test OK`.

- [ ] **Step 7: Live check that overview typing is unchanged** — `make install`, reload the plugin (`hyprctl plugin unload ~/.local/lib/hyprdrive/hyprdrive.so; hyprctl plugin load ~/.local/lib/hyprdrive/hyprdrive.so; hyprctl reload`), then with the computer tool:

```
act(steps=["do overview open", "type caf", "wait 600"], screenshot="never")
screen(mode="region", region=[560,20,320,60])   # the pill reads "caf"
act(steps=["do overview close"])
```
Expected: the pill shows `caf`; results appear as before.

- [ ] **Step 8: Commit**

```bash
git add TextInput.hpp TextInput.cpp TextInputUtf8.hpp main.cpp scripts/stack_model_test.cpp
git commit -m "refactor: TextInput -- one copy of the search field's typing logic"
```

---

### Task 3: Store v2, stack settings, the Downloads seed

**Files:**
- Modify: `DockStacks.hpp` (SStack fields, new setters, `moveTo`)
- Modify: `DockStacks.cpp:34-200` (store read/write via StackModel), `:698-797` (pin/unpin/setters), `:813-852` (open reads the new fields)
- Modify: `Dock.cpp:2993-3008` (`stackIsGrid`/`stackIsByTime`), `:3208-3211` (menu actions)
- Modify: `main.cpp:1720-1740` (`hyprdrive:stack view|sort` dispatcher)
- Modify: `docs/superpowers/specs/2026-09-24-dock-stacks-a-design.md` (two corrections, Step 7)

**Interfaces:**
- Consumes: `StackModel::{ESort, EDisplay, EView, SStore, parseStore, formatStore, shouldSeed}`.
- Produces:
  ```cpp
  namespace ScrollOverview::Stacks {
      struct SStack {
          std::string path;
          StackModel::ESort    sort    = StackModel::ESort::Name;
          StackModel::EDisplay display = StackModel::EDisplay::Stack;
          StackModel::EView    view    = StackModel::EView::Auto;
          std::string customLabel;     // "" = basename
          std::string icon, label;     // resolved: label = customLabel or basename
          bool isDir = false;
      };
      bool setSort(const std::string& path, StackModel::ESort sort);
      bool setDisplay(const std::string& path, StackModel::EDisplay display);
      bool setViewMode(const std::string& path, StackModel::EView view);
      bool setLabel(const std::string& path, const std::string& label);
      bool moveTo(const std::string& path, size_t index); // among stacks
  }
  ```
  (`setView(path, bool)` and `setSort(path, bool)` are removed.)

- [ ] **Step 1: Update `SStack` and the setter declarations in `DockStacks.hpp`**

Replace the `struct SStack { … };` block with the one in *Produces* above (keep its comment about resolving once), add `#include "StackModel.hpp"`, and replace
```cpp
    bool setView(const std::string& path, bool grid);
    bool setSort(const std::string& path, bool byTime);
```
with
```cpp
    bool setSort(const std::string& path, StackModel::ESort sort);
    bool setDisplay(const std::string& path, StackModel::EDisplay display);
    bool setViewMode(const std::string& path, StackModel::EView view);
    bool setLabel(const std::string& path, const std::string& label); // dock label only; "" = basename
    bool moveTo(const std::string& path, size_t index);                // reorder among stacks
```

- [ ] **Step 2: Rewrite the store read/write in `DockStacks.cpp`**

Replace `STORE_HEADER`/`STORE_VERSION`, `reload()` and `writeStore()` (lines 48-49, 111-191) with:

```cpp
    // The format lives in StackModel (host-tested); this file only does I/O.
    struct SStoreState {
        bool seeded   = false;
        bool readOnly = false; // a newer build's file: never write it back
    };
    SStoreState g_storeState;

    void reload(SStore& s) {
        std::error_code ec;
        const auto      PATH   = storePath();
        const bool      EXISTS = !PATH.empty() && std::filesystem::exists(PATH, ec);

        if (!EXISTS) {
            s.loaded = true;
            if (StackModel::shouldSeed(false, StackModel::SStore{.seeded = g_storeState.seeded})) {
                // FIRST RUN: Downloads, as macOS does. `seeded` is written with it,
                // so removing Downloads later keeps it removed.
                g_storeState.seeded = true;
                s.items.clear();
                if (const char* DL = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD); DL && std::filesystem::is_directory(DL, ec)) {
                    s.items.emplace_back(Stacks::SStack{.path = canonical(DL)});
                    decorate(s.items.back());
                }
                writeStore();
            }
            return;
        }

        const auto WRITTEN = std::filesystem::last_write_time(PATH, ec);
        if (ec || (s.loaded && WRITTEN == s.stamp))
            return;

        s.loaded = true;
        s.stamp  = WRITTEN;
        ++g_storeGeneration;

        std::ifstream      file(PATH);
        std::stringstream  buf;
        buf << file.rdbuf();
        const auto PARSED = StackModel::parseStore(buf.str());

        g_storeState.readOnly = PARSED.readOnly;
        g_storeState.seeded   = PARSED.seeded || !PARSED.records.empty();
        s.items.clear();
        for (const auto& R : PARSED.records) {
            s.items.emplace_back(Stacks::SStack{.path = canonical(R.path), .sort = R.sort, .display = R.display, .view = R.view, .customLabel = R.label});
            decorate(s.items.back());
        }
    }

    void writeStore() {
        const auto PATH = storePath();
        if (PATH.empty() || g_storeState.readOnly)
            return;

        StackModel::SStore out;
        out.seeded = g_storeState.seeded;
        for (const auto& S : store().items)
            out.records.push_back({.path = S.path, .sort = S.sort, .display = S.display, .view = S.view, .label = S.customLabel});

        std::error_code ec;
        std::filesystem::create_directories(PATH.parent_path(), ec);
        const auto TMP = PATH.string() + ".tmp";
        {
            std::ofstream f(TMP, std::ios::trunc);
            f << StackModel::formatStore(out);
            if (!f)
                return;
        }
        std::filesystem::rename(TMP, PATH, ec); // atomic on one filesystem
        store().stamp = std::filesystem::last_write_time(PATH, ec);
        ++g_storeGeneration;
    }
```

Add `#include <sstream>` and `#include "StackModel.hpp"`. `writeStore` is now called from `reload`, so declare it before `reload`: add `void writeStore();` next to the existing `void decorate(Stacks::SStack& stack);` forward declaration.

In `decorate()` set the label from the custom one:
```cpp
        const auto SLASH = stack.path.rfind('/');
        const auto BASE  = SLASH == std::string::npos ? stack.path : stack.path.substr(SLASH + 1);
        stack.label      = stack.customLabel.empty() ? BASE : stack.customLabel;
```

- [ ] **Step 3: Replace the setters (DockStacks.cpp:746-781) with the new ones, and add `moveTo`**

```cpp
    namespace {
        // Every setter: re-read, change one field, write, and re-show an open
        // popup that belongs to this stack.
        template <typename F>
        bool editStack(const std::string& path, F&& change) {
            all();
            auto* S = find(path);
            if (!S || !change(*S))
                return false;
            decorate(*S);
            writeStore();
            if (g_popup && g_popup->path == canonical(path)) {
                g_popup->since = Time::steadyNow();
                damagePopupMonitor();
            }
            return true;
        }
    }

    bool setSort(const std::string& path, StackModel::ESort sort) {
        return editStack(path, [&](SStack& s) { return std::exchange(s.sort, sort) != sort; });
    }
    bool setDisplay(const std::string& path, StackModel::EDisplay display) {
        return editStack(path, [&](SStack& s) { return std::exchange(s.display, display) != display; });
    }
    bool setViewMode(const std::string& path, StackModel::EView view) {
        return editStack(path, [&](SStack& s) { return std::exchange(s.view, view) != view; });
    }
    bool setLabel(const std::string& path, const std::string& label) {
        return editStack(path, [&](SStack& s) { return std::exchange(s.customLabel, label) != label; });
    }

    bool moveTo(const std::string& path, size_t index) {
        all();
        auto&      ITEMS = store().items;
        const auto CANON = canonical(path);
        const auto IT    = std::ranges::find_if(ITEMS, [&](const SStack& s) { return s.path == CANON; });
        if (IT == ITEMS.end())
            return false;
        const size_t FROM = sc<size_t>(IT - ITEMS.begin());
        index             = std::min(index, ITEMS.size() - 1);
        if (FROM == index)
            return false;
        auto moved = std::move(*IT);
        ITEMS.erase(IT);
        ITEMS.insert(ITEMS.begin() + sc<long>(index), std::move(moved));
        writeStore();
        return true;
    }
```

(The template sits in an anonymous namespace but is only a helper; every declared `Stacks::` function is at namespace scope, as `symcheck` requires. Add `#include <utility>` for `std::exchange`.)

In `open()` replace `p.grid = S && S->grid; p.sortByTime = S && S->sortByTime;` with:
```cpp
        p.sort = S ? S->sort : StackModel::ESort::Name;
        p.view = S ? S->view : StackModel::EView::Auto;
```
and change `SPopup`'s `bool grid; bool sortByTime;` to
```cpp
        StackModel::ESort sort = StackModel::ESort::Name;
        StackModel::EView view = StackModel::EView::Auto;
        bool              grid = false; // decided at open: view==Grid, or the fan does not fit (Task 6)
```
Set `p.grid = p.view == StackModel::EView::Grid;` in `open()` for now (Task 6 adds the fit test). Replace every `readDir(x, p.sortByTime)`/`g_popup->sortByTime` with `readDir(x, g_popup->sort == StackModel::ESort::Modified)` (readDir is replaced wholesale in Task 4).

- [ ] **Step 4: Update the callers**

`Dock.cpp:2993-3008`:
```cpp
bool stackIsGrid(const SItem& item) {
    for (const auto& S : Stacks::all())
        if (S.path == item.stackPath)
            return S.view == StackModel::EView::Grid;
    return false;
}

bool stackIsByTime(const SItem& item) {
    for (const auto& S : Stacks::all())
        if (S.path == item.stackPath)
            return S.sort == StackModel::ESort::Modified;
    return false;
}
```
`Dock.cpp:3208-3209`:
```cpp
        case ERow::StackView: Stacks::setViewMode(item.stackPath, stackIsGrid(item) ? StackModel::EView::Auto : StackModel::EView::Grid); return;
        case ERow::StackSort: Stacks::setSort(item.stackPath, stackIsByTime(item) ? StackModel::ESort::Name : StackModel::ESort::Modified); return;
```
`main.cpp` dispatcher (`hyprdrive:stack view|sort`):
```cpp
        if (VERB == "view") {
            if (MODE != "fan" && MODE != "grid" && MODE != "auto")
                return {.success = false, .error = "expected auto (fan) or grid"};
            ScrollOverview::Stacks::setViewMode(path, MODE == "grid" ? ScrollOverview::StackModel::EView::Grid : ScrollOverview::StackModel::EView::Auto);
            return {};
        }

        const auto SORT = MODE == "mtime" ? std::optional{ScrollOverview::StackModel::ESort::Modified} : ScrollOverview::StackModel::sortFromName(MODE);
        if (!SORT)
            return {.success = false, .error = "expected name, added, modified (mtime) or kind"};
        ScrollOverview::Stacks::setSort(path, *SORT);
        return {};
```

- [ ] **Step 5: Build**

Run: `make all 2>&1 | grep -E " error|FAIL|symcheck:"`
Expected: `symcheck: OK …`, no errors.

- [ ] **Step 6: Live check the store upgrade and the seed**

```bash
cp ~/.local/state/hyprdrive/stacks /tmp/claude-1000/stacks.v1.bak 2>/dev/null || true
make install >/dev/null && P=~/.local/lib/hyprdrive/hyprdrive.so && hyprctl plugin unload $P && hyprctl plugin load $P && hyprctl reload >/dev/null
hyprctl dispatch 'hl.plugin.hyprdrive.stack("sort added ~/Downloads")' ; head -5 ~/.local/state/hyprdrive/stacks
```
Expected: the file starts `# hyprdrive stacks v2`, `seeded\t1`, and every previously pinned stack is present. (If the dispatcher's Lua name differs, find it with `grep -n "onStackDispatcher" main.cpp` and use the registered name.)

- [ ] **Step 7: Correct the spec** — in `docs/superpowers/specs/2026-09-24-dock-stacks-a-design.md`:
  - replace "as the overview's Sushi handling already does" with "Hyprland's close gesture uses the public `window->move()`; the stack does the same when the Sushi window maps";
  - replace "A GIO file monitor on the folder" with "An inotify watch on the folder, on the compositor's event loop (Hyprland never iterates GLib's main context, so a GFileMonitor would never fire)";
  - replace "The pile is rendered once into a cached texture" with "The pile is drawn from cached per-item textures (no per-frame decode)".

- [ ] **Step 8: Commit**

```bash
git add DockStacks.hpp DockStacks.cpp Dock.cpp main.cpp docs/superpowers/specs/2026-09-24-dock-stacks-a-design.md
git commit -m "feat(stacks): store v2 (sort/display/view/label/order), atomic writes, Downloads seed"
```

---

### Task 4: StackReader — background listing, trash jobs, inotify

**Files:**
- Create: `StackReader.hpp`, `StackReader.cpp`
- Create: `scripts/stack_reader_test.cpp`
- Modify: `scripts/host_tests.sh`
- Modify: `DockStacks.cpp` (drop `readDir`, read through `StackReader`), `main.cpp` (ready callback on the wake fd, shutdown)

**Interfaces:**
- Consumes: `StackModel::SEntry`.
- Produces:
  ```cpp
  namespace ScrollOverview::StackReader {
      constexpr size_t MAX_ENTRIES = 5000;
      struct SListing { bool ready = false; bool truncated = false; bool missing = false; std::vector<StackModel::SEntry> entries; };
      SListing listing(const std::string& dir);          // never blocks; starts a read when stale
      void     invalidate(const std::string& dir);       // next listing() re-reads
      void     watch(const std::vector<std::string>& dirs); // inotify on exactly these dirs
      int      inotifyFd();                              // for wl_event_loop_add_fd; -1 if none
      void     drainInotify();                           // call when inotifyFd() is readable
      void     trash(std::vector<std::string> paths);    // worker; result in lastTrashResult()
      struct STrashResult { uint64_t seq = 0; size_t moved = 0; std::string error; };
      STrashResult lastTrashResult();
      uint64_t generation();                             // moves when a listing or a trash job lands
      void     setReadyCallback(std::function<void()> cb); // called on the WORKER thread
      void     shutdown();                               // join the worker, close inotify
      // Host-test hook: a synchronous read, same code path as the worker.
      SListing readNow(const std::string& dir);
  }
  ```

- [ ] **Step 1: Write the failing test** — `scripts/stack_reader_test.cpp`:

```cpp
// StackReader's directory read: caps, hidden files, directories, birth time.
#include "../StackReader.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

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

    fs::remove_all(DIR);
    std::printf("stack_reader_test: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
```

Add to `scripts/host_tests.sh`:
```bash
run stack_reader_test scripts/stack_reader_test.cpp StackReader.cpp StackModel.cpp $(pkg-config --cflags --libs gio-2.0)
```

- [ ] **Step 2: Run to see it fail**

Run: `bash scripts/host_tests.sh 2>&1 | grep stack_reader`
Expected: `FAILED TO BUILD`.

- [ ] **Step 3: Write `StackReader.hpp`** with exactly the *Produces* block above, plus includes `<cstdint> <functional> <string> <vector> "StackModel.hpp"`, and a header comment: "Folder reads and trash jobs off the compositor thread; inotify for change. Host-tested through readNow()."

- [ ] **Step 4: Write `StackReader.cpp`**

```cpp
#include "StackReader.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <fcntl.h>
#include <filesystem>
#include <mutex>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>

#include <gio/gio.h>

namespace {
    using ScrollOverview::StackModel::SEntry;
    using ScrollOverview::StackReader::SListing;
    using ScrollOverview::StackReader::STrashResult;

    std::string contentKind(const std::string& path, bool dir) {
        if (dir)
            return "inode/directory";
        gboolean uncertain = FALSE;
        char*    ct        = g_content_type_guess(path.c_str(), nullptr, 0, &uncertain); // name only: no open()
        std::string out    = ct ? ct : "application/octet-stream";
        g_free(ct);
        return out;
    }

    std::string iconFor(const std::string& kind) {
        if (kind == "inode/directory")
            return "folder";
        GIcon*      icon = g_content_type_get_icon(kind.c_str());
        std::string out;
        if (icon && G_IS_THEMED_ICON(icon)) {
            const char* const* N = g_themed_icon_get_names(G_THEMED_ICON(icon));
            if (N && N[0])
                out = N[0];
        }
        if (icon)
            g_object_unref(icon);
        return out.empty() ? "text-x-generic" : out;
    }

    SListing readDirectory(const std::string& dir) {
        SListing out;
        out.ready = true;
        std::error_code ec;
        if (!std::filesystem::is_directory(dir, ec)) {
            out.missing = true;
            return out;
        }
        auto it = std::filesystem::directory_iterator{dir, std::filesystem::directory_options::skip_permission_denied, ec};
        if (ec) {
            out.missing = true;
            return out;
        }
        for (const auto& E : it) {
            const auto NAME = E.path().filename().string();
            if (NAME.empty() || NAME[0] == '.')
                continue;
            if (out.entries.size() >= ScrollOverview::StackReader::MAX_ENTRIES) {
                out.truncated = true;
                break;
            }
            const auto  PATH = E.path().string();
            struct statx sx{};
            SEntry       e{.path = PATH, .name = NAME};
            if (statx(AT_FDCWD, PATH.c_str(), AT_STATX_SYNC_AS_STAT, STATX_BTIME | STATX_MTIME | STATX_CTIME | STATX_TYPE, &sx) == 0) {
                e.dir   = S_ISDIR(sx.stx_mode);
                e.mtime = sx.stx_mtime.tv_sec;
                e.btime = (sx.stx_mask & STATX_BTIME) ? sx.stx_btime.tv_sec : sx.stx_ctime.tv_sec;
            }
            e.kind = contentKind(PATH, e.dir);
            e.icon = iconFor(e.kind);
            out.entries.push_back(std::move(e));
        }
        return out;
    }

    struct SJob {
        enum class EKind { List, Trash } kind = EKind::List;
        std::string              dir;
        std::vector<std::string> paths;
    };

    std::mutex                                g_mutex;
    std::condition_variable                   g_cv;
    std::deque<SJob>                          g_jobs;
    std::unordered_map<std::string, SListing> g_cache;     // dir -> last listing
    std::unordered_map<std::string, bool>     g_inflight;  // dir -> a read is queued
    std::thread                               g_thread;
    bool                                      g_stop = false;
    std::atomic<uint64_t>                     g_generation{1};
    std::function<void()>                     g_ready;
    STrashResult                              g_trash;
    uint64_t                                  g_trashSeq = 0;

    int                                   g_inotify = -1;
    std::unordered_map<int, std::string>  g_watchDir; // wd -> dir

    void notify() {
        g_generation.fetch_add(1);
        std::function<void()> cb;
        {
            std::lock_guard l(g_mutex);
            cb = g_ready;
        }
        if (cb)
            cb();
    }

    void workerMain() {
        std::unique_lock lock(g_mutex);
        while (!g_stop) {
            g_cv.wait(lock, [] { return g_stop || !g_jobs.empty(); });
            if (g_stop)
                break;
            auto job = std::move(g_jobs.front());
            g_jobs.pop_front();
            lock.unlock();

            if (job.kind == SJob::EKind::List) {
                auto result = readDirectory(job.dir);
                lock.lock();
                g_cache[job.dir]    = std::move(result);
                g_inflight[job.dir] = false;
                lock.unlock();
            } else {
                STrashResult r;
                for (const auto& P : job.paths) {
                    GFile*  f   = g_file_new_for_path(P.c_str());
                    GError* err = nullptr;
                    const bool OK = g_file_trash(f, nullptr, &err);
                    g_object_unref(f);
                    if (!OK) {
                        r.error = err && err->message ? err->message : "could not move to Trash";
                        if (err)
                            g_error_free(err);
                        break;
                    }
                    ++r.moved;
                }
                lock.lock();
                r.seq   = ++g_trashSeq;
                g_trash = r;
                lock.unlock();
            }
            notify();
            lock.lock();
        }
    }

    void enqueue(SJob job) {
        if (!g_thread.joinable()) {
            g_stop   = false;
            g_thread = std::thread(workerMain);
        }
        g_jobs.push_back(std::move(job));
        g_cv.notify_all();
    }
}

namespace ScrollOverview::StackReader {

    SListing readNow(const std::string& dir) {
        return readDirectory(dir);
    }

    SListing listing(const std::string& dir) {
        std::lock_guard l(g_mutex);
        const auto      IT = g_cache.find(dir);
        if (IT != g_cache.end() && IT->second.ready)
            return IT->second;
        if (!g_inflight[dir]) {
            g_inflight[dir] = true;
            enqueue(SJob{.kind = SJob::EKind::List, .dir = dir});
        }
        return IT != g_cache.end() ? IT->second : SListing{};
    }

    void invalidate(const std::string& dir) {
        std::lock_guard l(g_mutex);
        if (auto IT = g_cache.find(dir); IT != g_cache.end())
            IT->second.ready = false; // keep the old entries to draw until the new read lands
    }

    void watch(const std::vector<std::string>& dirs) {
        if (g_inotify < 0)
            g_inotify = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (g_inotify < 0)
            return;
        for (auto it = g_watchDir.begin(); it != g_watchDir.end();) {
            if (std::ranges::find(dirs, it->second) == dirs.end()) {
                inotify_rm_watch(g_inotify, it->first);
                it = g_watchDir.erase(it);
            } else
                ++it;
        }
        for (const auto& D : dirs) {
            if (std::ranges::any_of(g_watchDir, [&](const auto& kv) { return kv.second == D; }))
                continue;
            const int WD = inotify_add_watch(g_inotify, D.c_str(), IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_CLOSE_WRITE | IN_DELETE_SELF | IN_MOVE_SELF | IN_ATTRIB);
            if (WD >= 0)
                g_watchDir[WD] = D;
        }
    }

    int inotifyFd() {
        return g_inotify;
    }

    void drainInotify() {
        alignas(struct inotify_event) char buf[4096];
        bool changed = false;
        for (;;) {
            const ssize_t N = ::read(g_inotify, buf, sizeof(buf));
            if (N <= 0)
                break;
            for (ssize_t off = 0; off < N;) {
                const auto* EV = reinterpret_cast<const struct inotify_event*>(buf + off);
                if (const auto IT = g_watchDir.find(EV->wd); IT != g_watchDir.end()) {
                    invalidate(IT->second);
                    changed = true;
                    if (EV->mask & (IN_DELETE_SELF | IN_MOVE_SELF | IN_IGNORED))
                        g_watchDir.erase(IT); // the folder is gone: the cell dims, no dead watch
                }
                off += sizeof(struct inotify_event) + EV->len;
            }
        }
        if (changed)
            g_generation.fetch_add(1);
    }

    void trash(std::vector<std::string> paths) {
        std::lock_guard l(g_mutex);
        enqueue(SJob{.kind = SJob::EKind::Trash, .paths = std::move(paths)});
    }

    STrashResult lastTrashResult() {
        std::lock_guard l(g_mutex);
        return g_trash;
    }

    uint64_t generation() {
        return g_generation.load();
    }

    void setReadyCallback(std::function<void()> cb) {
        std::lock_guard l(g_mutex);
        g_ready = std::move(cb);
    }

    void shutdown() {
        {
            std::lock_guard l(g_mutex);
            g_stop = true;
            g_jobs.clear();
            g_ready = nullptr;
        }
        g_cv.notify_all();
        if (g_thread.joinable())
            g_thread.join();
        if (g_inotify >= 0) {
            ::close(g_inotify);
            g_inotify = -1;
        }
        g_watchDir.clear();
        std::lock_guard l(g_mutex);
        g_cache.clear();
        g_inflight.clear();
    }
}
```

- [ ] **Step 5: Run the test**

Run: `bash scripts/host_tests.sh 2>&1 | grep stack_reader`
Expected: `stack_reader_test OK -- stack_reader_test: 7 checks, 0 failed`.

- [ ] **Step 6: Wire it into the plugin**

In `main.cpp`, next to the `ToolSearch::setReadyCallback` block (main.cpp ~2146):
```cpp
            // Stack folder reads and trash jobs land off-thread too; same wake.
            ScrollOverview::StackReader::setReadyCallback([]() {
                if (g_fileWakeFd >= 0) {
                    uint64_t one = 1;
                    ::write(g_fileWakeFd, &one, sizeof(one));
                }
            });
```
In `onFileResultsReady` (find with `grep -n "static int onFileResultsReady" main.cpp`), add at its end, before `return 0;`:
```cpp
    ScrollOverview::Stacks::onReaderReady();
```
In PLUGIN_EXIT, after `shutdownClickGestures();`:
```cpp
    ScrollOverview::Stacks::shutdown();
    ScrollOverview::StackReader::shutdown();
```
Add `#include "StackReader.hpp"`.

In `DockStacks.hpp` declare:
```cpp
    // A StackReader job landed (main thread, from the wake fd): refresh what
    // the popup and the cells show, and damage what changed.
    void onReaderReady();
    // Plugin unload: the inotify fd source, Sushi, the popup.
    void shutdown();
```

In `DockStacks.cpp`:
- delete `readDir` (lines 271-328) and the `SEntry` struct (203-209); use `StackModel::SEntry` (`using StackModel::SEntry;` in the anonymous namespace);
- add a file-scope `wl_event_source* g_inotifySource = nullptr;` and in `all()` after `reload(S)`, keep the watches in step with the store:
```cpp
        std::vector<std::string> dirs;
        for (const auto& ST : S.items)
            if (ST.isDir)
                dirs.push_back(ST.path);
        StackReader::watch(dirs);
        if (!g_inotifySource && StackReader::inotifyFd() >= 0 && g_pCompositor)
            g_inotifySource = wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, StackReader::inotifyFd(), WL_EVENT_READABLE,
                                                   [](int, uint32_t, void*) -> int {
                                                       StackReader::drainInotify();
                                                       Stacks::onReaderReady();
                                                       return 0;
                                                   },
                                                   nullptr);
```
- add a helper that returns a popup's sorted entries from the reader:
```cpp
    // The popup's entries: the reader's listing, sorted by the stack's key.
    // Empty and `ready=false` until the first read lands.
    StackReader::SListing sortedListing(const std::string& dir, StackModel::ESort sort) {
        auto L = StackReader::listing(dir);
        StackModel::sortEntries(L.entries, sort);
        return L;
    }
```
- in `SPopup` add `bool loading = true; bool truncated = false; uint64_t readerGen = 0;`; in `open()` replace `p.entries = readDir(...)` with
```cpp
        auto L       = sortedListing(CANON, p.sort);
        p.entries    = std::move(L.entries);
        p.loading    = !L.ready;
        p.truncated  = L.truncated;
        p.readerGen  = StackReader::generation();
```
- define at namespace scope:
```cpp
    void onReaderReady() {
        if (g_popup && !g_popup->leaving && g_popup->readerGen != StackReader::generation()) {
            auto L                = sortedListing(g_popup->path, g_popup->sort);
            g_popup->entries      = std::move(L.entries);
            g_popup->loading      = !L.ready;
            g_popup->truncated    = L.truncated;
            g_popup->readerGen    = StackReader::generation();
            damagePopupMonitor();
        }
        ++g_storeGeneration; // the dock cells (the pile, Task 8) key on this
        for (const auto& M : State::monitorState()->monitors())
            if (M)
                g_pHyprRenderer->damageMonitor(M);
    }

    void shutdown() {
        if (g_inotifySource) {
            wl_event_source_remove(g_inotifySource);
            g_inotifySource = nullptr;
        }
        g_popup.reset();
        g_itemDrag.reset();
    }
```
- `moveInto` success path (DockStacks.cpp ~997): replace the `readDir` refresh with `StackReader::invalidate(g_popup->path); StackReader::invalidate(DEST);` and let `onReaderReady` refresh.
- `popupProgress` and the fan code that used `FAN_MAX` keep compiling for now; Task 6 removes them.

- [ ] **Step 7: Build, install, live check**

Run: `make all 2>&1 | grep -E " error|FAIL|symcheck:|stack_"` → no errors, both stack suites OK.
Then install/reload and with the computer tool click the Downloads cell (find it with `state` + a region screenshot of the dock), confirm the fan shows the files, `touch ~/Downloads/zz-stack-test.txt` and confirm it appears within a second, then `rm` it. Close the fan by clicking the cell again.

- [ ] **Step 8: Commit**

```bash
git add StackReader.hpp StackReader.cpp scripts/stack_reader_test.cpp scripts/host_tests.sh DockStacks.hpp DockStacks.cpp main.cpp
git commit -m "feat(stacks): StackReader -- background folder reads (5000 cap), trash jobs, inotify"
```

---

### Task 5: Dock order and stack reordering

**Files:**
- Modify: `Dock.cpp:850-895` (`buildDockItems` order), `Dock.cpp:3480-3570` (`SDockDrag`, slots, caret), `Dock.cpp:4076-4095` (`dragBegin`), `Dock.cpp:4187-4230` (`dragRelease`), `Dock.cpp:4172` (`dragMove` insert)

**Interfaces:**
- Consumes: `Stacks::moveTo(path, index)`.
- Produces: `std::vector<size_t> stackSlots(const SLayout&)`; `SDockDrag::stackPath` (non-empty for a stack drag).

- [ ] **Step 1: Reorder `buildDockItems`** — move the stacks loop (Dock.cpp:884-893) to just BEFORE the locations loop (Dock.cpp:861), and split the locations loop into mounts first, then everything else (Trash):

```cpp
    // Dock order (dock stacks A): apps | Scratchpad | stacks | removable drives | Trash.
    for (const auto& STACK : Stacks::all()) {
        out.emplace_back(SItem{
            .icon         = STACK.icon,
            .uri          = "file://" + STACK.path,
            .locationName = STACK.label,
            .stack        = true,
            .stackPath    = STACK.path,
            .stackDir     = STACK.isDir,
        });
    }

    const bool ISOLATE = Config::getValue<bool>(KEY_ISOLATE_LOCATIONS);
    for (const bool MOUNTS : {true, false}) {
        for (const auto& LOCATION : Locations::items()) {
            if (LOCATION.mount != MOUNTS)
                continue;
            out.emplace_back(SItem{
                .icon         = LOCATION.icon,
                .running      = ISOLATE && Locations::openWindows(LOCATION.uri),
                .uri          = LOCATION.uri,
                .locationName = LOCATION.name,
                .mount        = LOCATION.mount,
                .canEject     = LOCATION.canEject,
                .mounted      = LOCATION.mounted,
                .trashEmpty   = LOCATION.empty,
            });
        }
    }
```
(Keep the explanatory comments that were on both loops above the new ones.)

- [ ] **Step 2: Let a stack cell start a drag** — add to `SDockDrag` (Dock.cpp ~3480): `std::string stackPath; // a stack being reordered; empty for an app`. In `dragBegin` replace the early return:
```cpp
    if (ITEM == LAYOUT.items.end() || (ITEM->desktopId.empty() && !ITEM->stack))
        return false;

    g_dockDrag.drag = SDockDrag{.id        = ITEM->desktopId,
                                .monitor   = monitor->m_id,
                                .start     = point,
                                .icon      = ITEM->icon,
                                .home      = ITEM->iconBox,
                                .grab      = point - Vector2D{ITEM->iconBox.x, ITEM->iconBox.y},
                                .at        = point,
                                .stackPath = ITEM->stack ? ITEM->stackPath : std::string{}};
```

- [ ] **Step 3: Stack slots and the caret** — beside `favoriteSlots` (Dock.cpp:3559):
```cpp
std::vector<size_t> stackSlots(const SLayout& layout) {
    std::vector<size_t> out;
    for (size_t i = 0; i < layout.items.size(); ++i)
        if (layout.items[i].stack)
            out.emplace_back(i);
    return out;
}

// Where a dragged STACK would land among the stacks: the same midpoint rule
// as dropIndexAt, over the stack cells only. -1 off the pill.
int stackDropIndexAt(PHLMONITOR monitor, const Vector2D& point) {
    if (!monitor)
        return -1;
    const auto LAYOUT = layout(monitor);
    if (LAYOUT.pill.width < 1 || !LAYOUT.pill.containsPoint(point))
        return -1;
    const auto SLOTS = stackSlots(LAYOUT);
    for (size_t k = 0; k < SLOTS.size(); ++k) {
        const auto& BOX = LAYOUT.items[SLOTS[k]].iconBox;
        if (point.x < BOX.x + BOX.width / 2.0)
            return sc<int>(k);
    }
    return sc<int>(SLOTS.size());
}
```
In `dragMove` (Dock.cpp:4172) replace `g_dockDrag.drag->insert = dropIndexAt(monitor, point);` with:
```cpp
    g_dockDrag.drag->insert = g_dockDrag.drag->stackPath.empty() ? dropIndexAt(monitor, point) : stackDropIndexAt(monitor, point);
```
Where the caret is DRAWN (find with `grep -n "caretIndex()" Dock.cpp`), the slot list it indexes must be the stack slots for a stack drag. At each call site that does `favoriteSlots(layout)` to position the caret, use:
```cpp
    const auto SLOTS = (g_dockDrag.drag && !g_dockDrag.drag->stackPath.empty()) ? stackSlots(layout) : favoriteSlots(layout);
```

- [ ] **Step 4: Release** — at the top of `dragRelease`, right after `g_dockDrag.drag.reset();`:
```cpp
    // A STACK being reordered: only among the stacks, and only a live drag
    // that ends on the pill. Anything else is a click (it opens the stack).
    if (!DRAG.stackPath.empty()) {
        const float CLICKTOL = 3.F * dragThresholdPx(monitor);
        if (!DRAG.live || DRAG.start.distanceSq(point) <= CLICKTOL * CLICKTOL)
            return false;
        const int INDEX = stackDropIndexAt(monitor, point);
        if (INDEX < 0)
            return true; // dropped off the pill: nothing moves (removing is the menu's job)
        // The index counts the dragged stack itself; moving later lands one short otherwise.
        size_t from = 0;
        for (const auto& S : Stacks::all()) {
            if (S.path == DRAG.stackPath)
                break;
            ++from;
        }
        const size_t TO = sc<size_t>(INDEX) > from ? sc<size_t>(INDEX) - 1 : sc<size_t>(INDEX);
        Stacks::moveTo(DRAG.stackPath, TO);
        return true;
    }
```

- [ ] **Step 5: Build, install, live check** — `make all` (no errors), install/reload. With the computer tool, pin a second stack (`hyprctl dispatch` the stack pin verb on `~/Documents`), take a dock region screenshot to confirm order apps | Scratchpad | Downloads | Documents | Trash, then drag Documents before Downloads in ONE act call:
```
act(steps=["require none", "move <Documents x> <y>", "down", "move <mid x> <y>", "move <Downloads left edge> <y>", "wait 300", "up"], screenshot="never")
```
Confirm the new order with a region screenshot and `grep stack ~/.local/state/hyprdrive/stacks`. Unpin Documents afterwards if it was not pinned before.

- [ ] **Step 6: Commit**

```bash
git add Dock.cpp
git commit -m "feat(stacks): dock order apps|scratchpad|stacks|drives|trash; drag stacks to reorder"
```

---

### Task 6: The fan — fit decision, keyboard, Space preview with a highlighted item, Sushi beside the fan

**Files:**
- Modify: `DockStacks.cpp` (layoutFan, open, render, previewHovered, new handleKey, Sushi placement)
- Modify: `DockStacks.hpp` (`handleKey`, `keyboardActive`)
- Modify: `main.cpp` raw key hook (route keys to the stack before the overview)

**Interfaces:**
- Consumes: `StackModel::fanFits`, `OvInternal::overviewTopChromeBottomL(monitor)` (OverviewInternal.hpp; include it in DockStacks.cpp as ScratchpadExit.cpp does), `StackReader::listing`.
- Produces:
  ```cpp
  namespace ScrollOverview::Stacks {
      bool handleKey(uint32_t keycode);  // a PRESS; true = claimed (the fan/pane handled it)
      bool keyboardActive();             // a fan or pane is open and wants the keyboard
  }
  ```

- [ ] **Step 1: Decide fan vs pane at open** — in `open()`, after the listing is read:
```cpp
        // THE FIT (spec: "no fixed cap"). The fan is shown when every row stays
        // one step below the top bar; otherwise the pane (Task 7). A view of
        // Always Grid always takes the pane.
        const float SCALE_L   = 1.F; // StackModel works in logical px
        const float PILLTOP_L = sc<float>(anchor.y) / std::max(monitor->m_scale, 0.01F);
        const float TOP_L     = OvInternal::overviewTopChromeBottomL(monitor) - sc<float>(monitor->m_position.y);
        const StackModel::SFanFit FIT{.pillTopL = PILLTOP_L, .topLimitL = TOP_L, .stepL = FAN_STEP * SCALE_L, .gapL = FAN_GAP * SCALE_L};
        p.grid = p.view == StackModel::EView::Grid || !StackModel::fanFits(std::max<size_t>(1, p.entries.size()), FIT);
```
Remove `FAN_MAX`, `p.shown`/`p.hidden` and the "N more…" branch: in `layoutFan` the rows are `p.entries.size()` file rows, plus ONE action row only when the folder is empty ("Empty folder"). Replace `const size_t ROWS = p.shown + 1;` with:
```cpp
        const size_t ROWS = std::max<size_t>(1, p.entries.size());
        const bool   EMPTYROW = p.entries.empty();
```
and the label/icon block with:
```cpp
            const bool ACTION = EMPTYROW;
            std::string LABEL = ACTION ? (p.loading ? std::string{"Loading\xe2\x80\xa6"} : std::string{"Empty folder"}) : p.entries[i].name;
            std::string ICON  = ACTION ? std::string{"folder-open"} : p.entries[i].icon;
```
Replace `STAGGER_MS * sc<float>(p.shown)` everywhere (layoutFan, render, popupProgress) with `STAGGER_MS * sc<float>(std::min<size_t>(p.entries.size(), 24))` — the stagger stays bounded however tall the fan is.

When the listing lands after open (`onReaderReady`), re-run the same fit test and flip `g_popup->grid` if needed (move the three lines above into `static bool chooseGrid(const SPopup&, PHLMONITOR)` and call it from both places).

- [ ] **Step 2: Hover index and keyboard** — add to `SPopup`: `int hover = -1; int previewing = -1; bool keyboard = false; int cols = 1; std::vector<size_t> visible;` (`cols` is set by the pane layout in Task 7 and stays 1 for the fan; `visible` is filled by `refilter` in Task 7 — until then set `p.visible` to `0..entries.size()-1` in `open()` and `onReaderReady()`). Add the claimed-key handler at namespace scope:

```cpp
    bool keyboardActive() {
        return g_popup && !g_popup->leaving;
    }

    bool handleKey(uint32_t keycode) {
        if (!keyboardActive())
            return false;
        auto&     P    = *g_popup;
        const int N    = sc<int>(P.entries.size());
        const int COLS = P.grid ? P.cols : 1; // cols is set by the pane layout (Task 7); 1 for the fan

        switch (keycode) {
            case 103: /* UP */ P.hover = std::clamp(P.hover < 0 ? 0 : P.hover + (P.grid ? -COLS : 1), 0, std::max(0, N - 1)); break; // the fan grows UP
            case 108: /* DOWN */ P.hover = std::clamp(P.hover < 0 ? 0 : P.hover - (P.grid ? -COLS : 1), 0, std::max(0, N - 1)); break;
            case 105: /* LEFT */ if (P.grid) P.hover = std::max(0, P.hover - 1); else return false; break;
            case 106: /* RIGHT */ if (P.grid) P.hover = std::min(N - 1, P.hover + 1); else return false; break;
            case 28: /* ENTER */
                if (P.hover >= 0 && P.hover < N) {
                    launchPath(P.entries[sc<size_t>(P.hover)].path);
                    close();
                }
                break;
            case 1: /* ESC */
                if (P.previewing >= 0) {
                    OmarchySearch::closeSushi();
                    P.previewing = -1;
                } else
                    close();
                break;
            case 57: /* SPACE */ return previewHovered();
            default: return false;
        }
        P.keyboard = true;
        damagePopupMonitor();
        return true;
    }
```
(`OmarchySearch::closeSushi()` closes the previewer over the same bus; include `OmarchySearch.hpp`.) Keycodes are evdev: UP 103, DOWN 108, LEFT 105, RIGHT 106, ENTER 28, ESC 1, SPACE 57.

- [ ] **Step 3: Preview what is hovered OR keyboard-selected, and remember it** — replace `hoveredItemPath()` and `previewHovered()` with:
```cpp
    // The item SPACE means: the keyboard selection if the keyboard moved last,
    // else the row under the pointer.
    int targetIndex() {
        if (!g_popup || g_popup->leaving)
            return -1;
        if (g_popup->keyboard && g_popup->hover >= 0)
            return g_popup->hover;
        const auto PATH = hoveredItemPath();
        for (size_t i = 0; i < g_popup->entries.size(); ++i)
            if (g_popup->entries[i].path == PATH)
                return sc<int>(i);
        return -1;
    }

    bool previewHovered() {
        const int I = targetIndex();
        if (I < 0)
            return false;
        const auto& PATH = g_popup->entries[sc<size_t>(I)].path;
        GError* err = nullptr;
        char*   uri = g_filename_to_uri(PATH.c_str(), nullptr, &err);
        if (!uri) {
            if (err)
                g_error_free(err);
            return false;
        }
        showPreview(uri);
        g_free(uri);
        // Sushi's own toggle: the same item again closes it.
        g_popup->previewing = g_popup->previewing == I ? -1 : I;
        g_popup->previewBox = {}; // placed when the window maps (Step 5)
        damagePopupMonitor();
        return true;
    }
```
`hoveredItemPath()` stays as is (it reads the drawn rows); a pointer move sets `g_popup->keyboard = false` — in `render()`, compare the mouse to last frame's and clear `keyboard` when it moved:
```cpp
        if (MOUSE.distance(g_popup->lastMouse) > 2.0)
            g_popup->keyboard = false;
        g_popup->lastMouse = MOUSE;
```
(add `Vector2D lastMouse;` to SPopup). In `close()` also close Sushi when a preview is up:
```cpp
        if (g_popup->previewing >= 0) {
            OmarchySearch::closeSushi();
            g_popup->previewing = -1;
        }
```

- [ ] **Step 4: Draw the previewed and keyboard rows** — in the fan loop of `render()`, before `drawPlate(R.box, …)`, compute the row's entry index `i` and:
```cpp
            const bool PREV  = sc<int>(i) == g_popup->previewing;
            const bool SEL   = g_popup->keyboard && sc<int>(i) == g_popup->hover;
            // The pointer's row gets the SAME ring, lighter (spec: "hover gives a subtle
            // row highlight; nothing opens"). The fan had no hover state before this.
            const bool HOVER = !g_popup->keyboard && R.box.containsPoint(MOUSE);
            CBox       box  = R.box;
            if (PREV) {
                // 1.15x about the row's centre, and an accent ring.
                box.x -= box.width * 0.075;
                box.y -= box.height * 0.075;
                box.width *= 1.15;
                box.height *= 1.15;
            }
            drawPlate(box, FAN_RADIUS, A);
            if (PREV || SEL || HOVER) {
                CBorderPassElement::SBorderData ring;
                ring.box        = box;
                ring.grad1      = ::Config::CGradientValueData(Dock::accentColor().modifyA(A * (PREV ? 1.F : SEL ? 0.6F : 0.35F)));
                ring.round      = sc<int>(std::round(FAN_RADIUS * SCALE));
                ring.borderSize = std::max(1, sc<int>(std::round(2.F * SCALE)));
                ring.a          = 1.F;
                g_pHyprRenderer->m_renderPass.add(makeUnique<CBorderPassElement>(ring));
            }
```
Draw the icon and label inside `box` (scale the icon by 1.15 too when PREV). `Dock::accentColor()`: if it does not exist, add to `Dock.hpp`/`Dock.cpp`:
```cpp
CHyprColor accentColor() {
    if (auto* const ACTIVE = sc<::Config::CGradientValueData*>(ScrollOverview::Config::valueRef<::Config::IComplexConfigValue>("general:col.active_border").ptr());
        ACTIVE && !ACTIVE->m_colors.empty())
        return ACTIVE->m_colors[0];
    return ScrollOverview::Config::themeForeground();
}
```
(the same lookup `OverviewRender::labelPill` uses). Include `<hyprland/src/render/pass/BorderPassElement.hpp>` in DockStacks.cpp. `needsFrames` already returns false for a settled popup; keyboard moves call `damagePopupMonitor()`. The pointer's hovered row must repaint when the pointer moves without anything else changing, so add it to `visualState()` (the dock compares that string to decide whether to damage):
```cpp
        if (g_popup)
            out += "|h" + hoveredItemPath();
```

- [ ] **Step 5: Sushi beside the fan** — listen for the previewer window's `openLate` (Hyprland fires it after window rules, so the rule's float + centre has already run) and move it. No deferred callback: a `doLater` still queued at unload would call into unloaded code. In `DockStacks.cpp`, a file-scope listener armed from `open()` and reset in `shutdown()`:
```cpp
    CHyprSignalListener g_sushiOpen;

    void placeSushi(const PHLWINDOW& W) {
        if (!W || !g_popup || g_popup->previewing < 0)
            return;
        PHLMONITOR mon = nullptr;
        for (const auto& M : State::monitorState()->monitors())
            if (M && M->m_id == g_popup->monitor)
                mon = M;
        if (!mon)
            return;
        const float S    = std::max(mon->m_scale, 0.01F);
        const auto  SIZE = W->m_realSize->goal();
        // The popup's drawn extent, in logical px (the pane: its plate; the fan: its rows).
        double left = 1e9, right = -1e9, top = 1e9;
        if (g_popup->grid && !g_popup->plate.empty()) {
            left  = g_popup->plate.x / S;
            right = (g_popup->plate.x + g_popup->plate.width) / S;
            top   = g_popup->plate.y / S;
        } else {
            for (const auto& R : g_popup->rows) {
                left  = std::min(left, R.box.x / S);
                right = std::max(right, (R.box.x + R.box.width) / S);
                top   = std::min(top, R.box.y / S);
            }
        }
        if (right < left)
            return;
        const double MONW       = mon->m_size.x;
        const bool   RIGHT_SIDE = (MONW - right) >= left; // the side with more room
        double       x          = RIGHT_SIDE ? right + 16.0 : left - 16.0 - SIZE.x;
        x                       = std::clamp(x, 8.0, std::max(8.0, MONW - SIZE.x - 8.0));
        const double Y          = std::clamp(top, 8.0, std::max(8.0, mon->m_size.y - SIZE.y - 8.0));
        W->move(Vector2D{mon->m_position.x + x, mon->m_position.y + Y});
    }

    void armSushiPlacement() {
        if (g_sushiOpen)
            return;
        g_sushiOpen = Event::bus()->m_events.window.openLate.listen([](PHLWINDOW w) {
            if (w && w->m_class == "org.gnome.NautilusPreviewer")
                placeSushi(w);
        });
    }
```
Call `armSushiPlacement()` at the end of `Stacks::open()`. Includes: `<hyprland/src/event/EventBus.hpp>`, `<hyprland/src/desktop/view/Window.hpp>`. In `shutdown()`: `g_sushiOpen.reset();`. Sushi reuses its window when the preview SWITCHES items (no new `openLate`), so the first placement holds for the rest of that preview; if the move is ignored, Sushi stays centred — acceptable per the spec.

- [ ] **Step 6: Route keys** — in `main.cpp`'s raw key hook, replace the SPACE-preview block (main.cpp:1099-1103) with:
```cpp
                // A stack fan/pane that is open owns ITS keys (arrows, Enter,
                // Esc, Space, and -- the pane -- typing). Ahead of the overview
                // for the same reason SPACE always was: a fan opened over the
                // overview must see them first.
                if (ScrollOverview::Stacks::keyboardActive() && ScrollOverview::Stacks::handleKey(event.keycode)) {
                    g_consumedPresses.insert(event.keycode);
                    info.cancelled = true;
                    return;
                }
```
`handleKey` claims SPACE only when `previewHovered()` does, so Space outside a fan/pane still falls through untouched.

- [ ] **Step 7: Build, install, live check** — `make all`, install/reload. With the computer tool:
  - put 6 files in a scratch folder `~/stack-a-test` (`for i in 1 2 3 4 5 6; do echo $i > ~/stack-a-test/f$i.txt; done`), pin it (`hyprctl dispatch` the stack pin verb), click its cell → a 6-row fan;
  - hover row 2, `key space` → Sushi opens beside the fan; the row is enlarged with an accent ring (screenshot the fan region);
  - `key Up` twice, `key space` → the preview moves to that row; `key Escape` → Sushi closes, `key Escape` → fan closes;
  - add 40 files and click again → the pane opens instead (Task 7 draws it; until then the old grid shows).
  Unpin and remove `~/stack-a-test` at the end.

- [ ] **Step 8: Commit**

```bash
git add DockStacks.hpp DockStacks.cpp Dock.hpp Dock.cpp main.cpp
git commit -m "feat(stacks): fan fits the screen, keyboard, Space preview with a ringed item, Sushi beside the fan"
```

---

### Task 7: The searchable pane

**Files:**
- Create: `StackPane.cpp` (layout + draw of the pane; declared in `DockStacks.hpp` as internal hooks)
- Modify: `DockStacks.cpp` (replace `layoutGrid` and the grid branch of `render`, typing in `handleKey`, `scroll`)
- Modify: `DockStacks.hpp`

**Interfaces:**
- Consumes: `TextInput::keyFor`, `TextInput::popCodepoint`, `StackModel::filter`, `activeXkbState` (main.cpp — expose it: see Step 4).
- Produces (internal, `DockStacks.hpp`, namespace `ScrollOverview::Stacks::Pane`):
  ```cpp
  struct SPaneLayout { CBox plate, header, field, grid; int cols = 1; float cellW = 0, cellH = 0; };
  SPaneLayout layout(PHLMONITOR monitor, const CBox& anchor, const CBox& pill, size_t items, float progress, bool leaving);
  ```

- [ ] **Step 1: Pane state** — add to `SPopup`: `std::string query; CBox fieldBox, headerBox;` (`visible` and `cols` exist since Task 6). After any listing refresh or query edit:
```cpp
    void refilter(SPopup& p) {
        p.visible = StackModel::filter(p.entries, p.query);
        if (p.hover >= sc<int>(p.visible.size()))
            p.hover = p.visible.empty() ? -1 : 0;
    }
```
Call it at the end of `open()` and in `onReaderReady()`. In pane mode, `hover`/`previewing` index into `visible` (fan mode: `visible` is all entries in order, so the same code works — make `open()` always call `refilter`, and change every `P.entries[i]` in `handleKey`/`targetIndex`/`previewHovered` to `P.entries[P.visible[i]]`, with `N = P.visible.size()`).

- [ ] **Step 2: Layout** — `StackPane.cpp`:
```cpp
#include "DockStacks.hpp"

#include <algorithm>
#include <cmath>

namespace ScrollOverview::Stacks::Pane {

    // Spec: <= 6 columns, <= 60% of the monitor's width, <= 55% of its height.
    constexpr float CELL_W = 104.F, CELL_H = 112.F, SPACING = 10.F, PAD = 14.F, HEADER_H = 30.F, FIELD_H = 34.F, GAP = 10.F;

    SPaneLayout layout(PHLMONITOR monitor, const CBox& anchor, const CBox& pill, size_t items, float progress, bool leaving) {
        SPaneLayout out;
        const float S     = std::max(monitor->m_scale, 0.01F);
        const float MONW  = sc<float>(monitor->m_size.x) * S;
        const float MONH  = sc<float>(monitor->m_size.y) * S;
        const float CELLW = CELL_W * S, CELLH = CELL_H * S, SP = SPACING * S, P = PAD * S;

        const int   MAXCOLS = std::max(1, std::min(6, sc<int>((MONW * 0.60F - 2.F * P + SP) / (CELLW + SP))));
        out.cols            = std::clamp(sc<int>(std::ceil(std::sqrt(std::max<size_t>(items, 1) * 1.6F))), 1, MAXCOLS);
        const int   ROWS    = std::max(1, sc<int>((items + out.cols - 1) / out.cols));

        const float W      = 2.F * P + out.cols * CELLW + (out.cols - 1) * SP;
        const float CHROME = 2.F * P + (HEADER_H + FIELD_H + 2.F * GAP) * S;
        const float MAXH   = MONH * 0.55F;
        const float GRIDH  = std::min(ROWS * CELLH + (ROWS - 1) * SP, MAXH - CHROME);
        const float H      = CHROME + std::max(CELLH, GRIDH);

        const float E     = leaving ? 1.F - houseEase(false, progress) : houseEase(true, progress);
        const float CX    = sc<float>(anchor.x) + sc<float>(anchor.width) / 2.F;
        const float RESTY = sc<float>(pill.y) - GAP * S - H;
        CBox        plate{sc<float>(anchor.x) + (CX - W / 2.F - sc<float>(anchor.x)) * E, sc<float>(anchor.y) + (RESTY - sc<float>(anchor.y)) * E,
                   sc<float>(anchor.width) + (W - sc<float>(anchor.width)) * E, sc<float>(anchor.height) + (H - sc<float>(anchor.height)) * E};
        plate.x   = std::clamp(plate.x, 0.0, std::max(0.0, MONW - plate.width));
        out.plate = plate.round();

        out.header = CBox{plate.x + P, plate.y + P, plate.width - 2 * P, HEADER_H * S}.round();
        out.field  = CBox{plate.x + P, out.header.y + out.header.height + GAP * S, plate.width - 2 * P, FIELD_H * S}.round();
        out.grid   = CBox{plate.x + P, out.field.y + out.field.height + GAP * S, plate.width - 2 * P, plate.y + plate.height - P - (out.field.y + out.field.height + GAP * S)}.round();
        out.cellW  = CELLW;
        out.cellH  = CELLH;
        return out;
    }
}
```
`houseEase` comes from the dock's shared header (DockStacks.cpp already uses it; include the same header here).

- [ ] **Step 3: Replace `layoutGrid`** with a pane layout that fills `p.rows` from `p.visible`, scrolled, clipped to `grid`:
```cpp
    void layoutPane(SPopup& p, PHLMONITOR monitor, const CBox& pill, float progress) {
        const auto L = Pane::layout(monitor, p.anchor, pill, p.visible.size(), progress, p.leaving);
        p.plate      = L.plate;
        p.cols       = L.cols;
        p.fieldBox   = L.field;
        p.headerBox  = L.header;
        const float S  = std::max(monitor->m_scale, 0.01F);
        const float SP = Pane::SPACING * S;
        const int   ROWS = std::max(1, sc<int>((p.visible.size() + L.cols - 1) / L.cols));
        const float MAXSCROLL = std::max(0.F, ROWS * (L.cellH + SP) - SP - sc<float>(L.grid.height));
        // Keep the keyboard selection in view.
        if (p.keyboard && p.hover >= 0) {
            const float ROWY = sc<float>(p.hover / L.cols) * (L.cellH + SP);
            if (ROWY < p.scroll)
                p.scroll = ROWY;
            else if (ROWY + L.cellH > p.scroll + L.grid.height)
                p.scroll = ROWY + L.cellH - sc<float>(L.grid.height);
        }
        p.scroll = std::clamp(p.scroll, 0.F, MAXSCROLL);

        p.rows.clear();
        for (size_t k = 0; k < p.visible.size(); ++k) {
            const auto& E = p.entries[p.visible[k]];
            CBox box{std::round(L.grid.x + (k % L.cols) * (L.cellW + SP)), std::round(L.grid.y + (k / L.cols) * (L.cellH + SP) - p.scroll), std::round(L.cellW), std::round(L.cellH)};
            if (box.y + box.height < L.grid.y || box.y > L.grid.y + L.grid.height)
                continue; // outside the viewport: not drawn, not hit
            p.rows.emplace_back(SRow{.box = box, .path = E.path, .name = E.name, .icon = E.icon});
        }
    }
```
Replace the grid branch of `render()` (the `if (g_popup->grid) { … return; }` block) with:
```cpp
        if (g_popup->grid) {
            auto&       P     = *g_popup;
            const auto  FG    = Config::themeForeground();
            const auto  MUTED = Config::themeMutedText();
            const auto  HEAD  = Config::themeHeading();
            drawPlate(P.plate, GRID_RADIUS, ALPHA);

            // Header: label, "12 of 340", and the sort on the right (clickable, see Step 5b).
            std::string label;
            for (const auto& ST : all())
                if (ST.path == P.path)
                    label = ST.label;
            const std::string COUNT = std::to_string(P.visible.size()) + " of " + std::to_string(P.entries.size());
            static const char* SORTS[] = {"Name", "Date Added", "Date Modified", "Kind"};
            const std::string  SORTL   = std::string(SORTS[sc<int>(P.sort)]) + "  \uf0dc";
            const int          HPX     = sc<int>(std::round(13.F * SCALE));
            float              hx      = sc<float>(P.headerBox.x);
            if (const auto T = OverviewRender::text(label, FG, HPX, false, "", sc<int>(P.headerBox.width * 0.5), 600); T && T->ok()) {
                drawTex(T, CBox{hx, P.headerBox.y + (P.headerBox.height - T->m_size.y) / 2.0, T->m_size.x, T->m_size.y}.round(), ALPHA);
                hx += sc<float>(T->m_size.x) + 10.F * SCALE;
            }
            if (const auto T = OverviewRender::text(COUNT, MUTED, sc<int>(std::round(11.F * SCALE)), false, "", 0, 500); T && T->ok())
                drawTex(T, CBox{hx, P.headerBox.y + (P.headerBox.height - T->m_size.y) / 2.0, T->m_size.x, T->m_size.y}.round(), ALPHA);
            if (const auto T = OverviewRender::text(SORTL, HEAD, sc<int>(std::round(11.F * SCALE)), false, "", 0, 600); T && T->ok()) {
                P.sortBox = CBox{P.headerBox.x + P.headerBox.width - T->m_size.x, P.headerBox.y + (P.headerBox.height - T->m_size.y) / 2.0, T->m_size.x, T->m_size.y}.round();
                drawTex(T, P.sortBox, ALPHA);
            }

            // The field: a darker inset plate, the query (or "Search"), a caret, and the Sushi hint when absent.
            Dock::frostedPlate(monitor, P.fieldBox, sc<int>(std::round(8.F * SCALE)), ALPHA * 0.5F);
            const bool        EMPTYQ = P.query.empty();
            const std::string SHOWN  = EMPTYQ ? (sushiAvailable() ? std::string{"Search"} : std::string{"Search \u00b7 install sushi for previews"}) : P.query;
            float             caretX = sc<float>(P.fieldBox.x) + 10.F * SCALE;
            if (const auto T = OverviewRender::text(SHOWN, EMPTYQ ? MUTED : FG, sc<int>(std::round(13.F * SCALE)), false, "", sc<int>(P.fieldBox.width - 20.F * SCALE), 500);
                T && T->ok()) {
                drawTex(T, CBox{caretX, P.fieldBox.y + (P.fieldBox.height - T->m_size.y) / 2.0, T->m_size.x, T->m_size.y}.round(), ALPHA);
                if (!EMPTYQ)
                    caretX += sc<float>(T->m_size.x) + 1.F;
            }
            CRectPassElement::SRectData caret;
            caret.box   = CBox{caretX, P.fieldBox.y + P.fieldBox.height * 0.25, std::max(1.0, 1.5 * SCALE), P.fieldBox.height * 0.5}.round();
            caret.color = Dock::accentColor().modifyA(ALPHA);
            g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(caret));

            // Cells: icon, two-line name, the previewed/selected/hovered ring; clipped to the grid.
            const CBox GRIDCLIP{P.plate.x, P.fieldBox.y + P.fieldBox.height, P.plate.width, P.plate.y + P.plate.height - (P.fieldBox.y + P.fieldBox.height)};
            for (size_t k = 0; k < P.rows.size(); ++k) {
                const auto& R    = P.rows[k];
                int         idx  = -1; // index into visible
                for (size_t v = 0; v < P.visible.size(); ++v)
                    if (P.entries[P.visible[v]].path == R.path)
                        idx = sc<int>(v);
                const bool PREV  = idx >= 0 && idx == P.previewing;
                const bool SEL   = P.keyboard && idx == P.hover;
                const bool HOVER = !P.keyboard && R.box.containsPoint(MOUSE);
                CBox       cell  = R.box;
                if (PREV) {
                    cell.x -= cell.width * 0.075;
                    cell.y -= cell.height * 0.075;
                    cell.width *= 1.15;
                    cell.height *= 1.15;
                }
                if (PREV || SEL || HOVER) {
                    CRectPassElement::SRectData hl;
                    hl.box     = cell;
                    hl.color   = Dock::accentColor().modifyA(ALPHA * (PREV ? 0.28F : SEL ? 0.2F : 0.12F));
                    hl.round   = sc<int>(std::round(10.F * SCALE));
                    hl.clipBox = GRIDCLIP;
                    g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(hl));
                }
                const float ICON = GRID_ICON * SCALE * (PREV ? 1.15F : 1.F);
                CBox        ib{std::round(cell.x + (cell.width - ICON) / 2.0), std::round(cell.y + 8.F * SCALE), std::round(ICON), std::round(ICON)};
                const auto  ENTRY = idx >= 0 ? P.entries[P.visible[sc<size_t>(idx)]] : StackModel::SEntry{};
                CTexPassElement::SRenderData d;
                d.tex     = iconTexture(ENTRY, sc<int>(ICON)); // Task 8; OverviewRender::appIcon(R.icon) until then
                d.box     = ib;
                d.a       = ALPHA;
                d.clipBox = GRIDCLIP;
                if (d.tex)
                    g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(d));
                if (const auto T = OverviewRender::textWrapped(R.name, FG, sc<int>(std::round(11.F * SCALE)), sc<int>(cell.width - 8.F * SCALE),
                                                               sc<int>(std::round(30.F * SCALE)), false, "", 500);
                    T && T->ok()) {
                    CTexPassElement::SRenderData l;
                    l.tex     = T;
                    l.box     = CBox{cell.x + (cell.width - T->m_size.x) / 2.0, ib.y + ICON + 6.F * SCALE, T->m_size.x, T->m_size.y}.round();
                    l.a       = ALPHA;
                    l.clipBox = GRIDCLIP;
                    g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(l));
                }
            }

            // States: loading, no matches, the cap.
            const char* STATE = P.loading && P.entries.empty() ? "Loading\u2026" : (P.visible.empty() && !P.query.empty() ? "No matches" : nullptr);
            if (STATE)
                if (const auto T = OverviewRender::text(STATE, MUTED, sc<int>(std::round(13.F * SCALE)), false, "", 0, 500); T && T->ok())
                    drawTex(T, CBox{GRIDCLIP.x + (GRIDCLIP.width - T->m_size.x) / 2.0, GRIDCLIP.y + (GRIDCLIP.height - T->m_size.y) / 2.0, T->m_size.x, T->m_size.y}.round(), ALPHA);
            if (P.truncated)
                if (const auto T = OverviewRender::text("showing first 5 000", MUTED, sc<int>(std::round(10.F * SCALE)), false, "", 0, 500); T && T->ok())
                    drawTex(T, CBox{P.plate.x + (P.plate.width - T->m_size.x) / 2.0, P.plate.y + P.plate.height - T->m_size.y - 4.F * SCALE, T->m_size.x, T->m_size.y}.round(), ALPHA);

            drawGhost();
            return;
        }
```
Add to `SPopup`: `CBox sortBox;`. Until Task 8 lands, write `OverviewRender::appIcon(R.icon)` where `iconTexture(...)` is used, and switch it in Task 8.

- [ ] **Step 3b: Sushi availability and the sort click** — at file scope in `DockStacks.cpp`:
```cpp
    // Spec: "No Sushi installed: Space does nothing and the pane header hints it."
    bool sushiAvailable() {
        static const bool AVAILABLE = [] {
            std::vector<std::string> dirs = {std::string(g_get_user_data_dir())};
            for (const char* const* D = g_get_system_data_dirs(); D && *D; ++D)
                dirs.emplace_back(*D);
            for (const auto& D : dirs)
                if (g_file_test((D + "/dbus-1/services/org.gnome.NautilusPreviewer.service").c_str(), G_FILE_TEST_EXISTS))
                    return true;
            return false;
        }();
        return AVAILABLE;
    }
```
At the top of `previewHovered()`: `if (!sushiAvailable()) return false;`. In `click()`, before the row loop of a press/release inside the popup:
```cpp
        if (g_popup->grid && !release && g_popup->sortBox.containsPoint(point)) {
            const auto NEXT = sc<StackModel::ESort>((sc<int>(g_popup->sort) + 1) % 4);
            setSort(g_popup->path, NEXT);
            g_popup->sort = NEXT;
            auto L        = sortedListing(g_popup->path, NEXT);
            g_popup->entries = std::move(L.entries);
            refilter(*g_popup);
            return true;
        }
```

- [ ] **Step 4: Typing** — expose the xkb state from main.cpp: in `IOverview.hpp` declare `xkb_state* overviewActiveXkbState();` and define it in main.cpp as `xkb_state* overviewActiveXkbState() { return activeXkbState(); }`. In `handleKey`, at the top when `P.grid`:
```cpp
        if (P.grid) {
            if (keycode == 14 /* BACKSPACE */) {
                if (!P.query.empty()) {
                    TextInput::popCodepoint(P.query);
                    refilter(P);
                    P.scroll = 0.F;
                    damagePopupMonitor();
                }
                return true;
            }
            if (keycode == 1 /* ESC */ && !P.query.empty() && P.previewing < 0) {
                P.query.clear();
                refilter(P);
                damagePopupMonitor();
                return true;
            }
            if (keycode != 57 /* SPACE previews */) {
                const auto K = TextInput::keyFor(overviewActiveXkbState(), keycode);
                if (K.consume) {
                    P.query += K.text;
                    refilter(P);
                    P.scroll = 0.F;
                    P.hover  = P.visible.empty() ? -1 : 0;
                    damagePopupMonitor();
                    return true;
                }
            }
        }
```
and at the end of `handleKey`, in pane mode, claim every remaining non-modifier key so it cannot reach the window underneath (the overview's rule):
```cpp
        if (P.grid)
            return true;
```
(Modifier keys never reach `handleKey` as claims because `TextInput::keyFor` returns `consume=false` for them and the switch has no case for them; the `return true` above must therefore come AFTER a check `if (isModifierLike(keycode)) return false;` — use the same keycodes main.cpp's `isModifierKeycode` lists: 29, 42, 54, 56, 97, 100, 125, 126.)

- [ ] **Step 5: Scroll** — `scroll()` in pane mode: `g_popup->scroll += (up ? -1.F : 1.F) * (Pane::CELL_H + Pane::SPACING) * SCALE * 0.5F;` (unchanged maths, new constants); `keyboard = false`.

- [ ] **Step 6: Build, install, live check** — `make all`, install/reload. With `~/stack-a-test` holding 60 files named `f1.txt … f60.txt` plus `photo.png`:
  - click the cell → the pane opens (theme plate), header "stack-a-test · 61 of 61 · Name";
  - `type photo` → header "1 of 61", one cell; `key BackSpace` ×5 → all back;
  - `key Down`, `key Right`, `key space` → Sushi beside the pane, the cell ringed; `key Escape` closes Sushi, `key Escape` clears (empty query already) → closes the pane;
  - Review Focus #4: after the pane closes, `act(steps=["focus Ghostty", "type echo ok"], require="ghostty")` in a scratch terminal types normally (use a terminal you opened for the test, not the user's).
  - Review Focus #3: `touch ~/stack-a-test/"$(printf 'a%.0s' {1..250}).txt"` → the cell's name wraps to two lines and elides.

- [ ] **Step 7: Commit**

```bash
git add StackPane.cpp DockStacks.hpp DockStacks.cpp IOverview.hpp main.cpp
git commit -m "feat(stacks): searchable pane -- filter-as-you-type, keyboard, scroll, loading/limit states"
```

---

### Task 8: The dock cell — the 4-item pile, thumbnails, missing folders, pile → fan

**Files:**
- Create: `StackIcon.cpp`
- Modify: `OverviewRender.hpp/.cpp` (`imageFile`)
- Modify: `DockStacks.hpp` (`drawCell`), `DockStacks.cpp` (pile → fan animation), `Dock.cpp:5940-5970` (call `drawCell` for stack items)

**Interfaces:**
- Consumes: `StackReader::listing`, `StackModel::sortEntries(…, ESort::Added)`.
- Produces:
  ```cpp
  namespace ScrollOverview::OverviewRender { SP<Render::ITexture> imageFile(const std::string& path, int px); }
  namespace ScrollOverview::Stacks {
      // Draws a stack's dock cell (pile, folder, empty or missing) into the pass. Returns false if the
      // caller should draw the plain icon instead (a pinned FILE, not a folder).
      bool drawCell(PHLMONITOR monitor, const Dock::SItem& item, const CBox& iconBox, float alpha);
      SP<Render::ITexture> iconTexture(const StackModel::SEntry& e, int px); // thumbnail or themed icon
  }
  ```

- [ ] **Step 1: `OverviewRender::imageFile`** — beside `appIcon`, same decode/upload code, keyed on path + mtime + px, cache cleared by `clearTextCache()`'s neighbour (add `g_imageFiles.clear()` where `g_overviewAppIcons` is cleared at unload):
```cpp
static std::unordered_map<std::string, SP<Render::ITexture>> g_imageFiles;

SP<Render::ITexture> imageFile(const std::string& path, int px) {
    if (path.empty() || px < 1)
        return nullptr;
    std::error_code ec;
    const auto      MT  = std::filesystem::last_write_time(path, ec);
    const auto      KEY = path + "\x01" + std::to_string(px) + "\x01" + std::to_string(ec ? 0 : MT.time_since_epoch().count());
    if (const auto IT = g_imageFiles.find(KEY); IT != g_imageFiles.end())
        return IT->second;
    if (g_imageFiles.size() > 256)
        g_imageFiles.clear();

    SP<Render::ITexture>  texture;
    Hyprgraphics::CImage image(path, {sc<double>(px), sc<double>(px)});
    if (image.success()) {
        if (const auto SURFACE = image.cairoSurface(); SURFACE && SURFACE->cairo()) {
            auto* const CAIRO = SURFACE->cairo();
            cairo_surface_flush(CAIRO);
            auto* const    DATA   = cairo_image_surface_get_data(CAIRO);
            const int      W      = cairo_image_surface_get_width(CAIRO);
            const int      H      = cairo_image_surface_get_height(CAIRO);
            const int      STRIDE = cairo_image_surface_get_stride(CAIRO);
            const uint32_t FORMAT = cairo_image_surface_get_format(CAIRO) == CAIRO_FORMAT_RGB24 ? DRM_FORMAT_XRGB8888 : DRM_FORMAT_ARGB8888;
            if (DATA && W > 0 && H > 0)
                texture = makeShared<Render::GL::CGLTexture>(FORMAT, DATA, sc<uint32_t>(STRIDE), Vector2D{sc<double>(W), sc<double>(H)});
        }
    }
    g_imageFiles[KEY] = texture;
    return texture;
}
```
(declare in OverviewRender.hpp; include `<filesystem>`).

- [ ] **Step 2: `StackIcon.cpp`**
```cpp
#include "DockStacks.hpp"
#include "Config.hpp"
#include "OverviewRender.hpp"

#include <gio/gio.h>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/BorderPassElement.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>

namespace ScrollOverview::Stacks {

    namespace {
        // The freedesktop thumbnail cache: ~/.cache/thumbnails/{large,normal}/md5(uri).png.
        // Only what is ALREADY there -- this never generates a thumbnail.
        std::string cachedThumbnail(const std::string& path) {
            char* uri = g_filename_to_uri(path.c_str(), nullptr, nullptr);
            if (!uri)
                return {};
            char* md5 = g_compute_checksum_for_string(G_CHECKSUM_MD5, uri, -1);
            g_free(uri);
            std::string out;
            for (const char* SIZE : {"large", "normal"}) {
                const std::string P = std::string(g_get_user_cache_dir()) + "/thumbnails/" + SIZE + "/" + md5 + ".png";
                if (g_file_test(P.c_str(), G_FILE_TEST_EXISTS)) {
                    out = P;
                    break;
                }
            }
            g_free(md5);
            return out;
        }

        // The pile's four slots: angle (deg), x/y offset as fractions of the cell,
        // scale. Bottom first; the newest item is drawn last (on top).
        struct SSlot { float deg, dx, dy, scale; };
        constexpr SSlot PILE[4] = {{-9.F, -0.10F, 0.06F, 0.70F}, {6.F, 0.09F, 0.03F, 0.72F}, {-3.F, -0.04F, -0.02F, 0.74F}, {2.F, 0.02F, -0.05F, 0.76F}};
    }

    SP<Render::ITexture> iconTexture(const StackModel::SEntry& e, int px) {
        if (!e.dir && (e.kind.starts_with("image/") || e.kind.starts_with("video/"))) {
            if (const auto T = cachedThumbnail(e.path); !T.empty())
                if (auto tex = OverviewRender::imageFile(T, px); tex)
                    return tex;
        }
        return OverviewRender::appIcon(e.icon);
    }

    bool drawCell(PHLMONITOR monitor, const Dock::SItem& item, const CBox& iconBox, float alpha) {
        if (!item.stackDir)
            return false;
        const SStack* stack = nullptr;
        for (const auto& X : all())
            if (X.path == item.stackPath)
                stack = &X;
        if (!stack)
            return false;

        const float S   = std::max(monitor->m_scale, 0.01F);
        auto        add = [&](const SP<Render::ITexture>& tex, CBox box, float rot, float a) {
            if (!tex)
                return;
            CTexPassElement::SRenderData d;
            d.tex     = tex;
            d.box     = box.round();
            d.box.rot = rot;
            d.a       = a;
            g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(d));
        };

        const auto L = StackReader::listing(stack->path);
        // MISSING (Review Focus #2): dimmed folder, the tooltip says why (Dock uses stack->label; the
        // "Folder not found" hint is added by the tooltip code in Step 4).
        if (L.ready && L.missing) {
            add(OverviewRender::appIcon(stack->icon), iconBox, 0.F, alpha * 0.35F);
            return true;
        }
        if (stack->display == StackModel::EDisplay::Folder) {
            add(OverviewRender::appIcon(stack->icon), iconBox, 0.F, alpha);
            return true;
        }

        // The outline: a thin rounded box in the theme's muted colour.
        CBox outline = iconBox;
        outline.expand(-iconBox.width * 0.06);
        CBorderPassElement::SBorderData border;
        border.box        = outline.round();
        border.grad1      = ::Config::CGradientValueData(ScrollOverview::Config::themeMutedText().modifyA(alpha * 0.8F));
        border.round      = sc<int>(std::round(outline.width * 0.18));
        border.borderSize = std::max(1, sc<int>(std::round(1.5F * S)));
        g_pHyprRenderer->m_renderPass.add(makeUnique<CBorderPassElement>(border));

        auto entries = L.entries;
        std::erase_if(entries, [](const StackModel::SEntry& e) { return e.dir; });
        StackModel::sortEntries(entries, StackModel::ESort::Added);
        const size_t N = std::min<size_t>(4, entries.size());
        if (N == 0) {
            // EMPTY: the folder icon inside the outline, until sub-project C draws its own.
            CBox inner = outline;
            inner.expand(-outline.width * 0.18);
            add(OverviewRender::appIcon(stack->icon), inner, 0.F, alpha * 0.85F);
            return true;
        }
        const int PX = sc<int>(std::round(iconBox.width));
        for (size_t k = 0; k < N; ++k) {
            const auto& E    = entries[N - 1 - k]; // oldest of the four first, newest last (on top)
            const auto& SL   = PILE[4 - N + k];
            const float W    = sc<float>(iconBox.width) * SL.scale;
            CBox        box{iconBox.x + iconBox.width * (0.5 + SL.dx) - W / 2.0, iconBox.y + iconBox.height * (0.5 + SL.dy) - W / 2.0, W, W};
            add(iconTexture(E, PX), box, SL.deg * 3.14159265F / 180.F, alpha);
        }
        return true;
    }
}
```
Declare `drawCell` and `iconTexture` in `DockStacks.hpp` (namespace scope, outside any anonymous namespace).

- [ ] **Step 3: Use it from the dock** — in `Dock.cpp` (the icon loop at ~5946), before the plain `appIcon` draw:
```cpp
        if (ITEM.stack) {
            CBox cell = ITEM.iconBox;
            cell.y -= Stacks::bounceLift(ITEM.stackPath, sc<float>(ITEM.iconBox.height));
            cell.x += Stacks::shakeOffset(ITEM.stackPath, sc<float>(ITEM.iconBox.width));
            if (Stacks::drawCell(monitor, ITEM, cell, 1.F))
                continue; // the running dot/badge are not drawn for stacks
        }
```
(Check that the rest of the loop body only draws things a stack does not have — dot, badge, urgent wiggle — so `continue` skips nothing a stack needs; if the tilt/squish code must still apply, move this block after it and apply `data.box` transforms to `cell` the same way.)

- [ ] **Step 4: "Folder not found" in the tooltip** — where the dock builds a stack's tooltip name (`itemName(*HOVEREDITEM)` in `renderTooltip`'s caller), for a stack item append `"  ·  " + suffix` when `const auto suffix = Stacks::tooltipSuffix(item.stackPath)` is non-empty. Declare `std::string tooltipSuffix(const std::string& stackPath);` in `DockStacks.hpp` now, implemented for the missing case only (Task 9 extends it with trash errors):
```cpp
    std::string tooltipSuffix(const std::string& stackPath) {
        if (const auto L = StackReader::listing(canonical(stackPath)); L.ready && L.missing)
            return "Folder not found";
        return {};
    }
``` In `Stacks::open()`, a missing folder shakes instead of opening: `if (L.ready && L.missing) { armShake(CANON); return; }`.

- [ ] **Step 5: Pile → fan** — in `layoutFan`, for the first 4 rows (the 4 newest items are rows 0-3 only when the stack's sort is Added/Modified; otherwise skip this and keep the plain grow-out), start each row's ICON from its pile slot: interpolate the icon box from the pile slot box (`PILE[…]` applied to `p.anchor`) to its row position with the row's own eased `E`, and the rotation from the slot's angle to the row's tilt. Record the pile boxes once at open in `SPopup::pileFrom[4]` (compute them with the same maths as `drawCell`). On close (`leaving`), the same interpolation runs backwards, so the rows fall back onto the pile. While a fan is open or closing, `drawCell` draws only the outline (the items are in flight): check `isOpen() || (g_popup && g_popup->path == item.stackPath)` and skip the item loop.

- [ ] **Step 6: Build, install, live check** — `make all`, install/reload.
  - Downloads cell shows a pile of the 4 newest (screenshot a 120×120 region around it, read it);
  - `Display as Folder` via `hyprctl dispatch` (or the menu after Task 9) → plain folder icon;
  - Review Focus #2: `mkdir ~/stack-gone && hyprctl dispatch <pin ~/stack-gone>`, then `rmdir ~/stack-gone` → the cell dims within a second, hovering shows "Folder not found", clicking shakes; unpin it.
  - Open and close the Downloads fan and watch the pile items fly into the first rows and back (two screenshots mid-animation are enough; set `animations:speed` slower temporarily only if it is too quick to see, and restore it).

- [ ] **Step 7: Commit**

```bash
git add StackIcon.cpp OverviewRender.hpp OverviewRender.cpp DockStacks.hpp DockStacks.cpp Dock.cpp
git commit -m "feat(stacks): 4-item pile cell with thumbnails, missing-folder state, pile-to-fan animation"
```

---

### Task 9: The context menu

**Files:**
- Modify: `Dock.cpp` (`ERow` additions, `MenuGlyph`, `buildRows` stack branch, `activateRow`, `menuClick` in-place swaps, `renderMenu` text-field row, a `menuHandleKey`)
- Modify: `Dock.hpp` (`menuHandleKey`, `menuWantsKeyboard`)
- Modify: `main.cpp` raw key hook (route to the menu while it edits)

**Interfaces:**
- Consumes: `Stacks::setSort/setDisplay/setViewMode/setLabel/unpin`, `StackReader::listing/trash/lastTrashResult`, `StackModel::relativeAge`, `TextInput::keyFor/popCodepoint`.
- Produces: `bool Dock::menuWantsKeyboard(); bool Dock::menuHandleKey(uint32_t keycode);` and, in `DockStacks.hpp`:
  ```cpp
  namespace ScrollOverview::Stacks {
      void        trashFrom(const std::string& stackPath, std::vector<std::string> paths); // queue + remember whose job it is
      std::string tooltipSuffix(const std::string& stackPath); // "Folder not found" / a recent trash error / ""
  }
  ```

- [ ] **Step 1: New rows** — extend `enum class ERow` with:
```cpp
    StackRecent,      // exec = the file's path
    StackSortKey,     // exec = sort name
    StackDisplayKey,  // exec = "stack" | "folder"
    StackViewKey,     // exec = "auto" | "grid"
    StackShowParent,
    StackRename,
    StackRenameField,
    StackTrashNewest,
    StackEmptyAsk,
    StackEmptyConfirm,
    StackEmptyCancel,
```
Add glyphs to `MenuGlyph`: `CHECK = ""`, `PENCIL = ""`, `CLOCK` exists, `FILE` exists, `TRASH` exists.

- [ ] **Step 2: The stack branch of `buildRows`** — replace the `if (item.stack) { … }` block (Dock.cpp:3028-3037) with:
```cpp
    if (item.stack) {
        const Stacks::SStack* S = nullptr;
        for (const auto& X : Stacks::all())
            if (X.path == item.stackPath)
                S = &X;
        if (item.stackDir && S) {
            // 1. Recent items: the 5 newest files.
            auto L = StackReader::listing(S->path);
            std::erase_if(L.entries, [](const StackModel::SEntry& e) { return e.dir; });
            StackModel::sortEntries(L.entries, StackModel::ESort::Added);
            const int64_t NOW = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
            for (size_t i = 0; i < std::min<size_t>(5, L.entries.size()); ++i)
                rows.emplace_back(SRow{.kind = ERow::StackRecent, .label = elide(L.entries[i].name), .exec = L.entries[i].path, .glyph = MenuGlyph::FILE,
                                       .hint = StackModel::relativeAge(NOW, L.entries[i].btime)});
            if (!L.entries.empty())
                rows.emplace_back(SRow{.kind = ERow::Separator});

            // 2-4. Sort by / Display as / View as, each checked.
            const auto CHECKED = [](bool on) { return on ? MenuGlyph::CHECK : ""; };
            for (auto K : {StackModel::ESort::Name, StackModel::ESort::Added, StackModel::ESort::Modified, StackModel::ESort::Kind}) {
                static const char* NAMES[] = {"Name", "Date Added", "Date Modified", "Kind"};
                rows.emplace_back(SRow{.kind = ERow::StackSortKey, .label = std::string("Sort by ") + NAMES[sc<int>(K)], .exec = StackModel::sortName(K), .glyph = CHECKED(S->sort == K)});
            }
            rows.emplace_back(SRow{.kind = ERow::Separator});
            rows.emplace_back(SRow{.kind = ERow::StackDisplayKey, .label = "Display as Stack", .exec = "stack", .glyph = CHECKED(S->display == StackModel::EDisplay::Stack)});
            rows.emplace_back(SRow{.kind = ERow::StackDisplayKey, .label = "Display as Folder", .exec = "folder", .glyph = CHECKED(S->display == StackModel::EDisplay::Folder)});
            rows.emplace_back(SRow{.kind = ERow::StackViewKey, .label = "View as Automatic", .exec = "auto", .glyph = CHECKED(S->view == StackModel::EView::Auto)});
            rows.emplace_back(SRow{.kind = ERow::StackViewKey, .label = "View as Grid", .exec = "grid", .glyph = CHECKED(S->view == StackModel::EView::Grid)});
            rows.emplace_back(SRow{.kind = ERow::Separator});
        }
        // 5-6.
        rows.emplace_back(SRow{.kind = ERow::StackOpen, .label = "Open in Files", .glyph = MenuGlyph::FOLDER});
        rows.emplace_back(SRow{.kind = ERow::StackShowParent, .label = "Show in Files", .glyph = MenuGlyph::FOLDER});
        rows.emplace_back(SRow{.kind = ERow::StackRename, .label = "Rename in Dock…", .glyph = MenuGlyph::PENCIL});
        // 7.
        if (item.stackDir) {
            rows.emplace_back(SRow{.kind = ERow::Separator});
            rows.emplace_back(SRow{.kind = ERow::StackTrashNewest, .label = "Move Newest to Trash", .glyph = MenuGlyph::TRASH, .danger = true});
            rows.emplace_back(SRow{.kind = ERow::StackEmptyAsk, .label = "Empty Folder…", .glyph = MenuGlyph::TRASH, .danger = true});
        }
        // 8.
        rows.emplace_back(SRow{.kind = ERow::Separator});
        rows.emplace_back(SRow{.kind = ERow::StackUnpin, .label = "Remove from Dock", .glyph = MenuGlyph::UNPIN});
        return rows;
    }
```
Delete the now-unused `stackIsGrid`/`stackIsByTime` helpers and the `StackView`/`StackSort` rows and cases (grep to confirm no other use).

- [ ] **Step 3: `activateRow` cases**
```cpp
        case ERow::StackRecent: Locations::activate("file://" + row.exec); return;
        case ERow::StackSortKey:
            if (const auto K = StackModel::sortFromName(row.exec))
                Stacks::setSort(item.stackPath, *K);
            return;
        case ERow::StackDisplayKey: Stacks::setDisplay(item.stackPath, row.exec == "folder" ? StackModel::EDisplay::Folder : StackModel::EDisplay::Stack); return;
        case ERow::StackViewKey: Stacks::setViewMode(item.stackPath, row.exec == "grid" ? StackModel::EView::Grid : StackModel::EView::Auto); return;
        case ERow::StackShowParent:
            Hyprutils::OS::CProcess("gdbus", {"call", "--session", "--dest", "org.freedesktop.FileManager1", "--object-path", "/org/freedesktop/FileManager1", "--method",
                                              "org.freedesktop.FileManager1.ShowItems", "['file://" + item.stackPath + "']", ""})
                .runAsync();
            return;
        case ERow::StackTrashNewest: {
            auto L = StackReader::listing(item.stackPath);
            std::erase_if(L.entries, [](const StackModel::SEntry& e) { return e.dir; });
            StackModel::sortEntries(L.entries, StackModel::ESort::Added);
            if (!L.entries.empty())
                Stacks::trashFrom(item.stackPath, {L.entries.front().path});
            return;
        }
        case ERow::StackEmptyConfirm: {
            const auto       L = StackReader::listing(item.stackPath);
            std::vector<std::string> paths;
            for (const auto& E : L.entries)
                paths.push_back(E.path);
            Stacks::trashFrom(item.stackPath, std::move(paths));
            return;
        }
        case ERow::StackRename: case ERow::StackRenameField: case ERow::StackEmptyAsk: case ERow::StackEmptyCancel: return; // handled in menuClick
```
Both trash rows call `Stacks::trashFrom(item.stackPath, {…})` instead of `StackReader::trash` directly. In `DockStacks.cpp`:
```cpp
    namespace {
        std::string     g_trashStack;   // whose job is in flight
        uint64_t        g_trashSeenSeq = 0;
        std::string     g_trashError;   // shown in that stack's tooltip for 3 s
        Time::steady_tp g_trashErrorAt{};
    }

    void trashFrom(const std::string& stackPath, std::vector<std::string> paths) {
        if (paths.empty())
            return;
        g_trashStack = canonical(stackPath);
        StackReader::trash(std::move(paths));
    }

    std::string tooltipSuffix(const std::string& stackPath) {
        const auto CANON = canonical(stackPath);
        if (const auto L = StackReader::listing(CANON); L.ready && L.missing)
            return "Folder not found";
        if (CANON == g_trashStack && !g_trashError.empty() && elapsed(g_trashErrorAt) < 3000.F)
            return g_trashError;
        return {};
    }
```
and at the end of `onReaderReady()`:
```cpp
        if (const auto R = StackReader::lastTrashResult(); R.seq != g_trashSeenSeq) {
            g_trashSeenSeq = R.seq;
            StackReader::invalidate(g_trashStack);
            if (!R.error.empty()) {
                g_trashError   = R.error + (R.moved ? " (" + std::to_string(R.moved) + " moved)" : "");
                g_trashErrorAt = Time::steadyNow();
                armShake(g_trashStack);
            } else
                armBounce(g_trashStack);
        }
```
This replaces Task 8's `tooltipSuffix` body (it keeps the missing-folder case and adds the trash error); the tooltip call site from Task 8 is unchanged.

- [ ] **Step 4: In-place swaps in `menuClick`** — extend the Get Info/Back branch:
```cpp
    if (ROW->kind == ERow::StackEmptyAsk) {
        const auto N = StackReader::listing(g_menu->item.stackPath).entries.size();
        g_menu->rows = {
            SRow{.kind = ERow::InfoLine, .label = "Move " + std::to_string(N) + (N == 1 ? " item" : " items") + " to Trash?", .glyph = MenuGlyph::TRASH},
            SRow{.kind = ERow::Separator},
            SRow{.kind = ERow::StackEmptyCancel, .label = "Cancel", .glyph = MenuGlyph::BACK},
            SRow{.kind = ERow::StackEmptyConfirm, .label = "Move to Trash", .glyph = MenuGlyph::TRASH, .danger = true},
        };
        return true;
    }
    if (ROW->kind == ERow::StackEmptyCancel) {
        g_menu->rows = buildRows(g_menu->item);
        return true;
    }
    if (ROW->kind == ERow::StackRename) {
        g_menu->editing  = true;
        g_menu->editText = g_menu->item.locationName;
        g_menu->rows     = {SRow{.kind = ERow::StackRenameField, .label = g_menu->editText, .glyph = MenuGlyph::PENCIL, .hint = "↵ save"}};
        return true;
    }
```
Add to `SMenu`: `bool editing = false; std::string editText;`. `renderMenu` draws a `StackRenameField` row's label as `editText` followed by a caret glyph `"│"` in the accent colour.

- [ ] **Step 5: Keys for the field** — in `Dock.cpp`:
```cpp
bool menuWantsKeyboard() {
    return g_menu && g_menu->editing;
}

bool menuHandleKey(uint32_t keycode) {
    if (!menuWantsKeyboard())
        return false;
    auto& M = *g_menu;
    if (keycode == 28 /* ENTER */) {
        Stacks::setLabel(M.item.stackPath, M.editText == Stacks::labelFor(M.item.stackPath) ? std::string{} : M.editText);
        closeMenu();
        return true;
    }
    if (keycode == 1 /* ESC */) {
        closeMenu();
        return true;
    }
    if (keycode == 14 /* BACKSPACE */)
        TextInput::popCodepoint(M.editText);
    else if (const auto K = TextInput::keyFor(overviewActiveXkbState(), keycode); K.consume)
        M.editText += K.text;
    else
        return true; // the field owns the keyboard while it is up
    M.rows.front().label = M.editText;
    return true;
}
```
Declare both in `Dock.hpp`. In `main.cpp`'s raw key hook, right before the stack routing added in Task 6:
```cpp
                if (ScrollOverview::Dock::menuWantsKeyboard() && ScrollOverview::Dock::menuHandleKey(event.keycode)) {
                    g_consumedPresses.insert(event.keycode);
                    info.cancelled = true;
                    return;
                }
```
An empty label on Enter restores the basename (`setLabel(path, "")`).

- [ ] **Step 6: Build, install, live check** — `make all`, install/reload. Right-click the Downloads cell (`rclick`, `require none`) and screenshot the menu: recent items with ages, the four sort rows with one checked, display and view rows checked, Open/Show in Files, Rename, the two danger rows, Remove from Dock. Then on the `~/stack-a-test` stack (never on Downloads):
  - `Sort by Kind` → the menu reopens with Kind checked, and the fan order changes;
  - `Rename in Dock…`, `type Tests`, `key Return` → tooltip says "Tests"; rename to empty restores "stack-a-test";
  - `Empty Folder…` → the confirm view with the right count; `Cancel` returns; `Move to Trash` → the folder empties (check `ls ~/stack-a-test`), the files are in `~/.local/share/Trash/files`;
  - `Remove from Dock` → the cell disappears, the folder still exists.
  Restore: `gio trash --restore` is not needed — the test files were scratch. Remove `~/stack-a-test`.

- [ ] **Step 7: Commit**

```bash
git add Dock.cpp Dock.hpp DockStacks.hpp DockStacks.cpp main.cpp
git commit -m "feat(stacks): context menu -- recent items, sort/display/view, show in files, rename, trash, remove"
```

---

### Task 10: Finish — checks, docs, full live pass

**Files:**
- Modify: `README.md` (Stacks section), `docs/CONFIG.md` (regenerate if any key changed — none are planned)
- Modify: `DockStacks.hpp` header comment (describe the new units; leave the drag-and-drop paragraph for sub-project B to correct, but add one line pointing at the spec's "Correction recorded for B")

- [ ] **Step 1: Full build and host tests**

Run: `make all 2>&1 | grep -E " error|FAIL|symcheck:|keycheck:|tests:"`
Expected: symcheck OK, keycheck OK, every `host_tests:` line OK including `stack_model_test` and `stack_reader_test`.

- [ ] **Step 2: Unload safety** — load, open a fan, press Space (Sushi up), then unload/load the plugin twice:
```bash
P=~/.local/lib/hyprdrive/hyprdrive.so; for i in 1 2; do hyprctl plugin unload $P; hyprctl plugin load $P; done; hyprctl plugin list | grep -c hyprdrive; coredumpctl list --since -5min | tail -2
```
Expected: `1`, and no new core dump.

- [ ] **Step 3: README** — replace the Stacks section with: what a stack is; pin via the Nautilus script or `hyprdrive:stack pin <path>` (drop-to-pin arrives with sub-project B); fan vs pane (automatic, or View as Grid); Space previews; the menu's items; the dock order; Downloads by default; the store path and that removing a stack never touches the folder.

- [ ] **Step 4: Commit**

```bash
git add README.md DockStacks.hpp
git commit -m "docs(stacks): README and header for dock stacks A"
```

- [ ] **Step 5: Hand back** — report to the user with the live screenshots from Tasks 6-9, anything that fell back (e.g. Sushi placement), and that B (drop into the dock) and C (icons) are next.
