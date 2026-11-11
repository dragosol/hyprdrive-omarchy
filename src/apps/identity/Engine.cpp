#include "Engine.hpp"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <format>
#include <map>
#include <mutex>
#include <unordered_map>

namespace Hyprdrive::AppIdentity {
    namespace {
        int64_t steadyNs() {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        bool under(const std::string& path, const std::string& dir) {
            return !dir.empty() && path.size() > dir.size() && path.starts_with(dir) && path[dir.size()] == '/';
        }

        // A weak answer: a helper resolved before its app, or nothing better
        // than the class.
        bool weak(const Identity& i) {
            return i.confidence < 60 || i.resolver == "Fallback" || i.resolver == "X11Props";
        }

        // Can own a session: a strong answer that is not itself a helper's.
        bool owner(const Identity& i) {
            return !weak(i) && i.resolver != "HelperOfSession" && i.placeable;
        }

        // "steam-app-<id>" / "steam-shortcut-<id32>" -> the id the index
        // must have; nullopt for any other key.
        std::optional<Steam::GameId> steamKey(std::string_view key) {
            for (const auto& [PRE, KIND] : {std::pair{std::string_view{"steam-app-"}, Steam::GameId::Kind::App},
                                            std::pair{std::string_view{"steam-shortcut-"}, Steam::GameId::Kind::Shortcut}})
                if (key.starts_with(PRE)) {
                    uint64_t v = 0;
                    auto     D = key.substr(PRE.size());
                    if (D.empty() || D.size() > 10)
                        return std::nullopt;
                    for (const char C : D) {
                        if (C < '0' || C > '9')
                            return std::nullopt;
                        v = v * 10 + static_cast<uint64_t>(C - '0');
                    }
                    if (v == 0 || v > 0xffffffffull)
                        return std::nullopt;
                    return Steam::GameId{KIND, static_cast<uint32_t>(v)};
                }
            return std::nullopt;
        }
    }

    struct Engine::SShared {
        Options                     o;
        mutable std::mutex          m;
        std::condition_variable_any cv;
        std::deque<Job>             jobs;
        std::vector<Result>         results;
        bool                        rescanFlag = false, indexesChanged = false;

        // Worker-only below (except under `m` where noted).
        std::shared_ptr<const std::vector<InventoryEntry>> inv; // under m
        Indexes                                            ix;  // under m (read by indexes())
        std::string                                        signature;
        int64_t                                            lastCheckNs = 0;
        struct SMemo {
            Job          job;
            ProcessFacts p;
            Identity     id;
            std::string  session;
        };
        std::unordered_map<uintptr_t, SMemo> memo; // under m (cancel() erases)
        std::map<std::string, int64_t>       probeFailedUntil, probeOkUntil;
        uint64_t                             resolved = 0, rebuilds = 0, probesFailed = 0; // under m

        // ---- the removable-library probe ---------------------------------------
        bool reachable(const std::string& lib) {
            const Env& E = o.src->env();
            if (under(lib, E.home) || lib == E.home)
                return true;
            const int64_t NOW = steadyNs();
            if (auto it = probeFailedUntil.find(lib); it != probeFailedUntil.end() && it->second > NOW)
                return false;
            if (auto it = probeOkUntil.find(lib); it != probeOkUntil.end() && it->second > NOW)
                return true;
            struct SProbe {
                std::mutex              m;
                std::condition_variable cv;
                bool                    done = false, ok = false;
            };
            auto           pr  = std::make_shared<SProbe>();
            const Sources* SRC = o.src;
            const bool     LAUNCHED = Workers::launch("hd-id-probe", [pr, SRC, lib](std::stop_token) {
                bool ok = false;
                try {
                    ok = SRC->stat(lib).has_value();
                } catch (...) {}
                std::lock_guard l(pr->m);
                pr->done = true;
                pr->ok   = ok;
                pr->cv.notify_all();
            });
            bool ok = false;
            if (LAUNCHED) {
                std::unique_lock l(pr->m);
                pr->cv.wait_for(l, o.probeTimeout, [&] { return pr->done; });
                ok = pr->done && pr->ok;
            }
            constexpr int64_t MINUTE = 60'000'000'000LL;
            if (ok)
                probeOkUntil[lib] = NOW + MINUTE;
            else {
                probeFailedUntil[lib] = NOW + MINUTE;
                std::lock_guard l(m);
                ++probesFailed;
            }
            return ok;
        }

