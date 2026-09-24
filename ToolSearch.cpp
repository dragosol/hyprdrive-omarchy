#include "ToolSearch.hpp"

#include "Config.hpp"
#include "ToolSearchParse.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <list>
#include <mutex>
#include <optional>
#include <poll.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>

extern char** environ;

namespace ToolSearch {

    using detail::lower;
    using detail::trim;

    bool enabled(EKind kind) {
        switch (kind) {
            case EKind::Define: return ScrollOverview::Config::getSearchDictionary();
            case EKind::Translate: return ScrollOverview::Config::getSearchTranslate();
            case EKind::None: return false;
        }
        return false;
    }

    namespace {

        // ------------------------------------------------------------- worker --

        // THE LAST LANGUAGE ASKED FOR (user, 2026-09-24: "for translations that
        // would be in the last used language"). Every `tr <lang> ...` records its
        // target; a query that names none goes there. Kept in ~/.cache so it
        // survives a restart.
        std::string lastTargetPath() {
            const char* HOME = std::getenv("HOME");
            return HOME ? std::string(HOME) + "/.cache/hyprdrive/translate-last" : "";
        }

        std::string& lastTargetMemo() {
            static std::string memo = [] {
                std::string v;
                if (const auto P = lastTargetPath(); !P.empty()) {
                    if (FILE* f = std::fopen(P.c_str(), "r")) {
                        char buf[32] = {};
                        if (std::fgets(buf, sizeof(buf), f))
                            v = languageCode(buf);
                        std::fclose(f);
                    }
                }
                return v;
            }();
            return memo;
        }

        void rememberTarget(const std::string& code) {
            if (code.empty() || lastTargetMemo() == code)
                return;
            lastTargetMemo() = code;
            if (const auto P = lastTargetPath(); !P.empty()) {
                std::string dir = P.substr(0, P.rfind('/'));
                mkdir(dir.substr(0, dir.rfind('/')).c_str(), 0755); // ~/.cache
                mkdir(dir.c_str(), 0755);
                if (FILE* f = std::fopen(P.c_str(), "w")) {
                    std::fputs(code.c_str(), f);
                    std::fclose(f);
                }
            }
        }

        // The target a query with none named goes to: the last one used, else
        // the config, else the language of $LANG ("ro_RO.UTF-8" -> "ro"), else
        // English.
        std::string defaultTarget() {
            if (const auto& LAST = lastTargetMemo(); !LAST.empty())
                return LAST;
            if (const auto CFG = languageCode(ScrollOverview::Config::getSearchTranslateTarget()); !CFG.empty())
                return CFG;
            if (const char* LANG = std::getenv("LANG"); LANG && std::strlen(LANG) >= 2) {
                if (const auto CODE = languageCode(std::string(LANG, 2)); !CODE.empty())
                    return CODE;
            }
            return "en";
        }

        // A query with its defaults filled in: two spellings of the same
        // request must hit the same cache entry.
        struct SResolved {
            SQuery      query;
            std::string endpoint; // "" = MyMemory
            std::string key;
        };

        SResolved resolve(const SQuery& q) {
            SResolved r;
            r.query = q;
            if (q.kind == EKind::Translate) {
                if (r.query.to.empty())
                    r.query.to = defaultTarget();
                else if (!q.term.empty())
                    rememberTarget(q.to);
                r.endpoint = trim(ScrollOverview::Config::getSearchTranslateEndpoint());
                while (!r.endpoint.empty() && r.endpoint.back() == '/')
                    r.endpoint.pop_back();
            }
            r.key = std::to_string(sc<int>(q.kind)) + '\x1f' + r.query.from + '\x1f' + r.query.to + '\x1f' + r.endpoint + '\x1f' + r.query.term;
            return r;
        }

        std::mutex                           g_mutex;
        std::condition_variable              g_cv;
        std::optional<SResolved>             g_want;
        std::unordered_map<std::string, SResult> g_cache;
        std::list<std::string>               g_cacheOrder; // oldest first
        std::atomic<uint64_t>                g_generation{0};
        std::function<void()>                g_ready;
        std::thread                          g_thread;
        bool                                 g_stop = false;
        // The curl in flight, so shutdown can end it: an unload joins the
        // worker, and a join that waited out a slow request would hold the
        // compositor for up to RUN_MAX_MS.
        std::atomic<pid_t>                   g_child{-1};

        constexpr size_t CACHE_MAX   = 96;
        constexpr int    DEBOUNCE_MS = 280; // typing a word is one request, not one per letter
        constexpr int    RUN_MAX_MS  = 15000;

