#pragma once

// /PROC/MOUNTS, READ BY HAND (Locations.cpp: every disk's trash, for the
// dock's Trash and Empty Trash). Kept apart from the GLib and Hyprland code so
// scripts/trash_dirs_test.cpp can run it on the host. Pure.

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace ScrollOverview::TrashDirs {

    // /proc/mounts writes a space, tab, newline or backslash inside a field as
    // \ooo. Decoded in ONE left-to-right pass, and only where three octal
    // digits follow (review 2026-11-09). The old loop searched from the start
    // again after every replacement, so the backslash it had just produced was
    // read as an escape: a drive labelled `foo\bar` (`foo\134bar`) became
    // std::stoi("bar", 8), which threw on the render thread and took the
    // compositor down within a second of the mount, and again at every login
    // while the drive stayed mounted. `a\134123` was decoded twice, to "aS".
    inline std::string unescapeMountField(std::string_view field) {
        const auto  OCTAL = [](char c) { return c >= '0' && c <= '7'; };
        std::string out;
        out.reserve(field.size());
        for (size_t i = 0; i < field.size(); ++i) {
            // \0oo..\3oo: one byte. Anything else is copied as it stands.
            if (field[i] == '\\' && i + 3 < field.size() && field[i + 1] <= '3' && OCTAL(field[i + 1]) && OCTAL(field[i + 2]) && OCTAL(field[i + 3])) {
                out += static_cast<char>((field[i + 1] - '0') * 64 + (field[i + 2] - '0') * 8 + (field[i + 3] - '0'));
                i += 3;
            } else
                out += field[i];
        }
        return out;
    }

    struct SMount {
        std::string device, dir, type;
    };

    // The first three fields of every line of a /proc/mounts table, decoded.
    // Split on the line, not scanned into fixed buffers: a long path cannot
    // spill into the next field.
    inline std::vector<SMount> parseMounts(std::string_view table) {
        std::vector<SMount> out;
        while (!table.empty()) {
            const size_t     EOL  = table.find('\n');
            std::string_view line = table.substr(0, EOL);
            table.remove_prefix(EOL == std::string_view::npos ? table.size() : EOL + 1);

            std::string_view fields[3];
            size_t           n = 0;
            while (n < 3 && !line.empty()) {
                const size_t SP = line.find(' ');
                fields[n++]     = line.substr(0, SP);
                line.remove_prefix(SP == std::string_view::npos ? line.size() : SP + 1);
            }
            if (n == 3 && !fields[0].empty() && !fields[1].empty())
                out.push_back({unescapeMountField(fields[0]), unescapeMountField(fields[1]), unescapeMountField(fields[2])});
        }
        return out;
    }
}
