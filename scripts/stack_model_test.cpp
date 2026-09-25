// StackModel: the stacks' pure core -- sort, filter, the store format, the fan's
// fit decision, relative ages. No compositor, no GLib.
#include "../StackModel.hpp"
#include "../TextInputUtf8.hpp"

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

    // View as Fan (2026-09-24): capacity agrees with fanFits, the name round-trips.
    {
        for (float top : {300.F, 700.F, 1100.F, 2000.F}) {
            const SFanFit F{.pillTopL = top, .topLimitL = 40.F};
            const size_t  N = fanCapacity(F);
            check(N == 0 ? !fanFits(1, F) : (fanFits(N, F) && !fanFits(N + 1, F)), "fanCapacity is the largest fitting row count");
        }
        check(viewFromName("fan") == EView::Fan && viewFromName("grid") == EView::Grid && viewFromName("auto") == EView::Auto && viewFromName("x") == EView::Auto, "viewFromName");
        SStore st;
        st.records.push_back({.path = "/d", .view = EView::Fan});
        check(parseStore(formatStore(st)).records[0].view == EView::Fan, "fan view round-trips");
    }

    std::printf("stack_model_test: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
