#pragma once

// Lutris's library: pga.db's `games` rows (the database is read behind
// Lutris::Db, so tests need no sqlite: LiveSources.cpp dlopens the system's
// libsqlite3 the way GameApps.cpp does, and the fixtures carry a pga.tsv) and
// the `game:` section of a game's yml, the subset Lutris writes: two-space
// keys, plain or quoted scalars.

#include "Caps.hpp"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Hyprdrive::AppIdentity::Lutris {
    struct Row {
        int64_t     id = 0;
        std::string slug, name, runner, directory, configpath, service, serviceId;
        bool        installed = false;
    };

    struct Db {
        virtual ~Db() = default;
        // Every row of `games`, or nullopt when the database cannot be read.
        virtual std::optional<std::vector<Row>> games(const std::string& path) const = 0;
    };

    inline constexpr size_t MAX_ROWS = 10000;

    // `game:` keys of a Lutris game yml: exe, prefix, working_dir, args, appid.
    inline std::unordered_map<std::string, std::string> parseGameSection(Bytes in) {
        std::unordered_map<std::string, std::string> out;
        if (in.size() > Caps::YML)
            return out;
        const std::string_view T = asText(in);
        bool                   inGame = false;
        for (size_t at = 0; at < T.size();) {
            const size_t     NL   = T.find('\n', at);
            std::string_view line = T.substr(at, NL == std::string_view::npos ? std::string_view::npos : NL - at);
            at                    = NL == std::string_view::npos ? T.size() : NL + 1;
            if (line.ends_with('\r'))
                line.remove_suffix(1);
            if (line.empty() || line.front() == '#')
                continue;
            if (line.front() != ' ') {
                inGame = line.starts_with("game:");
                continue;
            }
            // Two-space keys only (deeper ones are other things' options).
            if (!inGame || line.size() < 3 || line[1] != ' ' || line[2] == ' ' || line[2] == '-')
                continue;
            const auto COLON = line.find(':');
            if (COLON == std::string_view::npos || COLON < 3)
                continue;
            const std::string KEY{line.substr(2, COLON - 2)};
            std::string_view  v = line.substr(COLON + 1);
            while (!v.empty() && v.front() == ' ')
                v.remove_prefix(1);
            while (!v.empty() && v.back() == ' ')
                v.remove_suffix(1);
            if (v.size() >= 2 && (v.front() == '\'' || v.front() == '"') && v.back() == v.front())
                v = v.substr(1, v.size() - 2);
            out.try_emplace(KEY, v);
        }
        return out;
    }

    // The fixtures' stand-in for pga.db: a header line naming the columns,
    // then one tab-separated row per game ("\N" or empty for NULL).
    inline std::vector<Row> parsePgaTsv(Bytes in) {
        std::vector<Row>       out;
        const std::string_view T = asText(in);
        std::vector<std::string> cols;
        for (size_t at = 0; at < T.size() && out.size() < MAX_ROWS;) {
            const size_t     NL   = T.find('\n', at);
            std::string_view line = T.substr(at, NL == std::string_view::npos ? std::string_view::npos : NL - at);
            at                    = NL == std::string_view::npos ? T.size() : NL + 1;
            if (line.ends_with('\r'))
                line.remove_suffix(1);
            std::vector<std::string_view> f;
            for (size_t s = 0; s <= line.size();) {
                const size_t TAB = line.find('\t', s);
                f.push_back(line.substr(s, TAB == std::string_view::npos ? std::string_view::npos : TAB - s));
                s = TAB == std::string_view::npos ? line.size() + 1 : TAB + 1;
            }
            if (cols.empty()) {
                for (const auto& F : f)
                    cols.emplace_back(F);
                continue;
            }
            Row r;
            for (size_t k = 0; k < f.size() && k < cols.size(); ++k) {
                const std::string_view V = f[k] == "\\N" ? std::string_view{} : f[k];
                const std::string&     C = cols[k];
                if (C == "id")
                    r.id = allDigits(V) && V.size() < 18 ? std::stoll(std::string{V}) : 0;
                else if (C == "slug")
                    r.slug = V;
                else if (C == "name")
                    r.name = V;
                else if (C == "runner")
                    r.runner = V;
                else if (C == "directory")
                    r.directory = V;
                else if (C == "configpath")
                    r.configpath = V;
                else if (C == "service")
                    r.service = V;
                else if (C == "service_id")
                    r.serviceId = V;
                else if (C == "installed")
                    r.installed = V == "1";
            }
            if (r.id > 0)
                out.push_back(std::move(r));
        }
        return out;
    }
}
