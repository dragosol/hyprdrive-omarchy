#include "SearchProviders.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>

#include <systemd/sd-bus.h>

namespace SearchProviders {
    namespace {
        constexpr const char* IFACE          = "org.gnome.Shell.SearchProvider2";
        // X66. 400ms was far too short and it is why categories came and went.
        // A provider that is not already running has to be D-BUS ACTIVATED
        // first -- that spawns a whole GTK application -- and none of
        // Characters, Clocks, Contacts or Settings can do that and answer
        // inside 400ms. They timed out, were dropped as stale, and the user saw
        // the apps grid and nothing else; the only categories that ever showed
        // were the ones something else had happened to warm up, which is why
        // the set changed between two runs of the same query.
        //
        // This does NOT weaken X20's "a slow provider may never stall a frame":
        // every call here is sd_bus_call_async, so a longer deadline costs the
        // frame nothing. It only decides how long a late answer is still
        // wanted. Warming the providers at overview open, so this deadline is
        // never the thing being waited on, is X51.
        constexpr uint64_t    CALL_TIMEOUT_US = 8'000'000;
        // GNOME shows a handful per provider; asking for metas on 200 character
        // glyphs would cost more than the panel can ever show.
        constexpr size_t MAX_PER_PROVIDER = 6;

        // X54 trace. Where the wall clock between a keystroke and a filled row
        // actually goes: this protocol is two round trips per provider, and a
        // static read of the code cannot say which of the two is the slow one.
        using SPClock = std::chrono::steady_clock;
        SPClock::time_point w_genStart;
        bool spTrace() {
            static const bool ON = std::getenv("HYPRDRIVE_PROFILE") != nullptr || std::getenv("SCROLLOVERVIEW_PROFILE") != nullptr;
            return ON;
        }
        double sinceGen() {
            return std::chrono::duration<double, std::milli>(SPClock::now() - w_genStart).count();
        }

        std::string trim(const std::string& s) {
            const auto B = s.find_first_not_of(" \t\r\n");
            if (B == std::string::npos)
                return "";
            return s.substr(B, s.find_last_not_of(" \t\r\n") - B + 1);
        }

        std::vector<std::string> dataDirs() {
            std::vector<std::string> dirs;
            if (const char* HOME_DATA = std::getenv("XDG_DATA_HOME"); HOME_DATA && *HOME_DATA)
                dirs.emplace_back(HOME_DATA);
            else if (const char* HOME = std::getenv("HOME"); HOME)
                dirs.emplace_back(std::string{HOME} + "/.local/share");

            const char* SYS = std::getenv("XDG_DATA_DIRS");
            std::string sys = SYS && *SYS ? SYS : "/usr/local/share:/usr/share";
            for (size_t pos = 0; pos <= sys.size();) {
                const auto SEP  = sys.find(':', pos);
                const auto PART = sys.substr(pos, SEP == std::string::npos ? std::string::npos : SEP - pos);
                if (!PART.empty())
                    dirs.push_back(PART);
                if (SEP == std::string::npos)
                    break;
                pos = SEP + 1;
            }
            return dirs;
        }

