#pragma once

// Omarchy's two block files, parsed (the pure half of Rows.cpp; no Hyprland
// types: tests/unit/row_test.cpp).
//
//   monitor-workspaces          `block \t connector \t identity`, one line per
//                               monitor ever seen; the connector is empty while
//                               that monitor is unplugged. The rule is the
//                               bar's (shell Workspaces.qml): a non-empty
//                               connector gives connector -> block.
//   monitor-workspaces-merged   `id \t originalId \t originIdentity`, one line
//                               per guest Omarchy merged into another block.
//                               Hyprdrive only needs the ids (pinned desktops).
//
// Both are written whole by Omarchy (save()), so a line that does not parse
// means a file that is not what we think it is: the whole parse fails and the
// caller keeps what it had.

#include <cstdlib>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace Hyprdrive::RowsFile {
    inline std::vector<std::string_view> split(std::string_view line, char sep) {
        std::vector<std::string_view> out;
        size_t                        at = 0;
        for (;;) {
            const size_t NEXT = line.find(sep, at);
            out.push_back(line.substr(at, NEXT == std::string_view::npos ? std::string_view::npos : NEXT - at));
            if (NEXT == std::string_view::npos)
                return out;
            at = NEXT + 1;
        }
    }

    // A whole non-negative decimal number, or nothing.
    inline std::optional<long long> number(std::string_view s) {
        if (s.empty() || s.size() > 12)
            return std::nullopt;
        long long n = 0;
        for (const char C : s) {
            if (C < '0' || C > '9')
                return std::nullopt;
            n = n * 10 + (C - '0');
        }
        return n;
    }

    template <typename F>
    inline bool eachLine(std::string_view text, F&& f) {
        size_t at = 0;
        while (at < text.size()) {
            size_t end = text.find('\n', at);
            if (end == std::string_view::npos)
                end = text.size();
            std::string_view line = text.substr(at, end - at);
            if (!line.empty() && line.back() == '\r')
                line.remove_suffix(1);
            at = end + 1;
            if (line.empty())
                continue;
            if (!f(line))
                return false;
        }
        return true;
    }

    // connector -> block. nullopt: not a monitor-workspaces file.
    inline std::optional<std::map<std::string, long long>> blocks(std::string_view text) {
        std::map<std::string, long long> out;
        const bool                       OK = eachLine(text, [&](std::string_view line) {
            const auto F     = split(line, '\t');
            const auto BLOCK = F.empty() ? std::nullopt : number(F[0]);
            if (!BLOCK || F.size() < 2 || *BLOCK > 99)
                return false;
            if (!F[1].empty())
                out[std::string{F[1]}] = *BLOCK;
            return true;
        });
        if (!OK)
            return std::nullopt;
        return out;
    }

    // The guests' current ids. nullopt: not a merged file.
    inline std::optional<std::set<long long>> merged(std::string_view text) {
        std::set<long long> out;
        const bool          OK = eachLine(text, [&](std::string_view line) {
            const auto F  = split(line, '\t');
            const auto ID = F.empty() ? std::nullopt : number(F[0]);
            if (!ID || *ID < 1)
                return false;
            out.insert(*ID);
            return true;
        });
        if (!OK)
            return std::nullopt;
        return out;
    }

    // Omarchy's slotOf (its bar's label for any id): the place inside the
    // id's own block.
    inline long long slotOf(long long id, long long slots) {
        return slots > 0 && id >= 1 ? (id - 1) % slots + 1 : id;
    }
}
