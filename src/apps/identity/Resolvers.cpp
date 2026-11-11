#include "Resolvers.hpp"

#include "WinExe.hpp"

#include <array>
#include <format>

namespace Hyprdrive::AppIdentity::Resolvers {
    namespace {
        // ---- data ---------------------------------------------------------------
        // A helper process (AppIdentity design, (b)): its window belongs to the
        // app of its session, never to an app of its own.
        constexpr std::array<std::string_view, 13> HELPERS = {"explorer.exe",       "conhost.exe",        "steam.exe",          "start.exe",
                                                              "winedevice.exe",     "services.exe",       "crashreporter*.exe", "unitycrashhandler*.exe",
                                                              "crashpad_handler*",  "easyanticheat*.exe", "igoproxy*.exe",      "*overlay*.exe",
                                                              "steamwebhelper*"};
        // Classes several apps share, so a desktop entry cannot claim them by
        // class: every umu game is steam_app_default, every prefix has an
        // explorer.exe. (Non-numeric steam_app_* and the helpers are added in
        // isAmbiguousClass.)
        constexpr std::array<std::string_view, 3> AMBIGUOUS = {"steam_app_default", "steam_app_0", "explorer.exe"};

        constexpr uint8_t FILL = 40; // a hit that only fills in what the winner lacks

        bool endsWithExe(std::string_view s) {
            return s.size() > 4 && iequals(s.substr(s.size() - 4), ".exe");
        }
        bool isWindowsPath(std::string_view s) {
            return s.size() >= 3 && std::isalpha(static_cast<unsigned char>(s[0])) && s[1] == ':' && (s[2] == '\\' || s[2] == '/');
        }
        std::string trimSlash(std::string s) {
            while (s.size() > 1 && s.back() == '/')
                s.pop_back();
            return s;
        }
        bool under(std::string_view path, std::string_view folder) {
            if (folder.empty() || path.size() <= folder.size())
                return false;
            return iequals(path.substr(0, folder.size()), folder) && (path[folder.size()] == '/' || folder.back() == '/');
        }
        bool shortTitle(std::string_view t) {
            return !t.empty() && t.size() <= 40 && t.find(" - ") == std::string_view::npos;
        }
        std::string stem(std::string_view exe) {
            std::string_view b = baseName(exe);
            if (endsWithExe(b))
                b.remove_suffix(4);
            return std::string{b};
        }
        bool procOk(const Ctx& c) {
            return c.p.readable;
        }

        Identity hitOf(std::string_view resolver, uint8_t conf, std::string evidence) {
            Identity i;
            i.resolver   = std::string{resolver};
            i.confidence = conf;
            i.evidence   = std::move(evidence);
            return i;
        }

        const InventoryEntry* entryById(const InventoryView& inv, std::string_view id) {
            for (const auto& E : inv.entries)
                if (E.id == id)
                    return &E;
            return nullptr;
        }

        // ---- 0: placeability -----------------------------------------------------
        std::optional<Identity> hyprdriveOwn(const Ctx& c) {
            if (!c.w.hyprdriveOwn)
                return std::nullopt;
            Identity i    = hitOf("HyprdriveOwn", 100, "a Hyprdrive surface");
            i.key         = "hyprdrive-own";
            i.displayName = "Hyprdrive";
            i.family      = Family::Native;
            i.placeable   = false;
            return i;
        }

        bool wineish(const Ctx& c) {
            return procOk(c) ? c.wine : true; // unknown: the window's own shape decides
        }

        std::optional<Identity> wineSystrayFallback(const Ctx& c) {
            // Wine's own systray window when no XEmbed tray owns
            // _NET_SYSTEM_TRAY_S0: explorer.exe, no title, small and fixed.
            const bool SMALL = c.w.fixedSize || (c.w.w > 0 && c.w.h > 0 && c.w.w <= 128 && c.w.h <= 128);
            if (!c.w.x11 || c.cls != "explorer.exe" || !c.w.title.empty() || c.w.overrideRedirect || !SMALL || !wineish(c))
                return std::nullopt;
            Identity i  = hitOf("WineSystrayFallback", 0, std::format("wine systray fallback (explorer.exe, no title, {}x{}{})", c.w.w, c.w.h, procOk(c) ? ", wine process" : ""));
            i.placeable = false;
            return i;
        }

