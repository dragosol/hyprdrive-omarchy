#include "AppSearch.hpp"
#include "Launch.hpp"

#include "globals.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <sys/stat.h>
#include <ranges>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

#include <sys/resource.h>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include <hyprutils/os/Process.hpp>

// V3. The compositor's own exec, for the one launch that carries a window rule.
#include <hyprland/src/config/supplementary/executor/Executor.hpp>

using namespace Hyprutils::OS;

namespace AppSearch {
    namespace {
        constexpr const char* GENERIC_ICON = "application-x-executable";

        std::vector<SEntry> g_entries;
        bool                g_scanned = false;

        std::string         lower(std::string s) {
            std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return s;
        }

        // X18: what the host's strictMatch() gets from
        // `normalize('NFD').replace(/[̀-ͯ]/g, '').toLowerCase()` --
        // the accented Latin letters lose their marks, everything else is left
        // alone. Only the codepoints with a canonical decomposition are folded,
        // so AE/O-slash/L-stroke keep their identity here exactly as they do on
        // the host. ' ' marks "no fold".
        //
        // ponytail: outside the two-byte Latin range nothing is folded OR
        // lowercased, so a Cyrillic query must be typed in the title's own case.
        // Needs a case map, not a bigger fold table, and only if it ever bites.
        constexpr std::string_view FOLD_LATIN1 = "aaaaaa c"
                                                 "eeeeiiii"
                                                 " nooooo "
                                                 " uuuuy  "
                                                 "aaaaaa c"
                                                 "eeeeiiii"
                                                 " nooooo "
                                                 " uuuuy y"; // U+00C0..U+00FF

        constexpr std::string_view FOLD_LATINA = "aaaaaacc"
                                                 "ccccccdd"
                                                 "  eeeeee"
                                                 "eeeegggg"
                                                 "gggghh  "
                                                 "iiiiiiii"
                                                 "i   jjkk"
                                                 " llllll "
                                                 "   nnnnn"
                                                 "n   oooo"
                                                 "oo  rrrr"
                                                 "rrssssss"
                                                 "sstttt  "
                                                 "uuuuuuuu"
                                                 "uuuuwwyy"
                                                 "yzzzzzz "; // U+0100..U+017F

        std::string fold(const std::string& s) {
            std::string out;
            out.reserve(s.size());

            for (size_t i = 0; i < s.size();) {
                const auto C = static_cast<unsigned char>(s[i]);

                if (C < 0x80) {
                    out += static_cast<char>(std::tolower(C));
                    ++i;
                    continue;
                }

                // Only the two-byte range can hold a foldable letter; anything
                // longer is copied through a byte at a time.
                if ((C & 0xE0) == 0xC0 && i + 1 < s.size()) {
                    const auto CP = ((C & 0x1Fu) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3Fu);
                    if (CP >= 0xC0 && CP <= 0x17F) {
                        const char F = CP <= 0xFF ? FOLD_LATIN1[CP - 0xC0] : FOLD_LATINA[CP - 0x100];
                        if (F != ' ') {
                            out += F;
                            i += 2;
                            continue;
                        }

                        // No decomposition, so it stays itself -- but the host
                        // still lowercases it, and there are only eight of these
                        // outside Latin-1's own +0x20 run.
                        unsigned LOWER = CP;
                        switch (CP) {
                            case 0x110:
                            case 0x126:
                            case 0x132:
                            case 0x13F:
                            case 0x141:
                            case 0x14A:
                            case 0x152:
                            case 0x166: LOWER = CP + 1; break;
                            default:
                                if (CP >= 0xC0 && CP <= 0xDE && CP != 0xD7)
                                    LOWER = CP + 0x20;
                                break;
                        }

                        out += static_cast<char>(0xC0 | (LOWER >> 6));
                        out += static_cast<char>(0x80 | (LOWER & 0x3F));
                        i += 2;
                        continue;
                    }
                }

                out += s[i];
                ++i;
            }

            return out;
        }

        std::string trim(const std::string& s) {
            const auto B = s.find_first_not_of(" \t\r\n");
            if (B == std::string::npos)
                return "";
            const auto E = s.find_last_not_of(" \t\r\n");
            return s.substr(B, E - B + 1);
        }

        // XDG basedir order: $XDG_DATA_HOME first, then $XDG_DATA_DIRS. Earlier
        // entries win on desktop-id collision, which is what lets a user's
        // ~/.local override a system entry.
        std::vector<std::string> appDirs() {
            std::vector<std::string> dirs;

            const char* DATAHOME = std::getenv("XDG_DATA_HOME");
            const char* HOME     = std::getenv("HOME");
            if (DATAHOME && *DATAHOME)
                dirs.emplace_back(std::string{DATAHOME} + "/applications");
            else if (HOME && *HOME)
                dirs.emplace_back(std::string{HOME} + "/.local/share/applications");

            const char*       DATADIRS = std::getenv("XDG_DATA_DIRS");
            const std::string REST     = DATADIRS && *DATADIRS ? DATADIRS : "/usr/local/share:/usr/share";

            size_t start = 0;
            while (start <= REST.size()) {
                const auto  END  = REST.find(':', start);
                const auto  PART = REST.substr(start, END == std::string::npos ? std::string::npos : END - start);
                if (!PART.empty())
                    dirs.emplace_back(PART + "/applications");
                if (END == std::string::npos)
                    break;
                start = END + 1;
            }

            return dirs;
        }

        bool onPath(const std::string& binary) {
            if (binary.empty())
                return true;

            if (binary.contains('/')) {
                std::error_code ec;
                return std::filesystem::exists(binary, ec);
            }

            const char* PATHENV = std::getenv("PATH");
            if (!PATHENV || !*PATHENV)
                return true;

            const std::string PATHS = PATHENV;
            size_t            start = 0;
            while (start <= PATHS.size()) {
                const auto END  = PATHS.find(':', start);
                const auto PART = PATHS.substr(start, END == std::string::npos ? std::string::npos : END - start);
                if (!PART.empty()) {
                    std::error_code ec;
                    if (std::filesystem::exists(PART + "/" + binary, ec))
                        return true;
                }
                if (END == std::string::npos)
                    break;
                start = END + 1;
            }

            return false;
        }

        // Only the [Desktop Entry] group is read; later groups are actions and
        // must not overwrite the entry's own keys.
        bool parseEntry(const std::filesystem::path& path, const std::string& id, SEntry& out) {
            std::ifstream file(path);
            if (!file.is_open())
                return false;

            bool        inEntry = false;
            std::string type, tryExec;
            std::string line;

            while (std::getline(file, line)) {
                const auto L = trim(line);
                if (L.empty() || L.starts_with('#'))
                    continue;

                if (L.starts_with('[')) {
                    inEntry = L == "[Desktop Entry]";
                    continue;
                }

                if (!inEntry)
                    continue;

                const auto EQ = L.find('=');
                if (EQ == std::string::npos)
                    continue;

                const auto KEY = trim(L.substr(0, EQ));
                const auto VAL = trim(L.substr(EQ + 1));

                // Localised keys (Name[de]) are skipped for now: the C locale
                // name is what the fallback ranking uses.
                if (KEY == "Type")
                    type = VAL;
                else if (KEY == "Name")
                    out.name = VAL;
                else if (KEY == "GenericName")
                    out.generic = VAL;
                else if (KEY == "Comment")
                    out.comment = VAL;
                else if (KEY == "Keywords")
                    out.keywords = VAL;
                else if (KEY == "Exec")
                    out.exec = VAL;
                else if (KEY == "Icon")
                    out.icon = VAL;
                else if (KEY == "StartupWMClass")
                    out.wmClass = VAL;
                else if (KEY == "TryExec")
                    tryExec = VAL;
                else if (KEY == "Terminal")
                    out.terminal = VAL == "true";
                else if ((KEY == "NoDisplay" || KEY == "Hidden") && VAL == "true")
                    return false;
            }

            if (type != "Application" || out.name.empty() || out.exec.empty())
                return false;

            if (!onPath(tryExec))
                return false;

            out.id       = id;
            out.path     = path.string();
            out.haystack = lower(out.name + " " + out.generic + " " + out.comment + " " + out.keywords + " " + id);

            return true;
        }