        std::vector<std::string> splitTerms(const std::string& text) {
            std::vector<std::string> terms;
            size_t                   i = 0;
            while (i < text.size()) {
                while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])))
                    ++i;
                const size_t START = i;
                while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i])))
                    ++i;
                if (i > START)
                    terms.push_back(text.substr(START, i - START));
            }
            return terms;
        }

        std::vector<char*> cstrv(std::vector<std::string>& v) {
            std::vector<char*> out;
            out.reserve(v.size() + 1);
            for (auto& S : v)
                out.push_back(S.data());
            out.push_back(nullptr);
            return out;
        }

        // ---- discovery ----------------------------------------------------
        // Returns the string list in org.gnome.desktop.search-providers <key>.
        // Empty on any failure, which degrades to plain alphabetical order --
        // the behaviour before X18, not a broken search.
        std::vector<std::string> searchProviderSetting(const std::string& key) {
            std::vector<std::string> out;

            const std::string CMD = "gsettings get org.gnome.desktop.search-providers " + key + " 2>/dev/null";
            FILE*             pipe = popen(CMD.c_str(), "r");
            if (!pipe)
                return out;

            std::string raw;
            char        buf[512];
            while (std::fgets(buf, sizeof(buf), pipe))
                raw += buf;
            pclose(pipe);

            // "['a.desktop', 'b.desktop']", or "@as []" when unset.
            for (size_t i = 0; i < raw.size();) {
                const auto OPEN = raw.find('\'', i);
                if (OPEN == std::string::npos)
                    break;
                const auto CLOSE = raw.find('\'', OPEN + 1);
                if (CLOSE == std::string::npos)
                    break;
                out.emplace_back(raw.substr(OPEN + 1, CLOSE - OPEN - 1));
                i = CLOSE + 1;
            }

            return out;
        }


        std::vector<SProvider> g_providers;
        bool                   g_scanned = false;

        void                   scan() {
            g_providers.clear();
            for (const auto& DIR : dataDirs()) {
                const std::filesystem::path P = DIR + "/gnome-shell/search-providers";
                std::error_code             ec;
                if (!std::filesystem::is_directory(P, ec))
                    continue;
                for (const auto& ENT : std::filesystem::directory_iterator(P, ec)) {
                    if (ENT.path().extension() != ".ini")
                        continue;
                    std::ifstream in(ENT.path());
                    SProvider     p;
                    std::string   line;
                    while (std::getline(in, line)) {
                        const auto EQ = line.find('=');
                        if (EQ == std::string::npos)
                            continue;
                        const auto KEY = trim(line.substr(0, EQ));
                        const auto VAL = trim(line.substr(EQ + 1));
                        if (KEY == "DesktopId")
                            p.desktopId = VAL.ends_with(".desktop") ? VAL.substr(0, VAL.size() - 8) : VAL;
                        else if (KEY == "BusName")
                            p.busName = VAL;
                        else if (KEY == "ObjectPath")
                            p.objectPath = VAL;
                        else if (KEY == "Version")
                            p.version = std::atoi(VAL.c_str());
                    }
                    if (p.busName.empty() || p.objectPath.empty())
                        continue;
                    // Earlier data dirs win, same rule as AppSearch's desktop ids.
                    if (std::ranges::none_of(g_providers, [&](const SProvider& E) { return E.busName == p.busName; }))
                        g_providers.push_back(std::move(p));
                }
            }
            // X18: the user's own gsettings decide order and membership.
            //
            // Read through the `gsettings` CLI rather than by linking gio: this
            // runs once per scan, not per keystroke, and dconf has no stable
            // file format to parse. The command is a fixed string with nothing
            // interpolated into it.
            const auto DISABLED = searchProviderSetting("disabled");
            const auto ORDER    = searchProviderSetting("sort-order");

            std::erase_if(g_providers, [&](const SProvider& P) { return std::ranges::contains(DISABLED, P.desktopId + ".desktop"); });

            // GNOME's rule: everything named in sort-order first, in that
            // order, then the rest. Alphabetical for the remainder so the tail
            // is at least stable.
            std::ranges::sort(g_providers, {}, &SProvider::desktopId);
            std::ranges::stable_sort(g_providers, [&](const SProvider& A, const SProvider& B) {
                const auto RANK = [&](const SProvider& P) {
                    const auto IT = std::ranges::find(ORDER, P.desktopId + ".desktop");
                    return IT == ORDER.end() ? ORDER.size() : static_cast<size_t>(IT - ORDER.begin());
                };
                return RANK(A) < RANK(B);
            });

            g_scanned = true;
        }

        // ---- shared state between render thread and worker ----------------

        std::mutex              g_mutex;
        std::vector<SResult>    g_results;

        std::string             g_wantQuery;
        uint64_t                g_wantGen = 0;
        bool                    g_stop    = false;

        std::vector<std::function<void(sd_bus*)>> g_actions;

        std::thread                               g_worker;
        std::atomic<bool>                         g_running{false};
        int                                       g_wakeFd[2] = {-1, -1};

        // NO SESSION BUS (native port, 2026-10-14). The worker is the only
        // thread in this plugin that ENDS ON ITS OWN: when sd_bus_open_user
        // fails it returns at once. It used to return leaving g_worker
        // joinable, and the next overview open assigned a new std::thread over
        // it -- assigning to a joinable std::thread is std::terminate, which
        // took the compositor down (first headless run on fedora, no bus).
        // ensureWorker() now joins a finished worker before starting another,
        // and this stamp keeps a session with no bus at all from re-trying the
        // connect on every keystroke: one attempt per g_busRetryNs (5 s).
        std::atomic<int64_t>                      g_busRetryNs{std::chrono::nanoseconds(std::chrono::seconds(5)).count()};
        std::atomic<int64_t>                      g_busFailedAt{0}; // steady_clock ns, 0 = never failed
        std::atomic<uint64_t>                     g_busFailures{0};

        int64_t                                   steadyNs() {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        void                                      wake() {
            if (g_wakeFd[1] >= 0) {
                const char B = 1;
                [[maybe_unused]] auto _ = ::write(g_wakeFd[1], &B, 1);
            }
        }

        // ---- worker-only state --------------------------------------------

        struct SCall {
            uint64_t                 gen         = 0;
            size_t                   providerIdx = 0;
            bool                     metas       = false;
            // The full id list this generation wants shown, in the provider's
            // own ranking. A metas reply only carries the ids that were MISSING
            // from the cache, so the order has to be remembered here or a
            // partly-cached answer comes back shuffled.
            std::vector<std::string> ids;
            // The subset actually asked for, same order as the reply.
            std::vector<std::string> asked;
        };

        // X54. GNOME Shell fetches metas only for ids it has never displayed
        // (SearchResultsBase._ensureResultActors); we refetched every id on
        // every keystroke. Measured on this guest: the metas round trip is
        // ~20ms warm and ~200ms cold PER PROVIDER, paid again for "dis",
        // "disp", "displa", "display" even though the id set barely moves.
        // Keyed by bus + id because ids are only unique within a provider.
        constexpr size_t                                  META_CACHE_MAX = 512;
        std::unordered_map<std::string, SResult>          g_metaCache;
        std::string metaKey(const std::string& bus, const std::string& id) {
            return bus + "\x1f" + id;
        }

        uint64_t                             w_gen = 0;
        std::vector<sd_bus_slot*>            w_slots;
        std::deque<std::unique_ptr<SCall>>   w_calls;
        std::string                          w_query;
        std::vector<std::string>             w_terms;
        // Previous generation's ids per provider, for GetSubsearchResultSet.
        std::vector<std::vector<std::string>> w_prevIds;
        std::string                          w_prevQuery;
        std::atomic<uint64_t>                w_subsearchCalls{0};

        // Worker thread. This provider has nothing for the current query, so its
        // carried-over rows come down -- otherwise a provider that stops matching
        // (or dies) leaves its last answer on screen for every query after it.
        // Returns 0 to suit the sd_bus callback signature.
        int                                  dropStaleFor(size_t providerIdx) {
            {
                std::lock_guard lock(g_mutex);
                const auto&     BUS = g_providers[providerIdx].busName;
                std::erase_if(g_results, [&](const SResult& R) { return R.stale && R.providerBus == BUS; });
            }
            return 0;
        }

        void                                 dropInFlight() {
            for (auto* S : w_slots)
                sd_bus_slot_unref(S);
            w_slots.clear();
            w_calls.clear();
        }

        // A GIcon lands here as (sv): ("themed", <as>) or ("file", <s>).
        std::string readGIcon(sd_bus_message* m) {
            std::string out;
            if (sd_bus_message_enter_container(m, 'r', "sv") <= 0)
                return out;

            const char* tag = nullptr;
            sd_bus_message_read_basic(m, 's', &tag);

            char        type     = 0;
            const char* contents = nullptr;
            sd_bus_message_peek_type(m, &type, &contents);
            const std::string SIG = contents ? contents : "";

            if (SIG == "as") {
                sd_bus_message_enter_container(m, 'v', "as");
                char** names = nullptr;
                if (sd_bus_message_read_strv(m, &names) > 0 && names) {
                    // GThemedIcon names are a preference chain; first wins.
                    for (char** p = names; *p; ++p) {
                        if (out.empty())
                            out = *p;
                        free(*p);
                    }
                    free(names);
                }
                sd_bus_message_exit_container(m);
            } else if (SIG == "s") {
                sd_bus_message_enter_container(m, 'v', "s");
                const char* val = nullptr;
                sd_bus_message_read_basic(m, 's', &val);
                if (val)
                    out = val;
                sd_bus_message_exit_container(m);
            } else
                sd_bus_message_skip(m, nullptr);

            sd_bus_message_exit_container(m);
            return out;
        }

        void assign(SResult& r, const std::string& key, const std::string& val) {
            if (key == "id")
                r.id = val;
            else if (key == "name")
                r.name = val;
            else if (key == "description")
                r.description = val;
            else if (key == "clipboardText")
                r.clipboardText = val;
            else if (key == "icon" || key == "gicon")
                r.icon = val;
        }

        // One a{sv} meta dict. Unknown keys and icon-data (iiibiiay) are skipped
        // rather than fatal -- a provider we have never seen must degrade, not
        // abort the whole reply.
        bool readMeta(sd_bus_message* m, SResult& out) {
            if (sd_bus_message_enter_container(m, 'a', "{sv}") <= 0)
                return false;

            while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
                const char* key = nullptr;
                if (sd_bus_message_read_basic(m, 's', &key) < 0 || !key) {
                    sd_bus_message_exit_container(m);
                    break;
                }
                const std::string KEY = key;

                char              type     = 0;
                const char*       contents = nullptr;
                sd_bus_message_peek_type(m, &type, &contents);
                const std::string SIG = contents ? contents : "";

                if (SIG == "s") {
                    sd_bus_message_enter_container(m, 'v', "s");
                    const char* val = nullptr;
                    sd_bus_message_read_basic(m, 's', &val);
                    sd_bus_message_exit_container(m);
                    assign(out, KEY, val ? val : "");
                } else if (SIG == "(sv)") {
                    sd_bus_message_enter_container(m, 'v', "(sv)");
                    const auto ICON = readGIcon(m);
                    sd_bus_message_exit_container(m);
                    if (KEY == "icon" || KEY == "gicon")
                        out.icon = ICON;
                } else
                    sd_bus_message_skip(m, nullptr);

                sd_bus_message_exit_container(m);
            }

            sd_bus_message_exit_container(m);
            return !out.id.empty();
        }

        // The rows for one provider, in its ranking, for whatever the cache has.
        std::vector<SResult> collect(size_t providerIdx, const std::vector<std::string>& ids) {
            const auto&          PROV = g_providers[providerIdx];
            std::vector<SResult> out;
            out.reserve(ids.size());
            for (const auto& ID : ids) {
                const auto IT = g_metaCache.find(metaKey(PROV.busName, ID));
                if (IT == g_metaCache.end())
                    continue;
                SResult r      = IT->second;
                r.providerRank = providerIdx;
                r.stale        = false;
                out.push_back(std::move(r));
            }
            return out;
        }

        // Replace one provider's rows with the given batch, newest generation only.
        void publish(uint64_t gen, const std::string& busName, std::vector<SResult>& batch) {
            std::lock_guard lock(g_mutex);
            if (gen != g_wantGen)
                return;
            std::erase_if(g_results, [&](const SResult& R) { return R.providerBus == busName; });
            g_results.insert(g_results.end(), batch.begin(), batch.end());
            std::ranges::stable_sort(g_results, {}, &SResult::providerRank);
        }

        int onMetas(sd_bus_message* m, void* userdata, sd_bus_error*) {
            auto* call = static_cast<SCall*>(userdata);
            if (call->gen != w_gen || sd_bus_message_is_method_error(m, nullptr))
                return 0;

            const auto& PROV = g_providers[call->providerIdx];

            // Keyed by the id we ASKED for, taken positionally from the reply,
            // not by the id the provider echoes back. gnome-clocks ids are
            // serialized GVariants and do not survive the round trip verbatim;
            // keying on the echo silently lost every one of its rows. The
            // requested id is also the one ActivateResult has to be given.
            if (sd_bus_message_enter_container(m, 'a', "a{sv}") > 0) {
                for (size_t i = 0; i < call->asked.size(); ++i) {
                    SResult r;
                    r.providerBus       = PROV.busName;
                    r.providerDesktopId = PROV.desktopId;
                    r.providerRank      = call->providerIdx;
                    if (!readMeta(m, r))
                        break;
                    r.id = call->asked[i];
                    if (g_metaCache.size() >= META_CACHE_MAX)
                        g_metaCache.clear();
                    g_metaCache[metaKey(PROV.busName, r.id)] = std::move(r);
                }
                sd_bus_message_exit_container(m);
            }

            auto batch = collect(call->providerIdx, call->ids);
            if (spTrace())
                std::fprintf(stderr, "[sp] %-34s META +%7.1fms n=%zu\n", PROV.desktopId.c_str(), sinceGen(), batch.size());
            if (batch.empty())
                return dropStaleFor(call->providerIdx);

            publish(call->gen, PROV.busName, batch);
            return 0;
        }

        int onResultSet(sd_bus_message* m, void* userdata, sd_bus_error*) {
            auto* call = static_cast<SCall*>(userdata);
            if (call->gen != w_gen)
                return 0;
            // No answer for this query. A CALL_TIMEOUT_US timeout lands here too.
            if (sd_bus_message_is_method_error(m, nullptr))
                return dropStaleFor(call->providerIdx);

            char** ids = nullptr;
            if (sd_bus_message_read_strv(m, &ids) <= 0 || !ids)
                return dropStaleFor(call->providerIdx);

            std::vector<std::string> kept;
            for (char** p = ids; *p; ++p) {
                if (kept.size() < MAX_PER_PROVIDER)
                    kept.emplace_back(*p);
                free(*p);
            }
            free(ids);

            w_prevIds[call->providerIdx] = kept;
            if (spTrace())
                std::fprintf(stderr, "[sp] %-34s ids  +%7.1fms n=%zu\n", g_providers[call->providerIdx].desktopId.c_str(), sinceGen(), kept.size());
            if (kept.empty())
                return dropStaleFor(call->providerIdx);

            sd_bus*     bus  = sd_bus_message_get_bus(m);
            const auto& PROV = g_providers[call->providerIdx];

            std::vector<std::string> missing;
            for (const auto& ID : kept)
                if (!g_metaCache.contains(metaKey(PROV.busName, ID)))
                    missing.push_back(ID);

            // Everything already known: the rows go up on this reply, one round
            // trip instead of two. This is the refine case, which is most of them.
            if (missing.empty()) {
                auto batch = collect(call->providerIdx, kept);
                if (spTrace())
                    std::fprintf(stderr, "[sp] %-34s HIT  +%7.1fms n=%zu\n", PROV.desktopId.c_str(), sinceGen(), batch.size());
                if (batch.empty())
                    return dropStaleFor(call->providerIdx);
                publish(call->gen, PROV.busName, batch);
                return 0;
            }

            sd_bus_message* req = nullptr;
            if (sd_bus_message_new_method_call(bus, &req, PROV.busName.c_str(), PROV.objectPath.c_str(), IFACE, "GetResultMetas") < 0)
                return 0;
            auto ARGV = cstrv(missing);
            sd_bus_message_append_strv(req, ARGV.data());

            auto next  = std::make_unique<SCall>(SCall{call->gen, call->providerIdx, true, kept, missing});
            auto* raw  = next.get();
            sd_bus_slot* slot = nullptr;
            if (sd_bus_call_async(bus, &slot, req, onMetas, raw, CALL_TIMEOUT_US) >= 0) {
                w_slots.push_back(slot);
                w_calls.push_back(std::move(next));
            }
            sd_bus_message_unref(req);
            return 0;
        }

        // X54. A search provider is a D-BUS ACTIVATED service, so if nothing has
        // talked to it lately it is not running and the first keystroke pays
        // for launching a whole GTK app. Measured on this guest: 3 rounds of
        // the same query with 45s of idle between them cost 928/885/887ms --
        // it goes cold again every time, so this is not a first-search cost,
        // it is the cost of EVERY search. StartServiceByName does the launch
        // and nothing else; called at overview open it runs while the open
        // animation plays and the user is still reaching for the keyboard.
        std::vector<sd_bus_slot*> w_warmSlots;

        int onWarmed(sd_bus_message*, void*, sd_bus_error*) {
            return 0;
        }

        void warmProviders(sd_bus* bus) {
            for (auto* S : w_warmSlots)
                sd_bus_slot_unref(S);
            w_warmSlots.clear();

            for (const auto& PROV : g_providers) {
                sd_bus_message* req = nullptr;
                if (sd_bus_message_new_method_call(bus, &req, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "StartServiceByName") < 0)
                    continue;
                sd_bus_message_append(req, "su", PROV.busName.c_str(), 0U);
                sd_bus_slot* slot = nullptr;
                if (sd_bus_call_async(bus, &slot, req, onWarmed, nullptr, CALL_TIMEOUT_US) >= 0)
                    w_warmSlots.push_back(slot);
                sd_bus_message_unref(req);
            }
        }

        void startGeneration(sd_bus* bus) {
            w_genStart = SPClock::now();
            dropInFlight();
            auto prev = std::move(w_prevIds);
            w_prevIds.assign(g_providers.size(), {});

            if (w_terms.empty()) {
                w_prevQuery.clear();
                return;
            }

            // X20: when the query only got longer, hand back the previous ids so
            // providers narrow instead of searching from scratch.
            const bool EXTENDS = !w_prevQuery.empty() && w_query.starts_with(w_prevQuery);

            for (size_t i = 0; i < g_providers.size(); ++i) {
                const auto&     PROV = g_providers[i];
                sd_bus_message* req  = nullptr;
                const bool      SUB  = EXTENDS && i < prev.size() && !prev[i].empty();

                if (sd_bus_message_new_method_call(bus, &req, PROV.busName.c_str(), PROV.objectPath.c_str(), IFACE,
                                                   SUB ? "GetSubsearchResultSet" : "GetInitialResultSet") < 0)
                    continue;

                if (SUB) {
                    auto PREV_ARGV = cstrv(prev[i]);
                    sd_bus_message_append_strv(req, PREV_ARGV.data());
                    w_subsearchCalls.fetch_add(1);
                }
                auto TERM_ARGV = cstrv(w_terms);
                sd_bus_message_append_strv(req, TERM_ARGV.data());

                auto         call = std::make_unique<SCall>(SCall{w_gen, i, false});
                auto*        raw  = call.get();
                sd_bus_slot* slot = nullptr;
                if (sd_bus_call_async(bus, &slot, req, onResultSet, raw, CALL_TIMEOUT_US) >= 0) {
                    w_slots.push_back(slot);
                    w_calls.push_back(std::move(call));
                }
                sd_bus_message_unref(req);
            }
            w_prevQuery = w_query;
        }

        void workerMain() {
            sd_bus* bus = nullptr;
            if (sd_bus_open_user(&bus) < 0) {
                // Nothing queued can ever run without a bus: an ActivateResult
                // or a warm-up left in g_actions would fire against the NEXT
                // bus, minutes later, for a query nobody is looking at.
                {
                    std::lock_guard lock(g_mutex);
                    g_actions.clear();
                    for (auto& R : g_results)
                        R.stale = false; // nothing will answer; do not leave rows "pending" forever
                }
                if (g_busFailures.fetch_add(1) == 0)
                    std::fprintf(stderr, "[hyprdrive] search providers: no session bus (sd_bus_open_user failed); GNOME search providers are off\n");
                g_busFailedAt = steadyNs();
                g_running     = false;
                return; // ensureWorker() joins this thread before it starts another
            }

            while (true) {
                std::vector<std::function<void(sd_bus*)>> actions;
                bool                                      newQuery = false;
                {
                    std::unique_lock lock(g_mutex);
                    if (g_stop)
                        break;
                    if (g_wantGen != w_gen) {
                        w_gen    = g_wantGen;
                        w_query  = g_wantQuery;
                        w_terms  = splitTerms(w_query);
                        newQuery = true;
                    }
                    actions.swap(g_actions);
                }

                for (auto& A : actions)
                    A(bus);
                if (newQuery)
                    startGeneration(bus);

                int r = 0;
                do {
                    r = sd_bus_process(bus, nullptr);
                } while (r > 0);

                uint64_t deadline = 0;
                sd_bus_get_timeout(bus, &deadline);
                int msec = 1000;
                if (deadline != UINT64_MAX) {
                    timespec now{};
                    clock_gettime(CLOCK_MONOTONIC, &now);
                    const uint64_t NOW = static_cast<uint64_t>(now.tv_sec) * 1'000'000 + now.tv_nsec / 1000;
                    msec               = deadline <= NOW ? 0 : static_cast<int>(std::min<uint64_t>((deadline - NOW) / 1000 + 1, 1000));
                }

                pollfd pfds[2] = {
                    {sd_bus_get_fd(bus), static_cast<short>(sd_bus_get_events(bus)), 0},
                    {g_wakeFd[0], POLLIN, 0},
                };
                poll(pfds, 2, msec);

                if (pfds[1].revents & POLLIN) {
                    char drain[64];
                    while (::read(g_wakeFd[0], drain, sizeof(drain)) > 0) { }
                }
            }

            dropInFlight();
            sd_bus_flush_close_unref(bus);
            g_running = false;
        }

        void ensureWorker() {
            if (g_running.load())
                return;
            // No bus a moment ago: do not spin a thread up per keystroke to find
            // out again. The overview simply has no provider rows meanwhile.
            if (const auto FAILED = g_busFailedAt.load(); FAILED != 0 && steadyNs() - FAILED < g_busRetryNs.load())
                return;
            if (g_running.exchange(true))
                return;
            // A worker that ended on its own (no bus) is finished or finishing:
            // g_running goes false as its very last act, so this join is
            // immediate. Without it the assignment below is std::terminate.
            if (g_worker.joinable())
                g_worker.join();
            // ...and its wake pipe goes with it, or every retry leaks two fds.
            for (int& fd : g_wakeFd) {
                if (fd >= 0)
                    close(fd);
                fd = -1;
            }
            if (pipe(g_wakeFd) < 0) {
                g_wakeFd[0] = g_wakeFd[1] = -1;
                g_running                 = false;
                return;
            }
            // Non-blocking read end, so draining never parks the worker.
            for (int i = 0; i < 2; ++i) {
                const int FL = fcntl(g_wakeFd[i], F_GETFL, 0);
                fcntl(g_wakeFd[i], F_SETFL, FL | O_NONBLOCK);
            }
            g_stop   = false;
            g_worker = std::thread(workerMain);
        }
    }

    const std::vector<SProvider>& providers() {
        if (!g_scanned)
            scan();
        return g_providers;
    }

    void refresh() {
        scan();
    }

    void query(const std::string& text) {
        providers();
        ensureWorker();
        {
            std::lock_guard lock(g_mutex);
            if (g_wantQuery == text)
                return;
            g_wantQuery = text;
            ++g_wantGen;
            // X20: refine, do not blank. Clearing here emptied the panel on
            // every keystroke and refilled it a frame later, which reads as a
            // submit step. The previous rows stay up flagged stale until each
            // provider answers -- an empty query has no answer to wait for.
            if (text.empty())
                g_results.clear();
            else
                for (auto& R : g_results)
                    R.stale = true;
        }
        wake();
    }

    void warm() {
        providers();
        ensureWorker();
        {
            std::lock_guard lock(g_mutex);
            g_actions.emplace_back([](sd_bus* bus) { warmProviders(bus); });
        }
        wake();
    }

    std::vector<SResult> results() {
        std::lock_guard lock(g_mutex);
        return g_results;
    }

    void activate(const SResult& result, const std::string& queryText) {
        providers();
        ensureWorker();
        const auto PROV = std::ranges::find_if(g_providers, [&](const SProvider& P) { return P.busName == result.providerBus; });
        if (PROV == g_providers.end())
            return;

        SProvider   prov  = *PROV;
        std::string id    = result.id;
        auto        terms = splitTerms(queryText);
        {
            std::lock_guard lock(g_mutex);
            g_actions.emplace_back([prov, id, terms](sd_bus* bus) mutable {
                sd_bus_message* req = nullptr;
                if (sd_bus_message_new_method_call(bus, &req, prov.busName.c_str(), prov.objectPath.c_str(), IFACE, "ActivateResult") < 0)
                    return;
                sd_bus_message_append_basic(req, 's', id.c_str());
                auto ARGV = cstrv(terms);
                sd_bus_message_append_strv(req, ARGV.data());
                const uint32_t TS = 0;
                sd_bus_message_append_basic(req, 'u', &TS);
                sd_bus_call_async(bus, nullptr, req, nullptr, nullptr, CALL_TIMEOUT_US);
                sd_bus_message_unref(req);
            });
        }
        wake();
    }

    void shutdown() {
        if (g_worker.joinable()) {
            {
                std::lock_guard lock(g_mutex);
                g_stop = true;
            }
            wake();
            g_worker.join();
        }
        // Also when the worker already ended on its own (no bus): its pipe is
        // still open until ensureWorker or this closes it.
        for (int& fd : g_wakeFd) {
            if (fd >= 0)
                close(fd);
            fd = -1;
        }
        g_running = false;
    }

    uint64_t busFailures() {
        return g_busFailures.load();
    }

    void setBusRetry(std::chrono::milliseconds retry) {
        g_busRetryNs = std::chrono::nanoseconds(retry).count();
    }
}