        // Run `sh -c SCRIPT tool ARGS...`, capture stdout, return the exit code
        // (-1 on a failure to run or a timeout). The script is a constant; the
        // user's text only ever arrives as "$1".."$n", which sh does not
        // re-parse -- there is nothing to escape because nothing is spliced.
        int runScript(const char* script, const std::vector<std::string>& args, std::string& out) {
            int fds[2];
            if (pipe2(fds, O_CLOEXEC) != 0)
                return -1;

            posix_spawn_file_actions_t actions;
            posix_spawn_file_actions_init(&actions);
            posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
            posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
            posix_spawn_file_actions_addclose(&actions, fds[0]);

            std::vector<std::string> argvS = {"sh", "-c", script, "hyprdrive-tool"};
            argvS.insert(argvS.end(), args.begin(), args.end());
            std::vector<char*> argv;
            for (auto& a : argvS)
                argv.push_back(a.data());
            argv.push_back(nullptr);

            pid_t      pid = -1;
            const int  RC  = posix_spawnp(&pid, "sh", &actions, nullptr, argv.data(), environ);
            posix_spawn_file_actions_destroy(&actions);
            close(fds[1]);
            if (RC != 0) {
                close(fds[0]);
                return -1;
            }
            g_child.store(pid);

            const auto DEADLINE = std::chrono::steady_clock::now() + std::chrono::milliseconds(RUN_MAX_MS);
            char       buf[4096];
            bool       timedOut = false;
            while (true) {
                const auto LEFT = std::chrono::duration_cast<std::chrono::milliseconds>(DEADLINE - std::chrono::steady_clock::now()).count();
                if (LEFT <= 0) {
                    timedOut = true;
                    break;
                }
                pollfd pfd{fds[0], POLLIN, 0};
                if (poll(&pfd, 1, sc<int>(LEFT)) <= 0) {
                    timedOut = true;
                    break;
                }
                const ssize_t N = read(fds[0], buf, sizeof(buf));
                if (N <= 0)
                    break;
                out.append(buf, sc<size_t>(N));
                if (out.size() > 512 * 1024)
                    break; // a definition page is kilobytes; this is not one
            }
            close(fds[0]);

            if (timedOut)
                kill(pid, SIGKILL);
            int status = 0;
            waitpid(pid, &status, 0);
            g_child.store(-1);
            if (timedOut)
                return -1;
            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        }

        // @tsv's escapes back to text.
        std::string untsv(const std::string& s) {
            std::string o;
            o.reserve(s.size());
            for (size_t i = 0; i < s.size(); ++i) {
                if (s[i] == '\\' && i + 1 < s.size()) {
                    switch (s[++i]) {
                        case 't': o += '\t'; break;
                        case 'n': o += ' '; break; // one line per sense on screen
                        case 'r': break;
                        case '\\': o += '\\'; break;
                        default: o += s[i];
                    }
                } else
                    o += s[i];
            }
            return o;
        }

        std::vector<std::string> splitTabs(const std::string& line) {
            std::vector<std::string> cols;
            size_t                   at = 0;
            while (true) {
                const auto T = line.find('\t', at);
                cols.push_back(untsv(line.substr(at, T == std::string::npos ? std::string::npos : T - at)));
                if (T == std::string::npos)
                    break;
                at = T + 1;
            }
            return cols;
        }

        std::vector<std::string> lines(const std::string& s) {
            std::vector<std::string> out;
            size_t                   at = 0;
            while (at < s.size()) {
                const auto NL = s.find('\n', at);
                auto       l  = s.substr(at, NL == std::string::npos ? std::string::npos : NL - at);
                if (!l.empty())
                    out.push_back(std::move(l));
                if (NL == std::string::npos)
                    break;
                at = NL + 1;
            }
            return out;
        }

        std::string urlPathEncode(const std::string& s) {
            std::string o;
            for (unsigned char c : s) {
                if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
                    o += sc<char>(c);
                else {
                    char b[4];
                    std::snprintf(b, sizeof(b), "%%%02X", c);
                    o += b;
                }
            }
            return o;
        }