        uint64_t g_inventoryGen = 1;

        void scan() {
            Prof::Timer PROF{Prof::scanNs, Prof::scanCalls};
            ++g_inventoryGen;
            g_entries.clear();

            std::unordered_set<std::string> seen;

            for (const auto& DIR : appDirs()) {
                std::error_code ec;
                if (!std::filesystem::is_directory(DIR, ec))
                    continue;

                for (auto it = std::filesystem::recursive_directory_iterator(DIR, std::filesystem::directory_options::skip_permission_denied, ec);
                     it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
                    if (ec)
                        break;

                    const auto& PATH = it->path();
                    if (PATH.extension() != ".desktop")
                        continue;

                    // Desktop id: path relative to the applications dir with
                    // separators turned into '-', per the XDG menu spec.
                    // Lexical, not std::filesystem::relative: that one resolves
                    // symlinks, so an entry symlinked out to a store path came
                    // back as "..-..-..-app-<hash>-export-share-..." and dragged
                    // the whole store path into the search haystack.
                    auto id = PATH.lexically_relative(DIR).string();
                    if (id.empty() || id.starts_with(".."))
                        id = PATH.filename().string();
                    std::ranges::replace(id, '/', '-');
                    if (id.ends_with(".desktop"))
                        id = id.substr(0, id.size() - 8);

                    if (seen.contains(id))
                        continue;

                    SEntry entry;
                    if (!parseEntry(PATH, id, entry))
                        continue;

                    seen.insert(id);
                    g_entries.emplace_back(std::move(entry));
                }
            }
        }

        // X18 wants parity with the search the user actually has on the host,
        // where fuzzy-application-search@mkhl.codeberg.page overrides the
        // shell's app search outright. So this is a port of that extension's
        // scoring.js (itself the TextMate ranker), not a hand-rolled matcher:
        // any divergence here is one the user would feel. Returns (0,1]; 0 is
        // no match. share/x18_parity.js diffs it against the real JS.
        constexpr double KEYWORD_WEIGHT = 0.9;
        constexpr int    FUZZY_SCALE    = 1'000'000;
        constexpr int    FUZZY_BASE     = 10;

        // ASCII stand-in for the JS \p{Letter}\p{Number}\p{Connector_Punctuation}:
        // any UTF-8 lead or continuation byte counts as letter-like but never as
        // a capital, so a non-latin name still gets its word starts marked.
        std::vector<bool> capitalIndexes(const std::string& text) {
            std::vector<bool> caps(text.size(), false);
            bool              newWord = true;
            for (size_t i = 0; i < text.size(); ++i) {
                const unsigned char C          = static_cast<unsigned char>(text[i]);
                const bool          LETTERLIKE = std::isalnum(C) || C == '_' || C >= 0x80;
                if ((newWord && LETTERLIKE) || (C < 0x80 && std::isupper(C)))
                    caps[i] = true;
                newWord = !LETTERLIKE;
            }
            return caps;
        }

        double fuzzyScore(const std::string& query, const std::string& text) {
            if (query == text)
                return 1.0;

            const size_t N = query.size(), M = text.size();
            if (N == 0 || M == 0 || N > M)
                return 0.0;

            const auto Q = lower(query);
            const auto T = lower(text);

            // The JS keeps first/last in Uint8Array, so the matrix path is only
            // valid while both lengths fit a byte and falls back to a subset
            // ratio above that. Same bounds here, same answer.
            if (N > 255 || M > 255 || N * M > 255 * 30) {
                // Faithful to upstream, including its quirk: the scan position
                // is not advanced past a hit, so a repeated character can match
                // the same index twice. Unreachable for app names anyway.
                size_t pos = 0;
                for (const char C : Q) {
                    const auto AT = T.find(C, pos);
                    if (AT == std::string::npos)
                        return 0.0;
                    pos = AT;
                }
                return static_cast<double>(N) / static_cast<double>(M);
            }

            const auto CAPS      = capitalIndexes(text);
            size_t     capsCount = 0;
            for (const bool B : CAPS)
                capsCount += B ? 1 : 0;

            std::vector<uint8_t> matrix(N * M, 0);
            auto                 at = [&](size_t i, size_t j) -> uint8_t& { return matrix[i * M + j]; };

            std::vector<size_t> first(N, M), last(N, 0);

            for (size_t i = 0; i < N; ++i) {
                bool   found = false;
                size_t j     = i == 0 ? 0 : first[i - 1] + 1;
                for (; j < M; ++j) {
                    if (Q[i] != T[j])
                        continue;
                    at(i, j) = 1;
                    if (!found)
                        first[i] = j;
                    last[i] = j + 1;
                    found   = true;
                }
                if (!found)
                    return 0.0;
            }

            for (size_t i = N - 1; i > 0; --i) {
                size_t bound = last[i] - 1;
                if (bound >= last[i - 1])
                    continue;
                // JS reads matrix[i-1][bound-1]; at bound 0 that is undefined
                // and ends the loop, so stopping at 0 is the same behaviour.
                while (bound > 0 && at(i - 1, bound - 1) == 0)
                    --bound;
                last[i - 1] = bound;
            }

            for (size_t i = N - 1; i > 0; --i) {
                for (size_t j = first[i]; j < last[i]; ++j) {
                    if (j > 0 && at(i, j) && at(i - 1, j - 1))
                        at(i - 1, j - 1) = static_cast<uint8_t>(at(i, j) + 1);
                }
            }

            size_t capitalsTouched = 0, substrings = 0, prefixLength = 0;

            for (size_t i = 0; i < N;) {
                size_t bestJIndex = 0, bestJLength = 0;
                for (size_t j = first[i]; j < last[i]; ++j) {
                    if (at(i, j) && CAPS[j]) {
                        bestJIndex  = j;
                        bestJLength = at(i, j);
                        for (size_t k = j; k < j + bestJLength && k < M; ++k) {
                            if (CAPS[k])
                                ++capitalsTouched;
                        }
                    } else if (bestJLength < at(i, j)) {
                        bestJIndex  = j;
                        bestJLength = at(i, j);
                    }
                }

                if (i == 0)
                    prefixLength = bestJIndex;

                // Upstream would spin forever here; a hang on the compositor's
                // input path is not an acceptable way to be faithful.
                if (bestJLength == 0)
                    break;

                size_t length        = 0;
                bool   foundCapital  = false;
                while (length < bestJLength && !foundCapital) {
                    ++i;
                    ++length;
                    if (i == N)
                        break;

                    first[i] = std::max(bestJIndex + length, first[i]);
                    if (length < bestJLength && N < 4) {
                        if (first[i] < M && CAPS[first[i]])
                            continue;
                        for (size_t j = first[i]; j < last[i]; ++j) {
                            if (at(i, j) && CAPS[j])
                                foundCapital = true;
                        }
                    }
                }

                ++substrings;
            }

            // capitalsTouched accumulates across the whole j scan and CAN exceed
            // N -- upstream just goes negative there, but in size_t it wraps to
            // ~1.8e19 and the score comes back as garbage. Do this arithmetic in
            // double, not in the index type.
            const double DENOM    = static_cast<double>(N) * (N + 1) + 1;
            const double SUBTRACT = N == capitalsTouched ? 1.0 : static_cast<double>(substrings) * N + (static_cast<double>(N) - static_cast<double>(capitalsTouched));

            double s = (DENOM - SUBTRACT) / DENOM;
            s += static_cast<double>(M - prefixLength) / M / (2 * DENOM);
            if (capsCount > 0)
                s += static_cast<double>(capitalsTouched) / capsCount / (4 * DENOM);
            s += static_cast<double>(N) / M / (8 * DENOM);

            return s;
        }

