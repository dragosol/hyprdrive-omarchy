#include "AppIdentity.hpp"
#include "Resolvers.hpp"

#include <format>

namespace Hyprdrive::AppIdentity {
    std::string_view familyName(Family f) {
        switch (f) {
            case Family::Native: return "native";
            case Family::Steam: return "steam";
            case Family::SteamShortcut: return "steam-shortcut";
            case Family::Proton: return "proton";
            case Family::Lutris: return "lutris";
            case Family::Wine: return "wine";
            case Family::Unknown: break;
        }
        return "unknown";
    }

    Indexes Indexes::current() {
        return {.steam = SteamLibrary::current(), .lutris = LutrisLibrary::current()};
    }

    namespace {
        bool oldIsGameClass(const WindowFacts& w) {
            for (const std::string& C : {w.cls, w.initialClass}) {
                const std::string L = lower(C);
                if (L.starts_with("steam_app_") || L == "gamescope" || (w.x11 && L.size() > 4 && L.ends_with(".exe")))
                    return true;
            }
            return false;
        }

        // The minimal answer, for when anything below fails: today's.
        Identity plainFallback(const WindowFacts& w, std::string why) {
            Identity i;
            i.key         = w.cls;
            i.displayName = w.cls;
            i.icon        = Icon::of(lower(w.cls));
            i.resolver    = "Fallback";
            i.evidence    = std::move(why);
            i.isGame      = w.contentGame || oldIsGameClass(w);
            return i;
        }

        Identity run(const WindowFacts& w, const ProcessFacts& p, const Indexes& ix, const InventoryView& inv, const Sources* io, bool fastOnly) {
            try {
                const ProcessFacts EMPTY;
                const auto         C = Resolvers::makeCtx(w, fastOnly ? EMPTY : p, ix, inv, fastOnly ? nullptr : io, EMPTY);
                std::optional<Identity> win;
                Identity                fill; // what lower hits offer: name, icon, entry
                bool                    placeable = true;
                std::string             notes     = C.pidNote;
                const auto              note      = [&](const std::string& s) {
                    if (!s.empty())
                        notes += (notes.empty() ? "" : "; ") + s;
                };
                for (const auto& R : Resolvers::table()) {
                    if (fastOnly && !R.fast)
                        continue;
                    if (R.name == "Fallback")
                        break;
                    auto hit = R.run(C);
                    if (!hit)
                        continue;
                    if (!hit->placeable) {
                        placeable = false;
                        if (hit->confidence < Resolvers::WIN)
                            note(hit->evidence);
                    }
                    if (hit->confidence >= Resolvers::WIN) {
                        win = std::move(hit);
                        break;
                    }
                    if (fill.displayName.empty())
                        fill.displayName = hit->displayName;
                    if (fill.icon.empty())
                        fill.icon = hit->icon;
                    if (fill.desktopId.empty() && !hit->desktopId.empty()) {
                        fill.desktopId = hit->desktopId;
                        fill.launch    = hit->launch;
                        fill.family    = hit->family;
                    }
                    if (hit->resolver != "WineSystrayFallback" && hit->resolver != "WinePopup")
                        note(std::format("{} offers {}", hit->resolver, hit->evidence));
                }
                // Nothing strong, but a Wine window in a session whose app is
                // known: that app (a nameless tool window of a running game).
                if (!win && C.wine && C.p.readable && inv.sessionOwner)
                    if (const std::string K = sessionKey(C.p); !K.empty())
                        if (const Identity* O = inv.sessionOwner(K)) {
                            win             = *O;
                            win->resolver   = "HelperOfSession";
                            win->confidence = 50;
                            win->evidence   = std::format("weak, so the session {} -> {}", K, O->key);
                        }
                Identity out;
                if (win) {
                    out = std::move(*win);
                    if (out.displayName.empty())
                        out.displayName = fill.displayName;
                    if (out.icon.empty())
                        out.icon = fill.icon;
                } else {
                    // Today's answer: the class, unless an entry was found on the way.
                    for (const auto& R : Resolvers::table())
                        if (R.name == "Fallback")
                            out = *R.run(C);
                    if (!fill.desktopId.empty()) {
                        out.key       = fill.desktopId;
                        out.desktopId = fill.desktopId;
                        out.launch    = fill.launch;
                        out.family    = fill.family == Family::Unknown ? Family::Native : fill.family;
                    }
                    if (!fill.displayName.empty())
                        out.displayName = fill.displayName;
                    if (!fill.icon.empty())
                        out.icon = fill.icon;
                }
                if (out.displayName.empty())
                    out.displayName = w.cls;
                out.placeable = out.placeable && placeable;
                if (out.exePath.empty())
                    out.exePath = C.exePath;
                if (out.prefix.empty())
                    out.prefix = C.prefix;
                out.isGame    = out.isGame || w.contentGame || out.family == Family::Steam || out.family == Family::SteamShortcut || out.family == Family::Lutris ||
                    out.family == Family::Proton || oldIsGameClass(w);
                if (!notes.empty())
                    out.evidence += " [" + notes + "]";
                return out;
            } catch (...) { return plainFallback(w, "class " + w.cls + " [resolver failed]"); }
        }
    }