        std::optional<Identity> winePopup(const Ctx& c) {
            if (!c.w.x11 || !c.w.overrideRedirect || !c.wine)
                return std::nullopt;
            Identity i  = hitOf("WinePopup", 0, "wine popup (override-redirect)");
            i.placeable = false;
            return i;
        }

        // ---- 1: the desktop entry, strictly ------------------------------------------
        std::optional<Identity> desktopStrict(const Ctx& c) {
            if (c.want.empty() || (isAmbiguousClass(c.cls) && !c.cls.starts_with("org.quickshell")))
                return std::nullopt;
            for (const auto& E : c.inv.entries) {
                const std::string ID   = lower(E.id);
                const auto        DOT  = ID.find_last_of('.');
                const std::string TAIL = DOT == std::string::npos ? ID : ID.substr(DOT + 1);
                uint8_t           conf = 0;
                std::string       how;
                if (!E.wmClass.empty() && lower(E.wmClass) == c.want)
                    conf = 95, how = "StartupWMClass";
                else if (ID == c.want)
                    conf = 90, how = "desktop id";
                else if (TAIL == c.want)
                    conf = 85, how = "desktop id tail";
                else if (lower(E.name) == c.want)
                    conf = 80, how = "Name";
                else if (!E.chromeStem.empty() && c.want.starts_with(E.chromeStem) && (c.want.size() == E.chromeStem.size() || c.want[E.chromeStem.size()] == '-'))
                    conf = 90, how = "web app class";
                if (!conf)
                    continue;
                // A Wine entry (winemenubuilder's) speaks for its own prefix only:
                // launcher.exe of prefix A is not launcher.exe of prefix B.
                Family     fam = Family::Native;
                const auto EP  = execWinePrefix(E.exec);
                if (!EP.empty()) {
                    fam = Family::Wine;
                    if (!c.prefix.empty() && EP != c.prefix)
                        continue;
                    if (c.prefix.empty()) {
                        conf = 55; // the window's prefix is not known yet: a fill, not an answer
                        how += ", prefix unverified";
                    } else
                        how += ", WINEPREFIX matches";
                }
                Identity i    = hitOf("DesktopStrict", conf, std::format("{} {} -> {}.desktop", how, c.want, E.id));
                i.key         = E.id;
                i.desktopId   = E.id;
                i.displayName = E.name;
                i.icon        = Icon::of(E.icon);
                i.launch      = E.exec;
                i.family      = fam;
                return i;
            }
            return std::nullopt;
        }

        // ---- 2: Steam ---------------------------------------------------------------
        std::optional<Identity> lutrisById(const Ctx& c, int64_t id, std::string evidence);