        std::vector<std::string> splitKeywords(const std::string& keywords) {
            std::vector<std::string> out;
            size_t                   start = 0;
            while (start <= keywords.size()) {
                const auto END  = keywords.find(';', start);
                auto       PART = trim(keywords.substr(start, END == std::string::npos ? std::string::npos : END - start));
                if (!PART.empty())
                    out.emplace_back(std::move(PART));
                if (END == std::string::npos)
                    break;
                start = END + 1;
            }
            return out;
        }

        // The host extension scores the name at 1.0 and each keyword at 0.9 and
        // keeps the best, so ranking among matches it would produce is identical.
        // `query` is the raw text, NOT lowercased: upstream's exact-equality fast
        // path is case sensitive and lowering first would silently skip it.
        int score(const SEntry& entry, const std::string& query) {
            // Guard the fallback below: haystack.contains("") is true for every
            // entry, so an empty query would match the whole inventory.
            if (query.empty())
                return -1;

            double best = fuzzyScore(query, entry.name);

            for (const auto& KW : splitKeywords(entry.keywords))
                best = std::max(best, fuzzyScore(query, KW) * KEYWORD_WEIGHT);

            if (best > 0.0)
                return FUZZY_BASE + static_cast<int>(best * FUZZY_SCALE);

            // Deliberately NOT parity. The extension only ever sees name and
            // keywords, so going pure-parity would stop "nautilus" finding
            // Files. Every fuzzy hit outranks this whole band, so host ordering
            // is untouched and this only adds results the host would not show.
            if (entry.haystack.contains(lower(query)))
                return 1;

            return -1;
        }

        // App icons in practice live in hicolor, NOT in the active theme:
        // measured in the guest, Adwaita ships only 16x16/scalable/symbolic UI
        // icons and every app icon (kitty, org.gnome.Nautilus, foot,
        // pavucontrol...) is under /usr/share/icons/hicolor. That is why this
        // does not parse index.theme or walk Inherits= chains -- a filename
        // search over a few roots answers the question the scope was worried
        // about, and an SVG rasterises at whatever size the caller wants.
        std::string iconThemeName() {
            const char* ENVTHEME = std::getenv("XDG_ICON_THEME");
            if (ENVTHEME && *ENVTHEME)
                return ENVTHEME;

            const char* HOME = std::getenv("HOME");
            if (HOME && *HOME) {
                std::ifstream settings(std::string{HOME} + "/.config/gtk-3.0/settings.ini");
                std::string   line;
                while (std::getline(settings, line)) {
                    const auto L = trim(line);
                    if (!L.starts_with("gtk-icon-theme-name"))
                        continue;
                    const auto EQ = L.find('=');
                    if (EQ != std::string::npos)
                        return trim(L.substr(EQ + 1));
                }
            }

            return "Adwaita";
        }

        // The bases that hold an /icons tree, in XDG priority order.
        std::vector<std::string> iconBases() {
            std::vector<std::string> bases;
            for (const auto& DIR : appDirs()) {
                // appDirs() hands back ".../applications"; icons are a sibling.
                bases.emplace_back(DIR.substr(0, DIR.size() - std::string{"/applications"}.size()) + "/icons");
            }
            const char* HOME = std::getenv("HOME");
            if (HOME && *HOME)
                bases.emplace_back(std::string{HOME} + "/.icons");
            return bases;
        }

        // App icons live in hicolor, but NAMED freedesktop icons
        // (preferences-system, applications-other) live in the theme and its
        // ancestors: measured in the guest, Adwaita inherits AdwaitaLegacy then
        // hicolor, and preferences-system exists only in AdwaitaLegacy. So the
        // Inherits= chain does have to be walked -- just the chain, not the
        // per-directory Size/Context/Type machinery, which the filename search
        // makes unnecessary.
        std::vector<std::string> themeChain(const std::string& theme) {
            std::vector<std::string> chain, queue{theme};

            for (size_t head = 0; head < queue.size() && chain.size() < 16; ++head) {
                const auto NAME = queue[head];
                if (NAME.empty() || std::ranges::contains(chain, NAME))
                    continue;
                chain.emplace_back(NAME);

                for (const auto& BASE : iconBases()) {
                    std::ifstream index(BASE + "/" + NAME + "/index.theme");
                    if (!index.is_open())
                        continue;

                    std::string line;
                    while (std::getline(index, line)) {
                        const auto L = trim(line);
                        if (!L.starts_with("Inherits"))
                            continue;
                        const auto EQ = L.find('=');
                        if (EQ == std::string::npos)
                            continue;

                        const auto VAL = trim(L.substr(EQ + 1));
                        size_t     start = 0;
                        while (start <= VAL.size()) {
                            const auto END  = VAL.find(',', start);
                            const auto PART = trim(VAL.substr(start, END == std::string::npos ? std::string::npos : END - start));
                            if (!PART.empty())
                                queue.emplace_back(PART);
                            if (END == std::string::npos)
                                break;
                            start = END + 1;
                        }
                        break;
                    }
                    break;
                }
            }

            if (!std::ranges::contains(chain, std::string{"hicolor"}))
                chain.emplace_back("hicolor");

            return chain;
        }

        // Higher is better. SVG wins outright: one file scales to any row
        // height, so nothing has to be re-picked when the card transform
        // changes. Otherwise prefer the largest raster, parsed out of the
        // "256x256" style directory name.
        int iconCandidateScore(const std::filesystem::path& path) {
            if (path.extension() == ".svg")
                return 100000;

            if (path.extension() != ".png" && path.extension() != ".xpm")
                return -1;

            int        size = 0;
            const auto PARENTS = path.parent_path().string();
            for (size_t i = 0; i < PARENTS.size(); ++i) {
                if (!std::isdigit(static_cast<unsigned char>(PARENTS[i])))
                    continue;
                int n = 0;
                while (i < PARENTS.size() && std::isdigit(static_cast<unsigned char>(PARENTS[i])))
                    n = (n * 10) + (PARENTS[i++] - '0');
                size = std::max(size, n);
            }

            // A themeless location (pixmaps) still beats nothing.
            return std::min(size, 4096);
        }