        // -- Wiktionary. Titles are case- and space-sensitive: try the word as
        // typed, then lowercased (a sentence-initial capital is the usual
        // reason a real word misses). 22 is curl's HTTP-error exit; anything
        // else is the network, which is reported as such rather than as "no
        // such word".
        constexpr const char* DEFINE_SCRIPT = R"SH(
for t in "$1" "$2"; do
  out=$(curl -sS -f -m 7 -H "User-Agent: $4" "https://en.wiktionary.org/api/rest_v1/page/definition/$t"); rc=$?
  if [ "$rc" -eq 0 ]; then printf '%s' "$out" | jq -r "$3"; exit 0; fi
  [ "$rc" -eq 22 ] || exit 2
done
exit 3
)SH";

        constexpr const char* DEFINE_JQ =
            R"JQ(def clean: gsub("<[^>]*>";"") | gsub("&amp;";"&") | gsub("&quot;";"\"") | gsub("&#39;";"'") | gsub("&lt;";"<") | gsub("&gt;";">") | gsub("&nbsp;";" ") | gsub("\\s+";" ") | sub("^ ";"") | sub(" $";"");
(.en // [])[] | .partOfSpeech as $p | (.definitions // [])[] | select((.definition|clean) != "") | [$p, (.definition|clean), (((.examples // [])[0] // "")|clean)] | @tsv)JQ";

        // -- MyMemory: status, detected language, translation, then up to four
        // distinct alternatives from its translation memory -- only the ones it
        // scores as near-exact (>= 0.95). Below that its "matches" include fuzzy
        // hits on OTHER sentences, some not even in the target language (seen:
        // "The book is on the table" offered for a French translation).
        constexpr const char* MYMEMORY_SCRIPT = R"SH(
out=$(curl -sS -f -m 8 -G "https://api.mymemory.translated.net/get" --data-urlencode "q=$1" --data-urlencode "langpair=$2|$3") || exit 2
printf '%s' "$out" | jq -r '([(.responseStatus|tostring), (.responseData.detectedLanguage // ""), (.responseData.translatedText // "")] | @tsv), ([.matches[]? | select(((.match // 0) | tonumber? // 0) >= 0.95) | .translation] | map(select(. != null and . != "")) | unique | .[:4][] | ["alt", .] | @tsv)'
)SH";

        // -- LibreTranslate-compatible (self-hosted). "auto" is its detect.
        constexpr const char* LIBRE_SCRIPT = R"SH(