        std::optional<Identity> steamApp(const Ctx& c) {
            std::optional<Steam::GameId> id;
            std::string                  why;
            uint8_t                      conf = 0;
            if (procOk(c)) {
                for (const char* K : {"SteamGameId", "SteamAppId"})
                    if (!id)
                        if (const auto V = c.p.getenv(K); (id = Steam::parseGameId(V)))
                            why = std::format("env {}={}", K, V), conf = 95;
                if (const auto CD = c.p.getenv("STEAM_COMPAT_DATA_PATH"); !id && !CD.empty())
                    if ((id = Steam::parseGameId(baseName(trimSlash(std::string{CD})))))
                        why = std::format("STEAM_COMPAT_DATA_PATH {}", CD), conf = 90;
                for (const auto& H : c.p.chain) {
                    if (id)
                        break;
                    const auto AT = H.cmdline.find("AppId=");
                    if (AT == std::string::npos || (H.comm != "reaper" && H.cmdline.find("SteamLaunch") == std::string::npos))
                        continue;
                    size_t end = AT + 6;
                    while (end < H.cmdline.size() && std::isdigit(static_cast<unsigned char>(H.cmdline[end])))
                        ++end;
                    if ((id = Steam::parseGameId(std::string_view{H.cmdline}.substr(AT + 6, end - AT - 6))))
                        why = std::format("{} (pid {}) AppId={}", H.comm, H.pid, id->id), conf = 90;
                }
            }
            if (!id && (id = Steam::fromClass(c.cls)))
                why = "class " + c.cls, conf = 85;
            if (!id && c.w.steamGameProp && (id = Steam::parseGameId(*c.w.steamGameProp)))
                why = "STEAM_GAME " + *c.w.steamGameProp, conf = 85;
            if (!id)
                return std::nullopt;

            const auto* SNAP = c.ix.steam.get();
            if (id->kind == Steam::GameId::Kind::Shortcut && SNAP)
                if (const auto* S = SNAP->shortcut(id->id)) {
                    // A shortcut that runs Lutris: Lutris's game, not Steam's stub.
                    const auto AT = S->launchOptions.find("lutris:rungameid/");
                    if (lower(baseName(S->exe)) == "lutris" && AT != std::string::npos) {
                        const std::string_view REST = std::string_view{S->launchOptions}.substr(AT + 17);
                        size_t                 n    = 0;
                        while (n < REST.size() && n < 12 && std::isdigit(static_cast<unsigned char>(REST[n])))
                            ++n;
                        if (n)
                            if (auto l = lutrisById(c, std::stoll(std::string{REST.substr(0, n)}), std::format("{} -> steam shortcut {} runs lutris:rungameid/{}", why, id->id, REST.substr(0, n))))
                                return l;
                    }
                }

            // The desktop shortcut that launches it (newest file), whose Exec
            // names the app id -- or, for a shortcut, the 64-bit gameid.
            const std::string DIGITS = id->kind == Steam::GameId::Kind::App ? std::to_string(id->id) : std::to_string((uint64_t{id->id} << 32) | 0x02000000u);
            const size_t      AT     = SteamLibrary::newestShortcut(
                c.inv.entries.size(), DIGITS, [&](size_t i) { return std::string_view{c.inv.entries[i].exec}; }, [&](size_t i) { return c.inv.entries[i].mtimeNs; });
            const InventoryEntry* E = AT == std::string::npos ? nullptr : &c.inv.entries[AT];

            Identity i = hitOf("SteamApp", conf, why);
            i.key      = Steam::keyOf(*id);
            i.family   = id->kind == Steam::GameId::Kind::App ? Family::Steam : Family::SteamShortcut;
            i.isGame   = true;
            if (E) {
                i.displayName = E->name;
                i.desktopId   = E->id;
                i.launch      = E->exec;
                i.evidence += " -> " + E->id + ".desktop";
            } else if (SNAP && !SNAP->nameFor(*id).empty()) {
                i.displayName = SNAP->nameFor(*id);
                i.evidence += id->kind == Steam::GameId::Kind::App ? " -> appmanifest" : " -> shortcuts.vdf";
            } else
                i.evidence += SNAP ? " -> not in the Steam index" : " -> no Steam index yet";
            if (i.launch.empty())
                i.launch = "steam steam://rungameid/" + DIGITS;
            if (const std::string ICON = SNAP ? SNAP->iconFor(*id) : std::string{}; !ICON.empty())
                i.icon = Icon::of(ICON);
            else if (E && !E->icon.empty())
                i.icon = Icon::of(E->icon);
            else if (id->kind == Steam::GameId::Kind::App)
                i.icon = Icon::of("steam_icon_" + std::to_string(id->id));
            return i;
        }