        // Desktop Exec field codes. %% is a literal percent and must be handled
        // before the rest, or "%%f" would lose its f.
        std::string stripFieldCodes(const std::string& exec) {
            std::string out;
            out.reserve(exec.size());

            for (size_t i = 0; i < exec.size(); ++i) {
                if (exec[i] != '%') {
                    out.push_back(exec[i]);
                    continue;
                }

                if (i + 1 >= exec.size())
                    break;

                const char CODE = exec[++i];
                if (CODE == '%')
                    out.push_back('%');
                // every other code expands to nothing: no file, no url, no icon
            }

            return out;
        }

        std::vector<std::string> splitArgs(const std::string& cmd) {
            std::vector<std::string> args;
            std::string              current;
            bool                     quoted = false, any = false;

            for (size_t i = 0; i < cmd.size(); ++i) {
                const char C = cmd[i];

                if (C == '\\' && i + 1 < cmd.size() && quoted) {
                    current.push_back(cmd[++i]);
                    any = true;
                } else if (C == '"') {
                    quoted = !quoted;
                    any    = true;
                } else if (!quoted && (C == ' ' || C == '\t')) {
                    if (any)
                        args.emplace_back(current);
                    current.clear();
                    any = false;
                } else {
                    current.push_back(C);
                    any = true;
                }
            }

            if (any)
                args.emplace_back(current);

            return args;
        }

        // Chromium --app=URL (Omarchy's omarchy-launch-webapp execs that) sets
        // the Wayland app_id to chrome-{host}_{path}-Default, with '/' in the
        // path turned into '_'. Omarchy webapp .desktop files have no
        // StartupWMClass, so the dock would otherwise see a running window and
        // no Icon=.
        std::string chromeAppClassStem(const std::string& exec) {
            std::string url;
            const auto  ARGS = splitArgs(stripFieldCodes(exec));
            for (size_t i = 0; i < ARGS.size(); ++i) {
                if (ARGS[i].starts_with("--app=")) {
                    url = ARGS[i].substr(6);
                    break;
                }
                if (ARGS[i] == "omarchy-launch-webapp" && i + 1 < ARGS.size()) {
                    url = ARGS[i + 1];
                    break;
                }
            }
            if (url.empty())
                return {};

            std::string rest = url;
            if (const auto SCHEME = rest.find("://"); SCHEME != std::string::npos)
                rest = rest.substr(SCHEME + 3);
            if (rest.empty() || rest.starts_with('/'))
                return {};

            std::string host, path;
            if (const auto SLASH = rest.find('/'); SLASH != std::string::npos) {
                host = rest.substr(0, SLASH);
                path = rest.substr(SLASH);
            } else {
                host = rest;
                path = "/";
            }

            if (const auto AT = host.find('@'); AT != std::string::npos)
                host = host.substr(AT + 1);
            if (const auto CUT = host.find_first_of("?#"); CUT != std::string::npos)
                host = host.substr(0, CUT);
            if (const auto CUT = path.find_first_of("?#"); CUT != std::string::npos)
                path = path.substr(0, CUT);
            if (host.empty())
                return {};

            // GenerateApplicationNameFromURL is host + "_" + path; WM_CLASS then
            // replaces remaining '/' with '_'. Profile suffix is appended later
            // ("-Default"), so this stem stops before that.
            std::string stem = "chrome-" + host + "_" + path;
            for (char& c : stem) {
                if (c == '/')
                    c = '_';
            }
            return lower(stem);
        }

        bool chromeAppClassMatches(const std::string& want, const std::string& exec) {
            const auto STEM = chromeAppClassStem(exec);
            if (STEM.empty() || !want.starts_with(STEM))
                return false;
            return want.size() == STEM.size() || want[STEM.size()] == '-';
        }
    }

    const std::vector<SEntry>& inventory() {
        if (!g_scanned) {
            g_scanned = true;
            scan();
        }
        return g_entries;
    }

    void refresh() {
        g_scanned = true;
        scan();
    }

    uint64_t inventoryGeneration() {
        inventory(); // a first call must not report generation 1 for an unscanned list
        return g_inventoryGen;
    }

    std::vector<const SEntry*> search(const std::string& query, size_t limit) {
        Prof::Timer PROF{Prof::appNs, Prof::appCalls};
        std::vector<const SEntry*> results;
        if (query.empty() || limit == 0)
            return results;

        std::vector<std::pair<int, const SEntry*>> scored;
        for (const auto& ENTRY : inventory()) {
            const int S = score(ENTRY, query);
            if (S >= 0)
                scored.emplace_back(S, &ENTRY);
        }

        std::ranges::stable_sort(scored, [](const auto& a, const auto& b) { return a.first > b.first; });

        for (const auto& [SCORE, ENTRY] : scored) {
            if (results.size() >= limit)
                break;
            results.emplace_back(ENTRY);
        }

        return results;
    }

    // OVERVIEW_CPP_SPEC §13.12 X52. `stem -> best path`, built once.
    //
    // This used to walk the icon-theme roots recursively PER ICON, comparing
    // `path.stem()` against the name. Profiled at ~250ms for a single cold
    // icon -- 99% of the whole keystroke-to-paint cost of the search -- and a
    // miss then recursed into GENERIC_ICON and walked them all a second time.
    // A full pass over this machine's ~20k icon files costs ~10ms, so doing it
    // ONCE and hashing afterwards is the entire fix. See RICE_PROGRESS X52.
    struct SIconCandidate {
        std::string path;
        int         score = -1;
        size_t      root  = 0;
    };

    static std::unordered_map<std::string, SIconCandidate> g_iconIndex;
    static std::string                                     g_iconIndexTheme;
    static bool                                            g_iconIndexBuilt = false;

    static std::vector<std::string> iconRoots(const std::string& theme) {
        std::vector<std::string> roots;
        for (const auto& THEME : themeChain(theme)) {
            for (const auto& BASE : iconBases())
                roots.emplace_back(BASE + "/" + THEME);
        }
        roots.emplace_back("/usr/share/pixmaps");
        return roots;
    }

    // Y13. The scan below walks EVERY icon-theme root recursively. Measured at
    // plugin load 2026-08-06: 4237ms, against the 10ms the X51 note beside its
    // call site claimed. `hl.plugin.load` is PARSE-TIME, so all of it landed
    // before exec-once ran -- the wallpaper appeared 13.5s into the session
    // with the plugin loaded and 7.5s without it, and this was 4.2s of that
    // 6s difference.
    //
    // It is a CACHE, so it does not have to be finished before the compositor
    // starts; it only has to be finished before something asks. The scan now
    // runs on a worker and every reader takes the same mutex, so an early
    // lookup simply waits for it -- which is exactly the old behaviour, and is
    // why this is a scheduling change rather than a correctness one.
    std::mutex  g_iconIndexMutex;
    std::thread g_iconIndexWorker;

    static std::atomic<uint64_t> g_iconIndexGen{1};
    static std::atomic<bool>     g_iconRebuildRunning{false};

