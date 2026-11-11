#include "IdentityLive.hpp"

#include "AppSearch.hpp"
#include "Dock.hpp"
#include "Engine.hpp"
#include "GameApps.hpp"
#include "LiveGatherer.hpp"
#include "globals.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/protocols/types/ContentType.hpp>
#include <hyprland/src/xwayland/XSurface.hpp>

#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

#include <chrono>
#include <format>
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace Hyprdrive::AppIdentity {
    namespace {
        // How long after a map a changed identity still applies the new
        // app's "Always Open in" (owner ruling, Q8).
        constexpr std::chrono::milliseconds LATE_APPLY{1500};

        enum class EState : uint8_t { Provisional, Pending, Done };
        struct SSlot {
            uint64_t                              serial = 0;
            WindowFacts                           facts;
            Identity                              id;
            std::string                           group;
            EState                                state = EState::Provisional;
            std::chrono::steady_clock::time_point mappedAt;
            std::string                           proc; // what the worker read of the process (the debug dump)
        };

        std::unique_ptr<Engine>                       g_engine;
        std::unordered_map<const Desktop::View::CWindow*, SSlot>     g_slots;
        std::map<std::pair<std::string, pid_t>, Identity> g_l2; // (class, pid) -> a sibling's answer
        std::unordered_map<std::string, Identity>     g_byKey;
        std::shared_ptr<const std::vector<InventoryEntry>> g_inv = std::make_shared<std::vector<InventoryEntry>>();
        uint64_t                                      g_invGen = 0, g_invHash = 0, g_generation = 1, g_serial = 0;
        int                                           g_wakeFd = -1;
        wl_event_source*                              g_wakeSource = nullptr;
        long                                          g_clkTck     = 100;
        bool                                          g_live       = false;
        CHyprSignalListener g_openEarly, g_close, g_class, g_title;
        Identity            g_none;
        std::unordered_map<std::string, std::string> g_canon; // canonicalKey, per inventory generation

        uint64_t bootTicks() {
            timespec ts{};
            clock_gettime(CLOCK_BOOTTIME, &ts);
            return static_cast<uint64_t>(ts.tv_sec) * static_cast<uint64_t>(g_clkTck) + static_cast<uint64_t>(ts.tv_nsec) / (1'000'000'000ULL / static_cast<uint64_t>(g_clkTck));
        }

        WindowFacts factsOf(const PHLWINDOW& W) {
            WindowFacts f;
            f.cls          = W->m_class;
            f.initialClass = W->m_initialClass;
            f.title        = W->m_title;
            f.x11          = W->m_isX11;
            f.contentGame  = W->getContentType() == NContentType::CONTENT_TYPE_GAME;
            f.pid          = W->getPID();
            f.hasParent    = W->parent() != nullptr;
            if (W->m_isX11) {
                f.overrideRedirect = W->isX11OverrideRedirect();
                f.hasParent        = f.hasParent || W->x11Parent() != nullptr;
                if (const auto XS = W->m_xwaylandSurface.lock()) {
                    f.xid = XS->m_xID;
                    f.w   = static_cast<int>(XS->m_geometry.width);
                    f.h   = static_cast<int>(XS->m_geometry.height);
                    if (XS->m_sizeHints) {
                        const auto& H = *XS->m_sizeHints;
                        f.fixedSize   = H.min_width > 0 && H.min_height > 0 && H.min_width == H.max_width && H.min_height == H.max_height;
                    }
                }
            }
            return f;
        }

        void reresolveAll();

        // The app inventory as the resolvers see it, copied when AppSearch
        // rescanned (its generation) AND what the resolvers read changed.
        bool syncInventory() {
            const uint64_t GEN = AppSearch::inventoryGeneration();
            if (GEN == g_invGen)
                return false;
            g_invGen = GEN;
            g_canon.clear();
            auto     inv = std::make_shared<std::vector<InventoryEntry>>();
            uint64_t h   = 1469598103934665603ULL;
            for (const auto& E : AppSearch::inventory()) {
                inv->push_back({.id = E.id, .name = E.name, .icon = E.icon, .wmClass = E.wmClass, .exec = E.exec, .mtimeNs = E.mtimeNs});
                for (const std::string* S : {&E.id, &E.name, &E.icon, &E.wmClass, &E.exec})
                    h = (h ^ std::hash<std::string>{}(*S)) * 1099511628211ULL;
                h = (h ^ static_cast<uint64_t>(E.mtimeNs)) * 1099511628211ULL;
            }
            g_inv = std::move(inv);
            if (g_engine)
                g_engine->setInventory(g_inv);
            if (h == g_invHash)
                return false;
            g_invHash = h;
            reresolveAll(); // every window against the new entries
            return true;
        }

        InventoryView view() {
            return InventoryView{.entries = *g_inv};
        }

        void enqueue(const Desktop::View::CWindow* W, SSlot& s) {
            if (!g_engine)
                return;
            const char* D = getenv("DISPLAY");
            g_engine->enqueue({.addr = reinterpret_cast<uintptr_t>(W), .serial = s.serial, .w = s.facts, .display = D ? D : ""});
            if (s.state != EState::Done)
                s.state = EState::Pending;
        }

        SSlot& open(const PHLWINDOW& W) {
            SSlot& s   = g_slots[W.get()];
            s          = SSlot{};
            s.serial   = ++g_serial;
            s.facts    = factsOf(W);
            s.facts.mapTicks = bootTicks();
            s.mappedAt = std::chrono::steady_clock::now();
            s.id       = resolveFast(s.facts, Indexes::current(), view());
            // A sibling of the same process already resolved: its answer, until
            // this one's own lands (better than the class, never worse).
            if (s.id.resolver == "Fallback" || s.id.confidence < 60)
                if (const auto IT = g_l2.find({s.facts.cls, s.facts.pid}); IT != g_l2.end())
                    s.id = IT->second;
            s.group = groupKeyOf(s.id, s.facts);
            if (s.id.resolver != "Fallback")
                g_byKey[s.id.key] = s.id;
            enqueue(W.get(), s);
            return s;
        }

        void forget(const Desktop::View::CWindow* W) {
            const auto IT = g_slots.find(W);
            if (IT == g_slots.end())
                return;
            const auto CLS = IT->second.facts.cls;
            const auto PID = IT->second.facts.pid;
            g_slots.erase(IT);
            if (g_engine)
                g_engine->cancel(reinterpret_cast<uintptr_t>(W));
            // L2 lives as long as one window of that process does.
            if (std::ranges::none_of(g_slots, [&](const auto& KV) { return KV.second.facts.pid == PID; }))
                std::erase_if(g_l2, [&](const auto& KV) { return KV.first.second == PID; });
            (void)CLS;
        }

        void changed() {
            ++g_generation;
            Membership::bump("identity");
            if (State::monitorState())
            for (const auto& M : State::monitorState()->monitors())
                    if (M)
                        Dock::damageIfChanged(M);
        }

        // Every window again: the inventory or the indexes changed under them.
        void reresolveAll() {
            for (auto& [W, s] : g_slots) {
                const Identity FAST = resolveFast(s.facts, Indexes::current(), view());
                if (s.state != EState::Done) {
                    s.id    = FAST;
                    s.group = groupKeyOf(s.id, s.facts);
                }
                enqueue(W, s);
            }
        }

        int onWake(int fd, uint32_t, void*) {
            uint64_t v = 0;
            while (::read(fd, &v, sizeof(v)) > 0) {}
            if (!g_engine)
                return 0;
            try {
                bool dirty = false;
                if (g_engine->takeIndexesChanged()) {
                    AppSearch::refreshExtras(); // Lutris's games are inventory entries
                    if (!syncInventory())
                        reresolveAll(); // the indexes alone changed
                    dirty = true;
                }
                if (GameApps::takeChanged()) {
                    AppSearch::refreshExtras();
                    syncInventory(); // the new entries; the windows that made them are resolved already
                    dirty = true;
                }
                // Live windows by address: a result for a window that has gone
                // (or a reused address: the serial) is dropped, never dereferenced.
                std::unordered_map<const Desktop::View::CWindow*, PHLWINDOW> live;
                for (const auto& W : Desktop::windowState()->windows())
                    if (W)
                        live.emplace(W.get(), W);
                const auto NOW = std::chrono::steady_clock::now();
                for (auto& r : g_engine->take()) {
                    const auto* ADDR = reinterpret_cast<const Desktop::View::CWindow*>(r.addr);
                    const auto  IT   = g_slots.find(ADDR);
                    const auto  LW   = live.find(ADDR);
                    if (IT == g_slots.end() || IT->second.serial != r.serial || LW == live.end())
                        continue;
                    SSlot&            s        = IT->second;
                    const std::string OLDGROUP = s.group;
                    const Identity    OLD      = s.id;
                    s.facts                    = r.w;
                    s.proc = r.p.pid ? std::format("proc {} {} argv0={} env={} hops={}{}", r.p.pid, r.p.readable ? "readable" : "unreadable", r.p.argv0.substr(0, 80), r.p.env.size(),
                                                   r.p.chain.size(), r.session.empty() ? "" : " session=" + r.session)
                                     : "no process";
                    s.id                       = std::move(r.id);
                    s.state                    = EState::Done;
                    s.group                    = groupKeyOf(s.id, s.facts);
                    if (s.id.resolver != "Fallback") {
                        g_byKey[s.id.key]               = s.id;
                        g_l2[{s.facts.cls, s.facts.pid}] = s.id;
                    }
                    if (OLDGROUP == s.group && OLD.displayName == s.id.displayName && OLD.icon.value == s.id.icon.value && OLD.placeable == s.id.placeable)
                        continue;
                    dirty = true;
                    // Late: the new app's "Always Open in", within 1.5 s of the map,
                    // when the old answer did not already carry it.
                    if (OLDGROUP != s.group && NOW - s.mappedAt <= LATE_APPLY && LW->second->m_isMapped)
                        Dock::onWindowOpen(LW->second);
                }
                if (dirty)
                    changed();
            } catch (...) {}
            return 0;
        }

        void titleChanged(const PHLWINDOW& W) {
            const auto IT = W ? g_slots.find(W.get()) : g_slots.end();
            if (IT == g_slots.end())
                return;
            SSlot& s      = IT->second;
            s.facts.title = W->m_title;
            // Only the answers the title made: a quickshell panel, X11Props, a
            // nameless Wine program named after its title.
            const bool FROMTITLE = s.facts.cls == "org.quickshell" || s.id.resolver == "X11Props" || (s.id.resolver == "Fallback" && s.id.displayName == W->m_title);
            if (!FROMTITLE)
                return;
            const std::string OLD = s.group;
            s.id                  = resolveFast(s.facts, Indexes::current(), view());
            s.group               = groupKeyOf(s.id, s.facts);
            s.state               = EState::Provisional;
            enqueue(W.get(), s);
            if (OLD != s.group)
                changed();
        }

        SSlot* slotOf(const PHLWINDOW& W) {
            if (!W)
                return nullptr;
            syncInventory();
            const auto IT = g_slots.find(W.get());
            if (IT != g_slots.end()) {
                // A class the compositor changed without telling (or before init).
                if (IT->second.facts.cls != W->m_class)
                    return &open(W);
                return &IT->second;
            }
            if (!g_live || !W->m_isMapped)
                return nullptr;
            return &open(W);
        }

        bool identityForKey(const std::string& key, AppSearch::SIdentityAnswer& out) {
            const auto IT = g_byKey.find(key);
            if (IT == g_byKey.end())
                return false;
            out.name      = IT->second.displayName;
            out.icon      = IT->second.icon.kind == Icon::Kind::Pixels ? std::string{} : IT->second.icon.value;
            out.desktopId = IT->second.desktopId;
            return true;
        }

        std::string iconText(const Icon& i) {
            switch (i.kind) {
                case Icon::Kind::None: return "-";
                case Icon::Kind::Theme: return "theme:" + i.value;
                case Icon::Kind::File: return "file:" + i.value;
                case Icon::Kind::Pixels: return std::format("pixels:{}px", i.size);
            }
            return "-";
        }
    }

    void init() {
        if (g_live)
            return;
        g_clkTck = std::max(1L, sysconf(_SC_CLK_TCK));
        syncInventory();
        Engine::Options o{.src = &liveSources(), .gather = liveGatherer(liveSources())};
        o.record = &GameApps::record;
        if (const char* D = getenv("HYPRDRIVE_IDENTITY_DELAY_MS"); D && *D)
            o.jobDelay = std::chrono::milliseconds(std::clamp(atoi(D), 0, 60000));
        if (g_pCompositor && g_pCompositor->m_wlEventLoop) {
            g_wakeFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
            if (g_wakeFd >= 0)
                g_wakeSource = wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, g_wakeFd, WL_EVENT_READABLE, onWake, nullptr);
        }
        const int FD = g_wakeFd;
        o.wake       = [FD] {
            if (FD >= 0) {
                uint64_t one = 1;
                (void)::write(FD, &one, sizeof(one));
            }
        };
        g_engine = std::make_unique<Engine>(std::move(o));
        g_engine->setInventory(g_inv);
        g_engine->start();
        AppSearch::identityForKey = &identityForKey;
        g_live                    = true;

        g_openEarly = Event::bus()->m_events.window.openEarly.listen([](PHLWINDOW w) {
            if (w)
                open(w);
        });
        g_close = Event::bus()->m_events.window.close.listen([](PHLWINDOW w) {
            if (w)
                forget(w.get());
        });
        g_class = Event::bus()->m_events.window.class_.listen([](PHLWINDOW w) {
            if (w && g_slots.contains(w.get())) {
                open(w);
                changed();
            }
        });
        g_title = Event::bus()->m_events.window.title.listen([](PHLWINDOW w) { titleChanged(w); });
        for (const auto& W : Desktop::windowState()->windows())
            if (W && W->m_isMapped)
                open(W);
    }

    void shutdown() {
        g_openEarly.reset();
        g_close.reset();
        g_class.reset();
        g_title.reset();
        AppSearch::identityForKey = nullptr;
        g_live                    = false;
        // The worker first (it writes to the eventfd), then the fd.
        bool joined = true;
        if (g_engine)
            joined = g_engine->stop();
        if (joined)
            g_engine.reset();
        else
            (void)g_engine.release(); // abandoned: its state stays with its thread (Workers.hpp)
        if (g_wakeSource)
            wl_event_source_remove(g_wakeSource);
        g_wakeSource = nullptr;
        if (g_wakeFd >= 0 && joined)
            ::close(g_wakeFd);
        g_wakeFd = -1;
        g_slots.clear();
        g_l2.clear();
        g_byKey.clear();
    }

    const Identity& of(const PHLWINDOW& w) {
        if (SSlot* s = slotOf(w))
            return s->id;
        g_none = Identity{};
        if (w) {
            g_none.key = g_none.displayName = w->m_class;
            g_none.resolver                 = "Fallback";
        }
        return g_none;
    }

    std::string groupKey(const PHLWINDOW& w) {
        if (!w)
            return "";
        if (SSlot* s = slotOf(w))
            return s->group;
        return AppSearch::identityClass(w->m_class, w->m_title);
    }

    const Identity* forKey(std::string_view key) {
        if (key.starts_with("hd:"))
            key.remove_prefix(3);
        const auto IT = g_byKey.find(std::string{key});
        return IT == g_byKey.end() ? nullptr : &IT->second;
    }

    std::string canonical(std::string_view storedId) {
        syncInventory();
        std::string id{storedId};
        if (const auto IT = g_canon.find(id); IT != g_canon.end())
            return IT->second;
        if (g_canon.size() > 4096)
            g_canon.clear();
        return g_canon[id] = canonicalKey(storedId, view());
    }

    bool sameApp(std::string_view a, std::string_view b) {
        if (a.empty() || b.empty())
            return false;
        return a == b || canonical(a) == canonical(b);
    }

    bool placeable(const PHLWINDOW& w) {
        const SSlot* S = slotOf(w);
        return !S || S->id.placeable;
    }

    uint64_t generation() {
        return g_generation;
    }

    std::string liveReport() {
        const Indexes IX = Indexes::current();
        const auto    ST = g_engine ? g_engine->stats() : Engine::Stats{};
        std::string   r  = std::format("hyprdrive identity: generation {}; worker {} (queued {}, resolved {}, index builds {}, probes failed {}); {} inventory entries; steam index {}; lutris index {}",
                                       g_generation, g_engine && g_engine->running() ? "running" : "stopped", ST.queued, ST.resolved, ST.rebuilds, ST.probesFailed, g_inv->size(),
                                       IX.steam ? std::format("{} apps, {} shortcuts{}", IX.steam->apps.size(), IX.steam->shortcuts.size(),
                                                              IX.steam->skipped.empty() ? "" : std::format(", {} libraries skipped", IX.steam->skipped.size()))
                                                : "not built yet",
                                       IX.lutris ? std::format("{} games", IX.lutris->games.size()) : "not built yet");
        size_t n = 0;
        for (const auto& W : Desktop::windowState()->windows()) {
            if (!W || !W->m_isMapped)
                continue;
            const SSlot* S = slotOf(W);
            if (!S)
                continue;
            const auto& F = S->facts;
            const auto& I = S->id;
            r += std::format("\n  0x{:x} {} class={} instance={} title=\"{}\" pid={}{}{} -> key={} name=\"{}\" icon={} family={} conf={} by={} group={} state={}{}{} | {}",
                             reinterpret_cast<uintptr_t>(W.get()), F.x11 ? "x11" : "wl", F.cls, F.instance.empty() ? "-" : F.instance, F.title.substr(0, 60), F.pid,
                             F.xid ? std::format(" xid=0x{:x}", F.xid) : "", F.overrideRedirect ? " override-redirect" : "", I.key, I.displayName, iconText(I.icon), familyName(I.family),
                             I.confidence, I.resolver, S->group, S->state == EState::Done ? "resolved" : S->state == EState::Pending ? "pending" : "provisional",
                             I.isGame ? " game" : "", I.placeable ? "" : " NOT-PLACEABLE", I.evidence);
            if (!S->proc.empty())
                r += "\n      " + S->proc;
            ++n;
        }
        r += std::format("\n  {} windows", n);
        return r;
    }
}