        // ---- 3: Lutris -----------------------------------------------------------------
        Identity lutrisIdentity(const Ctx& c, const LutrisLibrary::Game& g, uint8_t conf, std::string evidence) {
            Identity i    = hitOf("LutrisGame", conf, std::move(evidence));
            i.key         = g.appId;
            i.displayName = g.name;
            i.icon        = Icon::of(g.icon);
            i.family      = Family::Lutris;
            i.isGame      = true;
            i.launch      = std::string(g.flatpak ? "flatpak run net.lutris.Lutris" : "env LUTRIS_SKIP_INIT=1 lutris") + " lutris:rungameid/" + std::to_string(g.id);
            if (entryById(c.inv, g.appId))
                i.desktopId = g.appId;
            return i;
        }

        std::optional<Identity> lutrisById(const Ctx& c, int64_t id, std::string evidence) {
            const auto* L = c.ix.lutris.get();
            const auto* G = L ? L->byId(id) : nullptr;
            if (!G)
                return std::nullopt;
            return lutrisIdentity(c, *G, 92, std::move(evidence));
        }

        std::optional<Identity> lutrisGame(const Ctx& c) {
            const auto* L = c.ix.lutris.get();
            if (!procOk(c) || !L)
                return std::nullopt;
            const std::string_view NAME = c.p.getenv("GAME_NAME");
            const std::string      EXE  = !c.exePath.empty() ? c.exePath : c.exe;
            // The window's program is a Lutris game itself (a launcher's child
            // that Lutris also knows, or one started outside Lutris): that game.
            // Its own app, whatever GAME_NAME it inherited.
            const auto* OWN = c.exePath.empty() ? nullptr : L->byExe(c.exePath);
            if (OWN && !iequals(OWN->exe, c.exePath))
                OWN = nullptr; // a basename match alone is not enough here
            const auto* G = NAME.empty() ? nullptr : L->byName(NAME);
            if (OWN && OWN != G)
                return lutrisIdentity(c, *OWN, 85, std::format("exe {} is the Lutris game {}{}", c.exePath, OWN->name, G ? std::format(" (GAME_NAME={})", NAME) : ""));
            if (!G)
                return std::nullopt;
            const std::string UUID = std::string{c.p.getenv("LUTRIS_GAME_UUID")};
            const std::string TAG  = std::format("env GAME_NAME={}{}", NAME, UUID.empty() ? "" : " LUTRIS_GAME_UUID=" + UUID);
            if (!G->exe.empty()) {
                if (under(EXE, G->folder))
                    return lutrisIdentity(c, *G, 90, TAG + ", exe under " + G->folder);
                if (!EXE.empty() && iequals(baseName(EXE), baseName(G->exe)))
                    return lutrisIdentity(c, *G, 88, TAG + ", same exe name " + std::string{baseName(G->exe)});
                return std::nullopt; // something the game started: its own app
            }
            // The game's exe is unknown (no yml has one): ask more of the window.
            if (isHelper(c.exe))
                return std::nullopt;
            if (L->byExe(EXE))
                return std::nullopt; // another Lutris game's program
            const std::string DIR = trimSlash(std::string{c.p.getenv("GAME_DIRECTORY")});
            if (!DIR.empty() && !c.exePath.empty() && !under(c.exePath, DIR))
                return std::nullopt;
            return lutrisIdentity(c, *G, 70, TAG + std::format(", exe unknown (yml: {}){}", G->ymlFrom.empty() ? "none" : G->ymlFrom, DIR.empty() ? "" : ", under GAME_DIRECTORY"));
        }

        // ---- 4: helpers --------------------------------------------------------------------
        std::optional<Identity> helperOfSession(const Ctx& c) {
            if (!procOk(c) || !c.inv.sessionOwner || !(isHelper(c.exe) || isHelper(c.cls)))
                return std::nullopt;
            const std::string KEY = sessionKey(c.p);
            const Identity*   O   = KEY.empty() ? nullptr : c.inv.sessionOwner(KEY);
            if (!O)
                return std::nullopt;
            Identity i   = *O;
            i.resolver   = "HelperOfSession";
            i.confidence = 80;
            i.evidence   = std::format("helper {} of session {} -> {}", c.exe.empty() ? c.cls : c.exe, KEY, O->key);
            return i;
        }