        // ---- the sources' signature: what a rebuild would read changed ---------
        std::string currentSignature() {
            std::string sig;
            const auto  STAMP = [&](const std::string& p) {
                if (const auto I = o.src->stat(p))
                    sig += std::format("{}={};", p, I->mtimeNs);
            };
            const Env& E = o.src->env();
            for (const auto& ROOT : SteamLibrary::roots(E)) {
                STAMP(ROOT + "/steamapps/libraryfolders.vdf");
                for (const auto& UID : o.src->list(ROOT + "/userdata", 64))
                    STAMP(ROOT + "/userdata/" + UID + "/config/shortcuts.vdf");
            }
            if (const auto& S = ix.steam)
                for (const auto& LIB : S->libraries)
                    if (reachable(LIB))
                        STAMP(LIB); // a manifest added or renamed changes its folder's mtime
            for (const auto& D : {E.dataHome + "/lutris", E.home + "/.var/app/net.lutris.Lutris/data/lutris"}) {
                STAMP(D + "/pga.db");
                STAMP(D + "/games");
            }
            STAMP(E.home + "/.config/lutris/games");
            return sig;
        }

        void rebuild() {
            Indexes next;
            try {
                next.steam  = SteamLibrary::rebuild(*o.src, [this](const std::string& lib) { return reachable(lib); });
                next.lutris = LutrisLibrary::rebuild(*o.src);
            } catch (...) { return; }
            if (o.publish) {
                SteamLibrary::publish(next.steam);
                LutrisLibrary::publish(next.lutris);
            }
            {
                std::lock_guard l(m);
                ix             = next;
                indexesChanged = true;
                ++rebuilds;
            }
            try {
                signature = currentSignature();
            } catch (...) {}
            lastCheckNs = steadyNs();
            if (o.wake)
                o.wake();
        }

        void recheck() {
            if (steadyNs() - lastCheckNs < std::chrono::duration_cast<std::chrono::nanoseconds>(o.recheckEvery).count())
                return;
            lastCheckNs = steadyNs();
            std::string now;
            try {
                now = currentSignature();
            } catch (...) { return; }
            if (now != signature)
                rebuild();
        }

        // ---- one resolution -----------------------------------------------------
        Identity resolveOne(const WindowFacts& w, const ProcessFacts& p, std::shared_ptr<const std::vector<InventoryEntry>> invp) {
            static const std::vector<InventoryEntry> NONE;
            const auto&                              INV = invp ? *invp : NONE;
            InventoryView                            view{.entries = INV};
            // The strongest owner of each session among the windows resolved,
            // copied: cancel() edits the memory from the compositor thread.
            std::unordered_map<std::string, Identity> owners;
            {
                std::lock_guard l(m);
                for (const auto& [A, M] : memo)
                    if (!M.session.empty() && owner(M.id))
                        if (auto [it, fresh] = owners.try_emplace(M.session, M.id); !fresh && M.id.confidence > it->second.confidence)
                            it->second = M.id;
            }
            view.sessionOwner = [&owners](std::string_view key) -> const Identity* {
                const auto IT = owners.find(std::string{key});
                return IT == owners.end() ? nullptr : &IT->second;
            };
            Indexes IX;
            {
                std::lock_guard l(m);
                IX = ix;
            }
            Identity id = resolveWith(w, p, IX, view, o.src);
            // A Steam id the index lacks: rebuild once per NEGATIVE_TTL, then again.
            if (const auto SK = steamKey(id.key); SK && IX.steam) {
                const bool HAVE = SK->kind == Steam::GameId::Kind::App ? IX.steam->app(SK->id) != nullptr : IX.steam->shortcut(SK->id) != nullptr;
                if (!HAVE && steadyNs() - IX.steam->builtAtNs >= SteamLibrary::NEGATIVE_TTL_NS) {
                    rebuild();
                    {
                        std::lock_guard l(m);
                        IX = ix;
                    }
                    id = resolveWith(w, p, IX, view, o.src);
                }
            }
            return id;
        }

        void post(Result r) {
            if (o.record)
                try {
                    o.record(r);
                } catch (...) {}
            {
                std::lock_guard l(m);
                results.push_back(std::move(r));
                ++resolved;
            }
            if (o.wake)
                o.wake();
        }