    // The walk itself, into a map nobody else can see yet. Pure apart from the
    // filesystem, so it can run on the worker while readers keep the old index.
    static std::unordered_map<std::string, SIconCandidate> buildIconIndex(const std::string& theme) {
        std::unordered_map<std::string, SIconCandidate> index;

        std::error_code ec;
        const auto      ROOTS = iconRoots(theme);

        for (size_t r = 0; r < ROOTS.size(); ++r) {
            if (!std::filesystem::is_directory(ROOTS[r], ec))
                continue;

            for (auto it = std::filesystem::recursive_directory_iterator(ROOTS[r], std::filesystem::directory_options::skip_permission_denied, ec);
                 it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
                if (ec)
                    break;

                const auto& PATH  = it->path();
                const int   SCORE = iconCandidateScore(PATH);
                // Negative means "not an icon file" -- the old loop could never
                // pick one either, because it started bestScore at -1. Skipping
                // here also keeps directories out of the index.
                if (SCORE < 0)
                    continue;

                auto& SLOT = index[PATH.stem().string()];

                // Roots are in priority order and the old code stopped at the
                // first root that answered, so a later root must never override
                // an earlier one -- only beat a sibling inside the same root.
                if (SLOT.score < 0)
                    SLOT = SIconCandidate{PATH.string(), SCORE, r};
                else if (SLOT.root == r && SCORE > SLOT.score) {
                    SLOT.path  = PATH.string();
                    SLOT.score = SCORE;
                }
            }
        }

        return index;
    }

    void warmIconIndexLocked() {
        const auto THEME = iconThemeName();
        if (g_iconIndexBuilt && g_iconIndexTheme == THEME)
            return;

        g_iconIndex      = buildIconIndex(THEME);
        g_iconIndexTheme = THEME;
        g_iconIndexBuilt = true;
        ++g_iconIndexGen;
    }

    void warmIconIndex() {
        std::scoped_lock lock(g_iconIndexMutex);
        warmIconIndexLocked();
    }

    // Started from PLUGIN_INIT. Joined in shutdownIconIndex() rather than
    // detached: a detached worker would still be writing g_iconIndex after the
    // .so is unloaded, and hyprpm reloads the plugin in-process.
    void warmIconIndexAsync() {
        if (g_iconIndexWorker.joinable())
            return;

        // Up for the warm-up too, so pollForChanges never joins -- and so never
        // blocks a frame on -- a scan that is still walking the themes.
        g_iconRebuildRunning = true;
        g_iconIndexWorker    = std::thread([] {
            // Y13. NICED, and the measurement is why: moving the scan off the
            // parse-time path bought 4.1s on when `qs` launches but only 2.3s
            // on when the wallpaper lands, because the worker then spent that
            // time competing with the session bring-up for a VM's few cores.
            // Nothing waits on this until something asks for an icon, so it is
            // exactly the work that should yield.
            ::setpriority(PRIO_PROCESS, 0, 10);

            {
                std::scoped_lock lock(g_iconIndexMutex);
                warmIconIndexLocked();
            }
            g_iconRebuildRunning = false;
        });
    }

    // ---- OTHER THEMES (dock stacks, 2026-09-24) ------------------------------
    // The stacks and the Trash can draw from a theme other than the one the
    // dock's app icons use (the Omarchy theme's icons.theme, or GTK's). Each
    // such theme gets its own index, built once on its own worker exactly as
    // the main one is -- a cold theme walk has measured in seconds -- and
    // until it lands a lookup answers from the main theme.
    std::mutex                                                              g_altMutex;
    std::unordered_map<std::string, std::unordered_map<std::string, SIconCandidate>> g_altIndex; // theme -> index
    std::string                                                             g_altBuilding;       // theme on the worker
    std::thread                                                             g_altWorker;

    std::string omarchyIconTheme() {
        static std::string                           cached;
        static std::chrono::steady_clock::time_point at{};
        const auto                                   NOW = std::chrono::steady_clock::now();
        if (at != std::chrono::steady_clock::time_point{} && NOW - at < std::chrono::seconds(2))
            return cached;
        at               = NOW;
        const char* HOME = std::getenv("HOME");
        std::string name;
        if (HOME && *HOME) {
            std::ifstream f(std::string{HOME} + "/.local/state/omarchy/current/theme/icons.theme");
            std::getline(f, name);
            name = trim(name);
        }
        cached = name;
        return cached;
    }

    std::string resolveIconPathInTheme(const std::string& iconName, const std::string& theme) {
        if (iconName.empty() || iconName.starts_with('/') || theme.empty() || theme == iconThemeName())
            return resolveIconPath(iconName);
        {
            std::scoped_lock lock(g_altMutex);
            if (const auto IT = g_altIndex.find(theme); IT != g_altIndex.end()) {
                if (const auto HIT = IT->second.find(iconName); HIT != IT->second.end())
                    return HIT->second.path;
                return resolveIconPath(iconName); // not in that theme's chain at all
            }
            if (g_altBuilding.empty()) {
                if (g_altWorker.joinable())
                    g_altWorker.join(); // the previous theme's worker, long finished
                g_altBuilding = theme;
                g_altWorker   = std::thread([theme] {
                    ::setpriority(PRIO_PROCESS, 0, 10);
                    auto index = buildIconIndex(theme);
                    {
                        std::scoped_lock lock(g_altMutex);
                        g_altIndex[theme] = std::move(index);
                        g_altBuilding.clear();
                    }
                    ++g_iconIndexGen; // textures keyed on names re-resolve
                });
            }
        }
        return resolveIconPath(iconName);
    }

    void shutdownIconIndex() {
        if (g_iconIndexWorker.joinable())
            g_iconIndexWorker.join();
        if (g_altWorker.joinable())
            g_altWorker.join();
    }

    uint64_t iconIndexGeneration() {
        return g_iconIndexGen.load();
    }