        // ---- 5: the Wine program itself -----------------------------------------------------
        std::optional<Identity> wineExe(const Ctx& c) {
            if (!procOk(c) || !c.wine || c.exePath.empty() || !endsWithExe(c.exePath) || isHelper(c.exe) || isWineSystemPath(lower(c.exePath)))
                return std::nullopt;
            const bool  PROTON = !c.p.getenv("PROTONPATH").empty() || !c.p.getenv("GAMEID").empty() || !c.p.getenv("STEAM_COMPAT_DATA_PATH").empty();
            std::string key    = "hyprdrive-wine-" + c.exe;
            std::string why    = "wine exe " + c.exePath;
            // The same key from another prefix or path (launcher.exe of two
            // games): this one gets @hash8 of its prefix and path.
            const InventoryEntry* E = entryById(c.inv, key);
            if (E && E->exec.find(c.exePath) == std::string::npos) {
                why += std::format(", {} is another program's", key);
                key += "@" + hash8(c.prefix + "|" + c.exePath);
                E = entryById(c.inv, key);
            }
            Identity i = hitOf("WineExe", 70, why);
            i.key      = key;
            i.family   = PROTON ? Family::Proton : Family::Wine;
            i.isGame   = true; // the old rule: any X11 .exe
            if (E) {           // remembered (wine-apps.tsv) or a desktop entry by that id
                i.desktopId   = E->id;
                i.displayName = E->name;
                i.icon        = Icon::of(E->icon);
                i.launch      = E->exec;
                i.confidence  = 75;
                i.evidence += " (known)";
                return i;
            }
            if (c.io) {
                const Sources& IO = *c.io;
                const auto     PE = WinExe::parse([&](uint64_t off, size_t n) { return IO.readRange(c.exePath, off, n); });
                if (PE.ok) {
                    if (auto n = WinExe::name(PE); !n.empty()) {
                        i.displayName = n;
                        i.confidence  = 75;
                        i.evidence += ", PE name";
                    }
                    const std::string ICONNAME = "wine-" + key.substr(15);
                    if (auto have = IO.storedIcon(ICONNAME); !have.empty())
                        i.icon = Icon::of(have);
                    else if (const auto IMG = WinExe::iconImage(PE))
                        if (auto stored = IO.storeIcon(ICONNAME, *IMG); !stored.empty()) {
                            i.icon = Icon::of(stored);
                            i.evidence += ", PE icon";
                        }
                }
            }
            if (i.icon.empty() && !c.w.netWmIcon.empty())
                i.icon = c.w.netWmIcon;
            if (i.displayName.empty())
                i.displayName = shortTitle(c.w.title) ? c.w.title : stem(c.exePath);
            if (i.icon.empty())
                i.icon = Icon::of("application-x-ms-dos-executable");
            return i;
        }

        // ---- 6: X11 properties ----------------------------------------------------------------
        std::optional<Identity> x11Props(const Ctx& c) {
            if (!c.w.x11)
                return std::nullopt;
            Identity i = hitOf("X11Props", FILL, "");
            if (!c.w.netWmIcon.empty()) {
                i.icon = c.w.netWmIcon;
                i.evidence += "_NET_WM_ICON";
            }
            if (endsWithExe(c.cls) && shortTitle(c.w.title)) {
                i.displayName = c.w.title;
                i.evidence += std::string(i.evidence.empty() ? "" : ", ") + "title";
            }
            if (i.evidence.empty())
                return std::nullopt;
            return i;
        }