body=$(jq -nc --arg q "$1" --arg s "$2" --arg t "$3" '{q:$q,source:$s,target:$t,format:"text"}')
out=$(curl -sS -f -m 10 -X POST "$4/translate" -H 'Content-Type: application/json' -d "$body") || exit 2
printf '%s' "$out" | jq -r '["200", (.detectedLanguage.language // ""), (.translatedText // "")] | @tsv'
)SH";

        constexpr const char* USER_AGENT = "hyprdrive-omarchy (https://github.com/dragosol/hyprdrive-omarchy)";

        SResult fetch(const SResolved& r) {
            SResult res;
            const auto& Q = r.query;

            if (Q.kind == EKind::Define) {
                res.title    = Q.term;
                res.subtitle = "Wiktionary";

                std::string word = Q.term;
                std::ranges::replace(word, ' ', '_');
                std::string out;
                const int   RC = runScript(DEFINE_SCRIPT, {urlPathEncode(word), urlPathEncode(lower(word)), DEFINE_JQ, USER_AGENT}, out);
                if (RC == 2 || RC < 0) {
                    res.state = SResult::EState::Error;
                    return res;
                }
                for (const auto& L : lines(out)) {
                    auto c = splitTabs(L);
                    c.resize(3);
                    if (!c[1].empty())
                        res.senses.push_back(SSense{.tag = c[0], .text = c[1], .example = c[2]});
                }
                res.state = res.senses.empty() ? SResult::EState::Empty : SResult::EState::Ready;
                return res;
            }

            // Translate.
            const bool  LIBRE = !r.endpoint.empty();
            const auto  FROM  = Q.from.empty() ? (LIBRE ? "auto" : "autodetect") : Q.from;
            std::string out;
            const int   RC = LIBRE ? runScript(LIBRE_SCRIPT, {Q.term, FROM, Q.to, r.endpoint}, out) : runScript(MYMEMORY_SCRIPT, {Q.term, FROM, Q.to}, out);

            res.title = Q.term;
            if (RC != 0) {
                res.state    = SResult::EState::Error;
                res.subtitle = std::string("→ ") + Q.to + (LIBRE ? "  ·  " + r.endpoint : "  ·  MyMemory");
                return res;
            }

            std::string detected;
            for (const auto& L : lines(out)) {
                auto c = splitTabs(L);
                if (c.size() >= 2 && c[0] == "alt") {
                    if (!res.senses.empty() && c[1] != res.senses.front().text)
                        res.senses.push_back(SSense{.tag = "also", .text = c[1]});
                    continue;
                }
                c.resize(3);
                if (c[0] != "200") {
                    // MyMemory says why in the text field (quota, bad pair).
                    res.state    = SResult::EState::Error;
                    res.message  = c[2].empty() ? "The translation service refused the request" : c[2];
                    res.subtitle = std::string("→ ") + Q.to + (LIBRE ? "  ·  " + r.endpoint : "  ·  MyMemory");
                    return res;
                }
                detected = c[1];
                if (!c[2].empty())
                    res.senses.insert(res.senses.begin(), SSense{.tag = "", .text = c[2]});
            }

            const auto SRC = Q.from.empty() ? (detected.empty() ? std::string("auto") : detected) : Q.from;
            res.subtitle   = SRC + " → " + Q.to + (LIBRE ? "  ·  " + r.endpoint : "  ·  MyMemory");
            res.state      = res.senses.empty() ? SResult::EState::Empty : SResult::EState::Ready;
            if (res.senses.size() > 5)
                res.senses.resize(5);
            return res;
        }

        void workerMain() {
            std::unique_lock lock(g_mutex);
            while (!g_stop) {
                g_cv.wait(lock, [] { return g_stop || g_want.has_value(); });
                if (g_stop)
                    break;

                // Debounce: keep taking the newest request until the field has
                // been still for a beat.
                auto want = *g_want;
                while (!g_stop) {
                    g_cv.wait_for(lock, std::chrono::milliseconds(DEBOUNCE_MS));
                    if (!g_want || g_want->key == want.key)
                        break;
                    want = *g_want;
                }
                g_want.reset();
                if (g_stop)
                    break;
                if (g_cache.contains(want.key))
                    continue;

                lock.unlock();
                SResult res = fetch(want);
                lock.lock();

                // Errors are not cached: the network coming back should not
                // need the query retyped.
                if (res.state != SResult::EState::Error) {
                    g_cache[want.key] = res;
                    g_cacheOrder.push_back(want.key);
                    while (g_cacheOrder.size() > CACHE_MAX) {
                        g_cache.erase(g_cacheOrder.front());
                        g_cacheOrder.pop_front();
                    }
                } else
                    g_cache["!" + want.key] = res; // shown until the next attempt

                g_generation.fetch_add(1, std::memory_order_release);
                if (g_ready) {
                    auto cb = g_ready;
                    lock.unlock();
                    cb();
                    lock.lock();
                }
            }
        }

    } // namespace

    SResult lookup(const SQuery& query) {
        SResult pending;
        pending.title = query.term;
        if (query.kind == EKind::None || trim(query.term).empty())
            return pending;

        const auto R = resolve(query);

        std::lock_guard lock(g_mutex);
        if (const auto IT = g_cache.find(R.key); IT != g_cache.end())
            return IT->second;

        if (!g_thread.joinable()) {
            g_stop   = false;
            g_thread = std::thread(workerMain);
        }

        // A failed attempt for THIS query stays on screen while it is retried.
        if (const auto ERR = g_cache.find("!" + R.key); ERR != g_cache.end()) {
            if (!g_want || g_want->key != R.key) {
                g_want = R;
                g_cv.notify_all();
            }
            return ERR->second;
        }

        if (!g_want || g_want->key != R.key) {
            g_want = R;
            g_cv.notify_all();
        }

        pending.subtitle = R.query.kind == EKind::Define ? "Wiktionary" : ((R.query.from.empty() ? "auto" : R.query.from) + " → " + R.query.to);
        return pending;
    }

    uint64_t generation() {
        return g_generation.load(std::memory_order_acquire);
    }

    std::string defaultTargetLanguage() {
        return defaultTarget();
    }

    void setReadyCallback(std::function<void()> cb) {
        std::lock_guard lock(g_mutex);
        g_ready = std::move(cb);
    }

    void shutdown() {
        {
            std::lock_guard lock(g_mutex);
            g_stop  = true;
            g_ready = nullptr;
        }
        if (const pid_t CHILD = g_child.load(); CHILD > 0)
            kill(CHILD, SIGKILL); // its pipe closes, the read returns, the worker sees g_stop
        g_cv.notify_all();
        if (g_thread.joinable())
            g_thread.join();
        std::lock_guard lock(g_mutex);
        g_cache.clear();
        g_cacheOrder.clear();
        g_want.reset();
    }

} // namespace ToolSearch