        void run(std::stop_token st) {
            rebuild();
            while (!st.stop_requested()) {
                Job  job;
                bool haveJob = false, doRescan = false;
                std::shared_ptr<const std::vector<InventoryEntry>> invp;
                {
                    std::unique_lock l(m);
                    cv.wait(l, st, [&] { return !jobs.empty() || rescanFlag; });
                    if (st.stop_requested())
                        break;
                    if (rescanFlag) {
                        rescanFlag = false;
                        doRescan   = true;
                    } else {
                        job = std::move(jobs.front());
                        jobs.pop_front();
                        haveJob = true;
                    }
                    invp = inv;
                }
                if (doRescan) {
                    rebuild();
                    continue;
                }
                if (!haveJob)
                    continue;
                recheck();
                if (o.jobDelay.count() > 0) {
                    std::unique_lock l(m);
                    cv.wait_for(l, st, o.jobDelay, [] { return false; });
                    if (st.stop_requested())
                        break;
                }
                try {
                    if (o.gather)
                        o.gather->window(job.w, job.display);
                    ProcessFacts p = o.gather && job.w.pid > 0 ? o.gather->process(job.w.pid) : ProcessFacts{};
                    Identity     id  = resolveOne(job.w, p, invp);
                    std::string  ses = sessionKey(p);
                    const bool   NEWOWNER = !ses.empty() && owner(id);
                    {
                        std::lock_guard l(m);
                        memo[job.addr] = SMemo{.job = job, .p = p, .id = id, .session = ses};
                    }
                    post(Result{.addr = job.addr, .serial = job.serial, .w = job.w, .p = std::move(p), .id = std::move(id), .session = ses});
                    // Helpers of this session resolved before it: again, now.
                    if (NEWOWNER) {
                        std::vector<uintptr_t> again;
                        {
                            std::lock_guard l(m);
                            for (const auto& [A, M] : memo)
                                if (A != job.addr && M.session == ses && weak(M.id))
                                    again.push_back(A);
                        }
                        for (const uintptr_t A : again) {
                            SMemo copy;
                            {
                                std::lock_guard l(m);
                                const auto IT = memo.find(A);
                                if (IT == memo.end())
                                    continue;
                                copy = IT->second;
                            }
                            Identity re = resolveOne(copy.job.w, copy.p, invp);
                            if (re.key == copy.id.key && re.displayName == copy.id.displayName)
                                continue;
                            {
                                std::lock_guard l(m);
                                const auto IT = memo.find(A);
                                if (IT == memo.end() || IT->second.job.serial != copy.job.serial)
                                    continue;
                                IT->second.id = re;
                            }
                            post(Result{.addr = A, .serial = copy.job.serial, .w = copy.job.w, .p = copy.p, .id = std::move(re), .session = copy.session});
                        }
                    }
                } catch (...) {
                    // A resolution that failed is the provisional answer kept.
                }
            }
            try {
                if (o.gather)
                    o.gather->close();
            } catch (...) {}
        }
    };

    Engine::Engine(Options o) : m_o(std::move(o)) {}

    Engine::~Engine() {
        stop();
    }

    bool Engine::start() {
        if (m_worker.joinable() || !m_o.src)
            return m_worker.joinable();
        m_s    = std::make_shared<SShared>();
        m_s->o = m_o;
        auto s = m_s;
        m_worker = Workers::start(m_o.threadName, [s](std::stop_token st) { s->run(st); });
        return m_worker.joinable();
    }

    bool Engine::stop() {
        if (!m_worker.joinable())
            return true;
        m_worker.requestStop();
        if (m_s)
            m_s->cv.notify_all();
        return m_worker.join();
    }

    bool Engine::running() const {
        return m_worker.joinable() && !m_worker.done();
    }

    void Engine::enqueue(Job j) {
        if (!m_s)
            return;
        {
            std::lock_guard l(m_s->m);
            // A newer job for the same window replaces the queued one.
            std::erase_if(m_s->jobs, [&](const Job& q) { return q.addr == j.addr; });
            m_s->jobs.push_back(std::move(j));
        }
        m_s->cv.notify_all();
    }

    void Engine::cancel(uintptr_t addr) {
        if (!m_s)
            return;
        std::lock_guard l(m_s->m);
        std::erase_if(m_s->jobs, [&](const Job& q) { return q.addr == addr; });
        m_s->memo.erase(addr);
        std::erase_if(m_s->results, [&](const Result& r) { return r.addr == addr; });
    }

    void Engine::setInventory(std::shared_ptr<const std::vector<InventoryEntry>> inv) {
        if (!m_s)
            return;
        std::lock_guard l(m_s->m);
        m_s->inv = std::move(inv);
    }

    void Engine::rescan() {
        if (!m_s)
            return;
        {
            std::lock_guard l(m_s->m);
            m_s->rescanFlag = true;
        }
        m_s->cv.notify_all();
    }

    std::vector<Result> Engine::take() {
        if (!m_s)
            return {};
        std::lock_guard l(m_s->m);
        return std::exchange(m_s->results, {});
    }

    bool Engine::takeIndexesChanged() {
        if (!m_s)
            return false;
        std::lock_guard l(m_s->m);
        return std::exchange(m_s->indexesChanged, false);
    }

    Engine::Stats Engine::stats() const {
        if (!m_s)
            return {};
        std::lock_guard l(m_s->m);
        return {.queued       = m_s->jobs.size(),
                .sessions     = m_s->memo.size(),
                .resolved     = m_s->resolved,
                .rebuilds     = m_s->rebuilds,
                .probesFailed = m_s->probesFailed,
                .indexed      = m_s->ix.steam != nullptr};
    }

    Indexes Engine::indexes() const {
        if (!m_s)
            return {};
        std::lock_guard l(m_s->m);
        return m_s->ix;
    }
}