        // ---- 7: the desktop entry, loosely (AppSearch's second pass) --------------------------
        std::optional<Identity> desktopLoose(const Ctx& c) {
            if (c.want.empty() || isAmbiguousClass(c.cls))
                return std::nullopt;
            std::vector<std::string> parts;
            for (size_t at = 0; at <= c.want.size();) {
                const size_t DOT = c.want.find('.', at);
                parts.emplace_back(c.want.substr(at, DOT == std::string::npos ? std::string::npos : DOT - at));
                at = DOT == std::string::npos ? c.want.size() + 1 : DOT + 1;
            }
            std::vector<std::string> cands;
            if (!parts.empty()) {
                std::string last = parts.back();
                for (const std::string_view SUFFIX : {"_app", "-app", "-desktop", "_desktop"})
                    if (last.size() > SUFFIX.size() + 2 && last.ends_with(SUFFIX))
                        last.resize(last.size() - SUFFIX.size());
                cands.push_back(last);
                for (size_t k = 1; k + 1 < parts.size(); ++k)
                    if (parts[k].size() >= 3)
                        cands.push_back(parts[k]);
            }
            for (const auto& C : cands)
                for (const auto& E : c.inv.entries)
                    if (lower(E.id) == C) {
                        Identity i    = hitOf("DesktopLoose", 60, std::format("class part {} -> {}.desktop", C, E.id));
                        i.key         = E.id;
                        i.desktopId   = E.id;
                        i.displayName = E.name;
                        i.icon        = Icon::of(E.icon);
                        i.launch      = E.exec;
                        i.family      = Family::Native;
                        return i;
                    }
            return std::nullopt;
        }

        // ---- 8: today's answer --------------------------------------------------------------
        std::optional<Identity> fallback(const Ctx& c) {
            Identity i    = hitOf("Fallback", 10, "class " + c.w.cls);
            i.key         = c.w.cls == "org.quickshell" && !c.w.title.empty() ? "org.quickshell:" + c.w.title : c.w.cls;
            i.displayName = c.w.cls;
            i.icon        = Icon::of(lower(c.w.cls));
            i.family      = Family::Unknown;
            return i;
        }

        constexpr Resolver TABLE[] = {
            {"HyprdriveOwn", true, &hyprdriveOwn},
            {"WineSystrayFallback", true, &wineSystrayFallback},
            {"WinePopup", true, &winePopup},
            {"DesktopStrict", true, &desktopStrict},
            {"SteamApp", true, &steamApp},
            {"LutrisGame", false, &lutrisGame},
            {"HelperOfSession", false, &helperOfSession},
            {"WineExe", false, &wineExe},
            {"X11Props", false, &x11Props},
            {"DesktopLoose", true, &desktopLoose},
            {"Fallback", true, &fallback},
        };
    }

    std::span<const Resolver> table() {
        return TABLE;
    }

    bool glob(std::string_view pat, std::string_view s) {
        // Iterative '*' matching, no backtracking blow-up.
        size_t p = 0, i = 0, star = std::string_view::npos, mark = 0;
        while (i < s.size()) {
            if (p < pat.size() && pat[p] == '*') {
                star = p++;
                mark = i;
            } else if (p < pat.size() && pat[p] == s[i]) {
                ++p;
                ++i;
            } else if (star != std::string_view::npos) {
                p = star + 1;
                i = ++mark;
            } else
                return false;
        }
        while (p < pat.size() && pat[p] == '*')
            ++p;
        return p == pat.size();
    }

    bool isHelper(std::string_view s) {
        if (s.empty())
            return false;
        const std::string L = lower(s);
        return std::ranges::any_of(HELPERS, [&](std::string_view h) { return glob(h, L); });
    }

    bool isAmbiguousClass(std::string_view cls) {
        const std::string L = lower(cls);
        if (std::ranges::find(AMBIGUOUS, std::string_view{L}) != AMBIGUOUS.end() || isHelper(L))
            return true;
        static constexpr std::string_view SA = "steam_app_";
        return L.starts_with(SA) && !Steam::parseGameId(std::string_view{L}.substr(SA.size()));
    }