// Build with:
//   g++ -std=c++2b -DSEARCHPROVIDERS_TEST SearchProviders.cpp -o /tmp/spv_test \
//       $(pkg-config --cflags --libs libsystemd)
#ifdef SEARCHPROVIDERS_TEST
    #include <cassert>
    #include <cstdio>

namespace {
    // Poll for up to timeoutMs, returning as soon as pred() holds. Measures the
    // wall clock so the dead-provider assertion below is real.
    long waitFor(const std::function<bool()>& pred, long timeoutMs) {
        timespec t0{};
        clock_gettime(CLOCK_MONOTONIC, &t0);
        while (true) {
            timespec now{};
            clock_gettime(CLOCK_MONOTONIC, &now);
            const long MS = (now.tv_sec - t0.tv_sec) * 1000 + (now.tv_nsec - t0.tv_nsec) / 1'000'000;
            if (pred() || MS >= timeoutMs)
                return MS;
            usleep(20'000);
        }
    }

    // X20 made results() carry the PREVIOUS query's rows until each provider
    // answers, so "not empty" stopped meaning "answered" -- waiting on it
    // returns instantly with the old set and every assertion after it is about
    // the wrong query. Anything that means "answered" waits on these.
    std::vector<SearchProviders::SResult> fresh() {
        auto r = SearchProviders::results();
        std::erase_if(r, [](const SearchProviders::SResult& E) { return E.stale; });
        return r;
    }
}

int main() {
    using namespace SearchProviders;

    // X54 bench. Does a provider go COLD again after an idle gap? If it does,
    // every keystroke-after-a-pause pays D-Bus activation of a whole GTK app,
    // and that -- not matching, not layout -- is the wait the user sees.
    // SP_BENCH=<idle seconds>.
    if (const char* IDLE = std::getenv("SP_BENCH")) {
        providers();
        const int GAP = std::atoi(IDLE);
        const bool DO_WARM = std::getenv("SP_WARM") != nullptr;
        for (int round = 0; round < 3; ++round) {
            query("");
            usleep(50'000);
            // The overview open, then the human reaching for the keyboard.
            if (DO_WARM) {
                warm();
                usleep(300'000);
            }
            query("heart");
            const long MS = waitFor([] { return fresh().size() >= 4; }, 9000);
            std::printf("round %d: settle %ld ms, %zu fresh\n", round, MS, fresh().size());
            std::fflush(stdout);
            if (round < 2) {
                std::printf("--- idle %ds ---\n", GAP);
                std::fflush(stdout);
                sleep(GAP);
            }
        }
        shutdown();
        return 0;
    }

    assert(splitTerms("  two  words ").size() == 2);
    assert(splitTerms("").empty());

    const auto& PROV = providers();
    std::printf("providers: %zu\n", PROV.size());
    for (const auto& P : PROV)
        std::printf("  %-34s %s %s v%d\n", P.desktopId.c_str(), P.busName.c_str(), P.objectPath.c_str(), P.version);
    assert(!PROV.empty());

    // DoD clause 15, "I can do sums in it".
    query("2+2");
    long ms = waitFor([] { return std::ranges::any_of(results(), [](const SResult& R) { return R.description.find('4') != std::string::npos || R.clipboardText == "4"; }); }, 3000);
    auto R  = results();
    std::printf("query \"2+2\": %zu results in %ld ms\n", R.size(), ms);
    for (const auto& E : R)
        std::printf("  [%s] id=%s name=%s desc=%s icon=%s clip=%s\n", E.providerDesktopId.c_str(), E.id.c_str(), E.name.c_str(), E.description.c_str(), E.icon.c_str(), E.clipboardText.c_str());
    assert(std::ranges::any_of(R, [](const SResult& E) { return E.description.find('4') != std::string::npos || E.clipboardText == "4"; }));

    // The whole point of X19's async requirement: org.gnome.Weather activates
    // and then never replies (28 s to the D-Bus default). If any of that leaked
    // into the caller this would not be under a second.
    assert(ms < 2000);

    // X20: a new query now KEEPS the old rows so the panel refines instead of
    // blanking. They must all be flagged stale until their provider answers --
    // that flag is what still guards against reading the previous query's
    // results as this query's answer, which regressed once and read as an
    // instant success.
    query("heart");
    assert(std::ranges::all_of(results(), [](const SResult& E) { return E.stale; }));

    // DoD clause 15, "find characters".
    ms = waitFor([] { return !fresh().empty(); }, 3000);
    R  = fresh();
    std::printf("query \"heart\": %zu results in %ld ms\n", R.size(), ms);
    assert(!R.empty());
    assert(std::ranges::any_of(R, [](const SResult& E) { return !E.name.empty(); }));

    // X20 groundwork: extending the query must take the GetSubsearchResultSet
    // path, and the set must still be coherent, not the previous leftovers.
    const uint64_t SUBS = w_subsearchCalls.load();
    query("heart s");
    ms = waitFor([] { return !fresh().empty(); }, 3000);
    std::printf("query \"heart s\" (subsearch): %zu results in %ld ms, %llu subsearch calls\n", fresh().size(), ms,
                static_cast<unsigned long long>(w_subsearchCalls.load() - SUBS));
    assert(w_subsearchCalls.load() > SUBS);

    // X20 proper: refining a live query must never leave the panel empty, not
    // for a single poll. That gap is the whole difference between "filters as I
    // type" and a submit step, and it is invisible in a screenshot -- it lasts
    // one round trip. Every step below extends the last, so the answer set only
    // narrows; if it ever reads empty, retention is broken.
    query("dis");
    waitFor([] { return !fresh().empty(); }, 3000);
    bool everEmpty = false;
    for (const char* Q : {"disp", "displa", "display"}) {
        query(Q);
        for (int i = 0; i < 40; ++i) {
            if (results().empty())
                everEmpty = true;
            usleep(5'000);
        }
    }
    std::printf("X20 refine \"dis\"->\"display\": blanked = %s, settled %zu results\n", everEmpty ? "YES" : "no", results().size());
    assert(!everEmpty);
    // Retention makes "never empty" cheap to satisfy by never answering, so the
    // set has to settle FRESH for the assertion above to mean anything.
    waitFor([] { return !fresh().empty(); }, 3000);
    assert(fresh().size() == results().size());

    // The other icon form. Settings serializes a GIcon as ('themed', <as>)
    // while Calculator sends a bare string; a client that handles only one of
    // them silently loses every icon from half the providers.
    query("display");
    ms = waitFor([] { return std::ranges::any_of(fresh(), [](const SResult& E) { return E.providerDesktopId == "org.gnome.Settings"; }); }, 3000);
    R  = fresh();
    std::printf("query \"display\": %zu results in %ld ms\n", R.size(), ms);
    for (const auto& E : R)
        std::printf("  [%s] id=%s name=%s icon=%s\n", E.providerDesktopId.c_str(), E.id.c_str(), E.name.c_str(), E.icon.c_str());
    const auto SETTINGS = std::ranges::find_if(R, [](const SResult& E) { return E.providerDesktopId == "org.gnome.Settings"; });
    assert(SETTINGS != R.end());
    assert(!SETTINGS->icon.empty()); // the ('themed', as) branch

    // Empty query clears, and does it without a round trip.
    query("");
    waitFor([] { return results().empty(); }, 1000);
    assert(results().empty());

    // A provider that answers must still resolve an icon name for the panel.
    query("2+2");
    waitFor([] { return !fresh().empty(); }, 3000);
    assert(std::ranges::any_of(fresh(), [](const SResult& E) { return !E.icon.empty(); }));

    shutdown();
    std::printf("SearchProviders self-check OK\n");
    return 0;
}
#endif
