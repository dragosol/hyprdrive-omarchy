#include "StackModel.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>

namespace {
    inline unsigned char sc_uchar(char c) { return static_cast<unsigned char>(c); }
    inline char          sc_char(unsigned char c) { return static_cast<char>(c); }
}


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
            return v == EView::Grid ? "grid" : v == EView::Fan ? "fan" : "auto";
        }
    }

    bool isPartialDownload(const std::string& name) {
        for (const std::string_view EXT : {".part", ".crdownload"})
            if (name.size() > EXT.size() && name.ends_with(EXT))
                return true;
        return false;
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
                .view    = P[3] == "grid" ? EView::Grid : P[3] == "fan" ? EView::Fan : EView::Auto,
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

    EView viewFromName(const std::string& name) {
        return name == "grid" ? EView::Grid : name == "fan" ? EView::Fan : EView::Auto;
    }

    size_t fanCapacity(const SFanFit& fit) {
        if (fit.stepL <= 0.F)
            return 0;
        const float ROOM = fit.pillTopL - fit.gapL - fit.topLimitL - fit.stepL;
        return ROOM < fit.stepL ? 0 : static_cast<size_t>(ROOM / fit.stepL);
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