    Identity resolveWith(const WindowFacts& w, const ProcessFacts& p, const Indexes& ix, const InventoryView& inv, const Sources* io) {
        return run(w, p, ix, inv, io, false);
    }

    Identity resolveFast(const WindowFacts& w, const Indexes& ix, const InventoryView& inv) {
        return run(w, ProcessFacts{}, ix, inv, nullptr, true);
    }

    Identity resolve(const WindowFacts& w, const ProcessFacts& p, const Sources& src, const InventoryView& inv) {
        try {
            const Indexes IX{.steam = SteamLibrary::rebuild(src), .lutris = LutrisLibrary::rebuild(src)};
            return resolveWith(w, p, IX, inv, &src);
        } catch (...) { return plainFallback(w, "class " + w.cls + " [index failed]"); }
    }

    std::string canonicalKey(std::string_view stored, const InventoryView& inv) {
        try {
            // A dock group key is its identity's key.
            if (stored.starts_with("hd:"))
                stored.remove_prefix(3);
            for (const std::string_view P : {"steam-app-", "steam-shortcut-", "net.lutris.", "hyprdrive-wine-"})
                if (stored.starts_with(P))
                    return std::string{stored};
            for (const auto& E : inv.entries)
                if (E.id == stored) {
                    if (const auto ID = Steam::fromExec(E.exec))
                        return Steam::keyOf(*ID);
                    break;
                }
            return std::string{stored};
        } catch (...) { return std::string{stored}; }
    }

    bool sameApp(std::string_view a, std::string_view b, const InventoryView& inv) {
        if (a == b)
            return !a.empty();
        if (a.empty() || b.empty())
            return false;
        return canonicalKey(a, inv) == canonicalKey(b, inv);
    }

    std::string groupKeyOf(const Identity& id, const WindowFacts& w) {
        // Today's key -- AppSearch::identityClass -- wherever the answer is today's.
        const bool TODAY = id.resolver == "Fallback" || id.resolver == "DesktopLoose" || (id.resolver == "DesktopStrict" && id.family == Family::Native);
        if (TODAY)
            return w.cls == "org.quickshell" && !w.title.empty() ? "org.quickshell:" + w.title : w.cls;
        return "hd:" + id.key;
    }

    std::string sessionKey(const ProcessFacts& p) {
        if (!p.readable)
            return "";
        for (const char* K : {"SteamGameId", "SteamAppId"})
            if (const auto V = p.getenv(K); Steam::parseGameId(V))
                return "steam:" + std::string{V};
        if (const auto U = p.getenv("LUTRIS_GAME_UUID"); !U.empty())
            return "lutris:" + std::string{U};
        if (auto pre = std::string{p.getenv("WINEPREFIX")}; !pre.empty()) {
            while (pre.size() > 1 && pre.back() == '/')
                pre.pop_back();
            return std::format("wine:{}|{}", pre, p.chain.empty() ? 0 : p.chain.back().pid);
        }
        return "";
    }
}
