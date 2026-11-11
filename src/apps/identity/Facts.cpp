#include "Facts.hpp"

namespace Hyprdrive::AppIdentity {
    std::vector<std::pair<std::string, std::string>> parseEnviron(Bytes environ) {
        std::vector<std::pair<std::string, std::string>> out;
        try {
            const std::string_view T = asText(environ.first(std::min(environ.size(), Caps::ENVIRON)));
            for (size_t at = 0; at < T.size();) {
                const size_t           END = T.find('\0', at);
                const std::string_view KV  = T.substr(at, END == std::string_view::npos ? std::string_view::npos : END - at);
                at                         = END == std::string_view::npos ? T.size() : END + 1;
                const size_t EQ            = KV.find('=');
                if (EQ == std::string_view::npos || EQ == 0)
                    continue;
                const std::string_view K = KV.substr(0, EQ);
                for (const auto A : ENV_ALLOW)
                    if (K == A && std::ranges::none_of(out, [&](const auto& p) { return p.first == K; })) {
                        out.emplace_back(K, KV.substr(EQ + 1));
                        break;
                    }
            }
        } catch (...) { out.clear(); }
        return out;
    }

    std::string argv0Of(Bytes cmdline) {
        const std::string_view T = asText(cmdline);
        return std::string{T.substr(0, std::min(T.find('\0'), size_t{4096}))};
    }

    std::string joinCmdline(Bytes cmdline, size_t cap) {
        std::string out{asText(cmdline.first(std::min(cmdline.size(), cap)))};
        while (!out.empty() && out.back() == '\0')
            out.pop_back();
        for (char& c : out)
            if (c == '\0')
                c = ' ';
        return out;
    }

    std::optional<StatLine> parseStat(Bytes stat) {
        const std::string_view T  = asText(stat.first(std::min(stat.size(), size_t{4096})));
        const size_t           LP = T.find('('), RP = T.rfind(')');
        if (LP == std::string_view::npos || RP == std::string_view::npos || RP < LP)
            return std::nullopt;
        StatLine s{.comm = std::string{T.substr(LP + 1, RP - LP - 1)}};
        // After ") ": state(3) ppid(4) ... starttime(22).
        std::string_view rest = T.substr(RP + 1);
        int              field = 2;
        for (size_t at = 0; at < rest.size() && field < 22;) {
            while (at < rest.size() && rest[at] == ' ')
                ++at;
            size_t end = rest.find(' ', at);
            if (end == std::string_view::npos)
                end = rest.size();
            const std::string_view F = rest.substr(at, end - at);
            at                       = end;
            if (F.empty())
                break;
            ++field;
            uint64_t v = 0;
            if ((field == 4 || field == 22) && (!allDigits(F) || F.size() > 19))
                return std::nullopt;
            if (field == 4 || field == 22)
                for (char c : F)
                    v = v * 10 + static_cast<uint64_t>(c - '0');
            if (field == 4)
                s.ppid = static_cast<pid_t>(v);
            if (field == 22)
                s.startTime = v;
        }
        if (field < 22)
            return std::nullopt;
        return s;
    }
}