    // mtimes of the directories whose contents decide the answers. A directory's
    // mtime moves when an entry is added, removed or renamed in it, which is what
    // an install does (and what install scripts that rm + rewrite do). Icon files
    // land deep inside a theme (hicolor/256x256/apps), so a theme is watched
    // through its icon-theme.cache as well -- the pacman hook regenerates that
    // on every package that ships icons.
    static std::vector<int64_t> stampOf(const std::vector<std::string>& paths) {
        std::vector<int64_t> out;
        out.reserve(paths.size());
        for (const auto& PATH : paths) {
            struct stat st{};
            out.emplace_back(::stat(PATH.c_str(), &st) == 0 ? sc<int64_t>(st.st_mtim.tv_sec) * 1'000'000'000LL + st.st_mtim.tv_nsec : -1);
        }
        return out;
    }

    static std::vector<std::string> iconWatchPaths() {
        std::vector<std::string> paths;
        for (const auto& THEME : themeChain(iconThemeName())) {
            for (const auto& BASE : iconBases()) {
                paths.emplace_back(BASE + "/" + THEME);
                paths.emplace_back(BASE + "/" + THEME + "/icon-theme.cache");
            }
        }
        paths.emplace_back("/usr/share/pixmaps");
        return paths;
    }

    void pollForChanges() {
        static auto                 lastPoll = std::chrono::steady_clock::time_point{};
        static std::vector<int64_t> appStamp, iconStamp;
        static bool                 primed = false;

        const auto NOW = std::chrono::steady_clock::now();
        if (primed && NOW - lastPoll < std::chrono::seconds(2))
            return;
        lastPoll = NOW;

        const auto APPS  = stampOf(appDirs());
        const auto ICONS = stampOf(iconWatchPaths());

        // The first call only records: the initial scan and warm-up happen on
        // their own schedule, and treating "never polled" as "changed" would
        // redo both at plugin load.
        if (!primed) {
            primed    = true;
            appStamp  = APPS;
            iconStamp = ICONS;
            return;
        }

        if (APPS != appStamp) {
            appStamp = APPS;
            refresh();
        }

        if (ICONS != iconStamp && !g_iconRebuildRunning.load()) {
            iconStamp = ICONS;
            // The previous worker (the warm-up, or an earlier rebuild) has
            // finished if the flag is down, so this join returns at once.
            if (g_iconIndexWorker.joinable())
                g_iconIndexWorker.join();

            g_iconRebuildRunning = true;
            g_iconIndexWorker    = std::thread([] {
                ::setpriority(PRIO_PROCESS, 0, 10);
                const auto THEME = iconThemeName();
                auto       fresh = buildIconIndex(THEME);
                {
                    std::scoped_lock alt(g_altMutex);
                    if (g_altBuilding.empty())
                        g_altIndex.clear(); // rebuilt on their next lookup
                }
                {
                    std::scoped_lock lock(g_iconIndexMutex);
                    g_iconIndex      = std::move(fresh);
                    g_iconIndexTheme = THEME;
                    g_iconIndexBuilt = true;
                }
                ++g_iconIndexGen;
                g_iconRebuildRunning = false;
            });
        }
    }

    // Is this exact name in the icon index? resolveIconPath answers the generic
    // tile for a miss, which is right for an app and wrong for a caller walking
    // a fallback list (a file's content-type icon names, most specific first).
    // Called from the RENDER path (a stack's pile), so it must never wait: under
    // try_lock, and "not known yet" while the startup scan holds the index --
    // the caller then falls back to a generic icon and asks again later
    // (review 2026-09-24: an unlocked read raced the rebuild, and a blocking
    // lock stalled the frame for the whole startup scan).
    std::optional<bool> iconExistsNow(const std::string& iconName) {
        if (iconName.empty())
            return false;
        std::unique_lock lock(g_iconIndexMutex, std::try_to_lock);
        if (!lock.owns_lock() || g_iconIndex.empty())
            return std::nullopt;
        return g_iconIndex.contains(iconName);
    }

    bool iconExists(const std::string& iconName) {
        return iconExistsNow(iconName).value_or(false);
    }

    std::string resolveIconPath(const std::string& iconName) {
        if (iconName.empty())
            return "";

        std::error_code ec;

        // Icon= may legitimately be an absolute path.
        if (iconName.starts_with('/'))
            return std::filesystem::exists(iconName, ec) ? iconName : "";

        warmIconIndex();

        if (const auto IT = g_iconIndex.find(iconName); IT != g_iconIndex.end())
            return IT->second.path;

        // Named icons from another desktop's theme (breeze's
        // utilities-system-monitor) simply are not present here. A generic tile
        // is the honest answer, and it is what GNOME shows too.
        if (iconName != GENERIC_ICON) {
            if (const auto IT = g_iconIndex.find(GENERIC_ICON); IT != g_iconIndex.end())
                return IT->second.path;
        }

        return "";
    }

    // The argv a launch would run: field codes stripped, and Terminal= entries
    // wrapped in the terminal exactly once. Empty when there is nothing to run.
    // Both launch paths build their command HERE so a targeted launch can never
    // disagree with a plain one about what the entry means.
    static std::vector<std::string> launchArgv(const SEntry& entry, const std::string& terminalCmd) {
        auto args = splitArgs(stripFieldCodes(entry.exec));
        if (args.empty())
            return {};

        if (entry.terminal) {
            const char* ENVTERM = std::getenv("TERMINAL");
            const auto  TERM    = !terminalCmd.empty() ? terminalCmd : (ENVTERM && *ENVTERM ? std::string{ENVTERM} : std::string{});
            if (TERM.empty())
                return {};

            auto termArgs = splitArgs(TERM);
            if (termArgs.empty())
                return {};

            termArgs.emplace_back("-e");
            termArgs.insert(termArgs.end(), args.begin(), args.end());
            args = std::move(termArgs);
        }

        return args;
    }

    // One argv word as /bin/sh will read it back. Single quotes, because inside
    // them the shell expands nothing at all -- the only escape needed is for a
    // single quote itself, which closes, escapes and reopens.
    static std::string shellQuote(const std::string& word) {
        std::string out = "'";
        for (const char C : word) {
            if (C == '\'')
                out += "'\\''";
            else
                out += C;
        }
        return out + "'";
    }

    bool launch(const SEntry& entry, const std::string& terminalCmd) {
        auto args = launchArgv(entry, terminalCmd);
        if (args.empty())
            return false;

        auto binary = args.front();
        args.erase(args.begin());
        ScrollOverview::Launch::wrap(binary, args); // its own scope, not Hyprland's (Launch.hpp)

        CProcess process(binary, args);
        return process.runAsync();
    }

    bool launchWithRule(const SEntry& entry, const std::string& terminalCmd, const std::string& execRule) {
        if (execRule.empty())
            return launch(entry, terminalCmd);

        const auto ARGS = launchArgv(entry, terminalCmd);
        if (ARGS.empty() || !::Config::Supplementary::executor())
            return false;

        // The rule rides the environment, which uwsm-app keeps (Launch.hpp).
        std::string cmd = "[" + execRule + "] " + ScrollOverview::Launch::prefix();
        for (const auto& WORD : ARGS)
            cmd += " " + shellQuote(WORD);

        return ::Config::Supplementary::executor()->spawn(cmd).has_value();
    }

    bool strictMatch(const std::string& query, const std::string& text) {
        const auto HAY     = fold(text);
        const auto NEEDLES = fold(query);

        size_t start = 0;
        while (start < NEEDLES.size()) {
            const auto BEGIN = NEEDLES.find_first_not_of(' ', start);
            if (BEGIN == std::string::npos)
                break;
            const auto END = NEEDLES.find(' ', BEGIN);
            if (!HAY.contains(NEEDLES.substr(BEGIN, END == std::string::npos ? std::string::npos : END - BEGIN)))
                return false;
            if (END == std::string::npos)
                break;
            start = END + 1;
        }

        // Every term hit, or the query was nothing but spaces -- which matches
        // everything on the host too, since its split leaves one empty pattern.
        return true;
    }

    // Index of the entry a window class belongs to, or npos. Cached as an INDEX
    // rather than a pointer: refresh() rewrites the inventory in place, so a
    // pointer cached across it would dangle while the vector's own address does
    // not change and would not trip the guard below.
    static size_t entryIndexForClass(const std::string& wmClass) {
        if (wmClass.empty())
            return std::string::npos;

        // Called from the overview's per-window membership gate, so it runs for
        // every window every frame. The inventory is small but the compare is
        // not free; the answer only changes when the inventory is rescanned.
        // Keyed on the GENERATION, not the vector's address: a rescan rewrites
        // the vector in place, so the address never changed and a class looked
        // up before its app was installed stayed a miss forever.
        static std::unordered_map<std::string, size_t> cache;
        static uint64_t                                cachedGen = 0;

        const auto& INV = inventory();
        if (cachedGen != g_inventoryGen || (cache.size() > INV.size() + 64)) {
            cache.clear();
            cachedGen = g_inventoryGen;
        }

        if (const auto IT = cache.find(wmClass); IT != cache.end())
            return IT->second;

        // A synthesized quickshell identity resolves on the TITLE half: the
        // class half is shared by every panel and matches nothing on its own.
        static constexpr std::string_view QS_PREFIX = "org.quickshell:";
        const auto WANT  = lower(wmClass.starts_with(QS_PREFIX) ? wmClass.substr(QS_PREFIX.size()) : wmClass);
        size_t     found = std::string::npos;

        for (size_t i = 0; i < INV.size(); ++i) {
            const auto& ENTRY = INV[i];
            // StartupWMClass is authoritative but measured absent on 185/186
            // entries, so the desktop id carries the load: on Wayland the app id
            // usually IS the id ("org.gnome.Nautilus"), and the trailing
            // component covers the rest ("kitty", "firefox").
            const auto ID   = lower(ENTRY.id);
            const auto TAIL = ID.substr(ID.find_last_of('.') == std::string::npos ? 0 : ID.find_last_of('.') + 1);
            if (lower(ENTRY.wmClass) == WANT || ID == WANT || TAIL == WANT || lower(ENTRY.name) == WANT ||
                chromeAppClassMatches(WANT, ENTRY.exec)) {
                found = i;
                break;
            }
        }

        cache[wmClass] = found;
        return found;
    }

    std::string identityClass(const std::string& wmClass, const std::string& title) {
        if (wmClass != "org.quickshell" || title.empty())
            return wmClass;

        return "org.quickshell:" + title;
    }

    std::string appNameForClass(const std::string& wmClass) {
        const auto IDX = entryIndexForClass(wmClass);
        return IDX == std::string::npos ? "" : inventory()[IDX].name;
    }

    std::string iconForClass(const std::string& wmClass) {
        const auto IDX = entryIndexForClass(wmClass);
        return IDX == std::string::npos ? "" : inventory()[IDX].icon;
    }

    std::string idForClass(const std::string& wmClass) {
        const auto IDX = entryIndexForClass(wmClass);
        return IDX == std::string::npos ? "" : inventory()[IDX].id;
    }

    // X32. Favourites are stored as desktop ids and a pinned app is usually NOT
    // running, so the dock cannot reach its icon or its Exec through a window
    // class the way every other lookup here does.
    const SEntry* entryForId(const std::string& id) {
        if (id.empty())
            return nullptr;

        // Indexed, like entryIndexForClass beside it. The dock calls this once
        // per favourite while building its layout, and the layout is built more
        // than once a frame -- as a linear scan over the whole inventory that
        // was thousands of string compares per frame for an answer that only
        // changes when the inventory is rebuilt.
        static std::unordered_map<std::string, size_t> cache;
        static uint64_t                                cachedGen = 0;

        const auto& INV = inventory();
        if (cachedGen != g_inventoryGen) {
            cache.clear();
            cachedGen = g_inventoryGen;
        }

        if (const auto IT = cache.find(id); IT != cache.end())
            return IT->second == std::string::npos ? nullptr : &INV[IT->second];

        size_t found = std::string::npos;
        for (size_t i = 0; i < INV.size(); ++i) {
            if (INV[i].id == id) {
                found = i;
                break;
            }
        }

        cache[id] = found;
        return found == std::string::npos ? nullptr : &INV[found];
    }

    std::vector<SAction> actionsFromFile(const std::string& path);

    std::vector<SAction> actionsForClass(const std::string& wmClass) {
        const auto IDX = entryIndexForClass(wmClass);
        if (IDX == std::string::npos)
            return {};
        return actionsFromFile(inventory()[IDX].path);
    }

    std::vector<SAction> actionsForId(const std::string& id) {
        const auto* ENTRY = entryForId(id);
        return ENTRY ? actionsFromFile(ENTRY->path) : std::vector<SAction>{};
    }

    std::vector<SAction> actionsFromFile(const std::string& path) {
        if (path.empty())
            return {};

        std::ifstream file(path);
        if (!file.is_open())
            return {};

        // `Actions=` is the declared ORDER and the whitelist; a group with no
        // matching id is not shown, per the desktop entry spec.
        std::vector<std::string>                     order;
        std::unordered_map<std::string, SAction>     found;
        std::string                                  group, line;

        while (std::getline(file, line)) {
            const auto L = trim(line);
            if (L.empty() || L.starts_with('#'))
                continue;

            if (L.starts_with('[')) {
                const auto CLOSE = L.find(']');
                const auto NAME  = CLOSE == std::string::npos ? L.substr(1) : L.substr(1, CLOSE - 1);
                group            = NAME.starts_with("Desktop Action ") ? trim(NAME.substr(15)) : "";
                continue;
            }

            const auto EQ = L.find('=');
            if (EQ == std::string::npos)
                continue;

            const auto KEY = trim(L.substr(0, EQ));
            const auto VAL = trim(L.substr(EQ + 1));

            if (group.empty()) {
                if (KEY != "Actions")
                    continue;

                size_t start = 0;
                while (start < VAL.size()) {
                    const auto END = VAL.find(';', start);
                    const auto ID  = trim(VAL.substr(start, END == std::string::npos ? std::string::npos : END - start));
                    if (!ID.empty())
                        order.emplace_back(ID);
                    if (END == std::string::npos)
                        break;
                    start = END + 1;
                }
                continue;
            }

            if (KEY == "Name")
                found[group].name = VAL;
            else if (KEY == "Exec")
                found[group].exec = VAL;
        }

        std::vector<SAction> out;
        for (const auto& ID : order) {
            const auto IT = found.find(ID);
            if (IT != found.end() && !IT->second.name.empty() && !IT->second.exec.empty()) {
                out.emplace_back(IT->second);
                out.back().id = ID;
            }
        }

        return out;
    }

    bool launchExec(const std::string& exec) {
        auto args = splitArgs(stripFieldCodes(exec));
        if (args.empty())
            return false;

        auto binary = args.front();
        args.erase(args.begin());
        ScrollOverview::Launch::wrap(binary, args); // its own scope, not Hyprland's (Launch.hpp)

        CProcess process(binary, args);
        return process.runAsync();
    }
}

// ponytail: self-check lives in the TU so it can reach the anonymous namespace.
// Build with: g++ -std=c++2b -DAPPSEARCH_TEST AppSearch.cpp -o /tmp/appsearch_test -lhyprutils
#ifdef APPSEARCH_TEST
    #include <cassert>
    #include <cstdio>

int main() {
    using namespace AppSearch;

    assert(stripFieldCodes("firefox %u") == "firefox ");
    assert(stripFieldCodes("foo %%f") == "foo %f");
    assert(stripFieldCodes("gimp-2.10 %U") == "gimp-2.10 ");

    auto a = splitArgs("env LANG=C \"my app\" -x");
    assert(a.size() == 4);
    assert(a[2] == "my app");
    assert(splitArgs("   ").empty());

    // A quoted empty argument must survive, or Exec="" collapses into nothing.
    auto q = splitArgs("sh -c \"\"");
    assert(q.size() == 3 && q[2].empty());

    SEntry e;
    e.name     = "Files";
    e.id       = "org.gnome.Nautilus";
    e.haystack = lower("Files File Manager folders org.gnome.Nautilus");
    SEntry f;
    f.name     = "Firmware Updater";
    f.id       = "org.fwupd.fwupdmgr";
    f.haystack = lower("Firmware Updater org.fwupd.fwupdmgr");

    // X18: fuzzy, not substring. "fls" is not a substring of "Files" and must
    // still match -- this is the assertion the old scorer could not pass.
    assert(score(e, "fls") > 0);
    assert(score(e, "files") > score(e, "fls")); // contiguous beats scattered
    assert(score(e, "fi") > score(f, "fi"));     // shorter name wins at equal quality
    assert(score(e, "zzzz") == -1);
    assert(score(e, "filesfilesfiles") == -1);   // needle longer than name
    assert(score(e, "nautilus") > 0);            // id match, via the fallback band
    assert(score(e, "fls") > score(e, "nautilus")); // any fuzzy hit outranks the band

    SEntry w;
    w.name     = "Text Editor";
    w.haystack = lower("Text Editor");
    assert(score(w, "editor") > 0);
    assert(score(w, "txed") > 0); // initials-ish, scattered across both words

    // Keywords are matched too, at 0.9, and split on ';' like the desktop spec.
    SEntry k;
    k.name     = "Console";
    k.keywords = "terminal;shell;";
    k.haystack = lower("Console terminal shell");
    assert(score(k, "terminal") > 0);
    // Upstream's equality fast path is case SENSITIVE, so an exactly-typed
    // keyword (1.0 x 0.9) genuinely outranks a name that only differs in case
    // (~0.77). Real host behaviour, not a defect in the port -- assert the
    // direction that is actually invariant instead.
    assert(score(k, "Console") > score(k, "terminal"));

    // The ranker must not hang or trip UB on degenerate input.
    assert(score(e, "") == -1);
    SEntry punct;
    punct.name     = "-- --";
    punct.haystack = lower("-- --");
    assert(score(punct, "--") > 0);

    const auto& INV = inventory();
    std::printf("inventory: %zu visible apps\n", INV.size());
    assert(!INV.empty());

    std::printf("icon theme: %s\n", iconThemeName().c_str());
    assert(iconCandidateScore("/usr/share/icons/hicolor/scalable/apps/kitty.svg") > iconCandidateScore("/usr/share/icons/hicolor/256x256/apps/kitty.png"));
    assert(iconCandidateScore("/usr/share/icons/hicolor/256x256/apps/kitty.png") > iconCandidateScore("/usr/share/icons/hicolor/48x48/apps/foot.png"));
    assert(iconCandidateScore("/usr/share/icons/hicolor/48x48/apps/x.txt") == -1);
    assert(resolveIconPath("").empty());
    // An unknown name resolves to the generic tile, not to nothing.
    const auto GENERIC = resolveIconPath(GENERIC_ICON);
    assert(!GENERIC.empty());
    assert(resolveIconPath("definitely-not-an-icon-zzz") == GENERIC);
    // The chain must reach the ancestor that actually holds the legacy names.
    const auto CHAIN = themeChain(iconThemeName());
    assert(std::ranges::contains(CHAIN, std::string{"hicolor"}));
    std::printf("theme chain:");
    for (const auto& T : CHAIN)
        std::printf(" %s", T.c_str());
    std::printf("\n");

    size_t withIcons = 0;
    for (const auto& E : inventory()) {
        if (!resolveIconPath(E.icon).empty())
            ++withIcons;
    }
    std::printf("icons resolved: %zu / %zu entries\n", withIcons, inventory().size());
    assert(withIcons > 0);

    // X18 open-window half. The host's setting is search-method=0 (Strict), so
    // parity is multi-term-in-any-order substring, not the fuzzy app scorer.
    const std::string WIN = "2: nvim ~/notes.md kitty";
    assert(strictMatch("kitty", WIN));
    assert(strictMatch("KITTY", WIN));
    assert(strictMatch("kitty notes", WIN));  // order-independent
    assert(strictMatch("notes kitty", WIN));  // ... both ways
    assert(!strictMatch("kitty firefox", WIN)); // every term must hit
    assert(strictMatch("2:", WIN));           // the workspace number is searchable
    assert(strictMatch("", WIN));
    assert(strictMatch("   ", WIN));
    assert(!strictMatch("fls", WIN));         // strict, NOT fuzzy -- unlike apps

    // Accents are ignored, in the query and in the title alike.
    assert(strictMatch("cafe", "1: Le Café kitty"));
    assert(strictMatch("café", "1: Le Cafe kitty"));
    assert(strictMatch("zlutoucky", "1: Žluťoučký kůň"));       // Latin Ext-A
    assert(!strictMatch("zlutoucky", "1: Zlutouck kitty"));      // no false hit
    // No canonical decomposition: folds to nothing, stays matchable as itself.
    assert(strictMatch("ø", "1: Ørsted"));
    // Non-Latin passes through untouched rather than collapsing to one blob.
    assert(strictMatch("東京", "1: 東京 kitty"));
    assert(!strictMatch("京東", "1: 東京 kitty"));

    assert(appNameForClass("").empty());
    assert(appNameForClass("definitely-not-an-app-zzz").empty());
    if (const auto& I = inventory(); !I.empty()) {
        // Whatever the id's trailing component is, it must find its own entry:
        // that is the lookup Wayland app ids actually exercise.
        const auto& FIRST = I.front();
        const auto  ID    = FIRST.id;
        const auto  TAIL  = ID.substr(ID.find_last_of('.') == std::string::npos ? 0 : ID.find_last_of('.') + 1);
        std::printf("appNameForClass(\"%s\") = \"%s\"\n", TAIL.c_str(), appNameForClass(TAIL).c_str());
        assert(!appNameForClass(TAIL).empty());
        assert(appNameForClass(TAIL) == appNameForClass(TAIL)); // cache agrees with itself
    }

    // X28. The menu's Actions section. Nautilus is the entry on this host that
    // actually ships an Actions= list, so it is the only honest fixture; assert
    // the shape rather than a literal list, which upstream is free to reorder.
    // Quickshell's shared app_id: identity is the title, and only for that
    // class. Everything else must come back byte-identical or the whole
    // identity path would start lying about ordinary windows.
    assert(identityClass("org.quickshell", "Omamail") == "org.quickshell:Omamail");
    assert(identityClass("org.quickshell", "") == "org.quickshell");
    assert(identityClass("kitty", "Omamail") == "kitty");
    if (!idForClass("org.quickshell:Omamail").empty())
        std::printf("quickshell identity: org.quickshell:Omamail -> \"%s\"\n", idForClass("org.quickshell:Omamail").c_str());

    assert(idForClass("definitely-not-an-app-zzz").empty());
    assert(actionsForClass("definitely-not-an-app-zzz").empty());
    if (const auto NAUT = idForClass("org.gnome.Nautilus"); !NAUT.empty()) {
        const auto ACTS = actionsForClass("org.gnome.Nautilus");
        std::printf("nautilus actions: %zu\n", ACTS.size());
        assert(!ACTS.empty());
        for (const auto& A : ACTS) {
            // A half-parsed action is worse than none: it renders a blank row
            // that does nothing when clicked.
            assert(!A.name.empty() && !A.exec.empty());
            std::printf("  %s -> %s\n", A.name.c_str(), A.exec.c_str());
        }
    }

    const auto R = search("te", 5);
    std::printf("search(\"te\"): %zu results, top = %s\n", R.size(), R.empty() ? "(none)" : R.front()->name.c_str());
    assert(search("", 5).empty());

    std::printf("AppSearch self-check OK\n");
    return 0;
}
#endif