    bool isWineSystemPath(std::string_view p) {
        return p.find("/drive_c/windows/") != std::string_view::npos || p.find("/appdata/local/temp/") != std::string_view::npos || p.find("/temp/") != std::string_view::npos ||
            p.starts_with("/tmp/");
    }

    std::string wineUnixPath(std::string_view win, std::string_view prefix) {
        if (!isWindowsPath(win) || prefix.empty())
            return "";
        const char  D    = static_cast<char>(std::tolower(static_cast<unsigned char>(win[0])));
        std::string rest = std::string{win.substr(2)};
        std::ranges::replace(rest, '\\', '/');
        const std::string P = trimSlash(std::string{prefix});
        if (D == 'c')
            return P + "/drive_c" + rest;
        if (D == 'z') // Wine's z: is /
            return rest;
        return P + "/dosdevices/" + std::string(1, D) + ":" + rest;
    }

    std::string execWinePrefix(std::string_view exec) {
        const auto AT = exec.find("WINEPREFIX=");
        if (AT == std::string_view::npos)
            return "";
        const bool   QUOTED = AT > 0 && exec[AT - 1] == '"';
        const size_t FROM   = AT + 11;
        size_t       end    = FROM;
        while (end < exec.size() && exec[end] != (QUOTED ? '"' : ' ') && !(!QUOTED && exec[end] == '"'))
            ++end;
        return trimSlash(std::string{exec.substr(FROM, end - FROM)});
    }

    std::string hash8(std::string_view s) {
        uint32_t h = 2166136261u;
        for (unsigned char c : s) {
            h ^= c;
            h *= 16777619u;
        }
        return std::format("{:08x}", h);
    }

    Ctx makeCtx(const WindowFacts& w, const ProcessFacts& p, const Indexes& ix, const InventoryView& inv, const Sources* io, const ProcessFacts& empty) {
        // A process that started after its window mapped is not its process.
        const bool         STALE = p.readable && w.mapTicks && p.startTicks && p.startTicks > w.mapTicks;
        const ProcessFacts& P    = p.readable && !STALE ? p : empty;
        Ctx                 c{.w = w, .p = P, .ix = ix, .inv = inv, .io = io};
        if (STALE)
            c.pidNote = std::format("pid {} started after the window mapped: its facts are not this window's", p.pid);
        c.cls  = lower(w.cls);
        c.want = c.cls == "org.quickshell" && !w.title.empty() ? lower(w.title) : c.cls;
        if (P.readable) {
            c.prefix = trimSlash(std::string{P.getenv("WINEPREFIX")});
            if (const auto CD = P.getenv("STEAM_COMPAT_DATA_PATH"); c.prefix.empty() && !CD.empty())
                c.prefix = trimSlash(std::string{CD}) + "/pfx";
            const bool WINARGV = isWindowsPath(P.argv0);
            if (WINARGV) {
                c.exe     = lower(baseName(P.argv0));
                c.exePath = wineUnixPath(P.argv0, !c.prefix.empty() ? c.prefix : (io ? io->env().home + "/.wine" : std::string{}));
            } else if (P.argv0.starts_with('/') && endsWithExe(P.argv0)) {
                c.exe     = lower(baseName(P.argv0));
                c.exePath = P.argv0;
            } else
                c.exe = lower(baseName(!P.argv0.empty() ? P.argv0 : P.exe));
            // Known process: Wine says so itself (a prefix, a C:\ argv0, its loader).
            c.wine = !std::string_view{P.getenv("WINEPREFIX")}.empty() || WINARGV || P.exe.find("wine") != std::string::npos || P.comm.starts_with("wine");
        } else {
            if (endsWithExe(c.cls))
                c.exe = c.cls;
            c.wine = w.x11 && endsWithExe(c.cls);
        }
        return c;
    }
}
