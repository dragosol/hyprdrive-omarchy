#pragma once

// Steam's files, read through Vdf.hpp: an appmanifest_<id>.acf (name and
// install dir), libraryfolders.vdf (where the other libraries are) and the
// binary userdata/<uid>/config/shortcuts.vdf (non-Steam games). Plus the ids
// that tie a window to them: the class `steam_app_<digits>` and the
// `steam://rungameid/<digits>` of a desktop shortcut.
//
// A non-Steam shortcut's id is 32-bit with the high bit set (its compatdata
// folder is named after it); Steam hands the game the 64-bit form
// (id32 << 32) | 0x02000000 as SteamGameId. So digits >= 2^32 are a shortcut
// whose id32 is the top half, digits in [2^31, 2^32) are already an id32, and
// anything smaller is a Steam app id.

#include "Vdf.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Hyprdrive::AppIdentity::Steam {
    struct Manifest {
        uint32_t    appid = 0;
        std::string name, installdir;
    };
    inline std::optional<Manifest> parseManifest(Bytes in) {
        const auto DOC = Vdf::parseText(in, Caps::VDF);
        if (!DOC)
            return std::nullopt;
        const Vdf::Node* S = DOC->child("AppState");
        if (!S)
            return std::nullopt;
        const auto ID = S->number("appid");
        if (!ID || !*ID || *ID > 0xFFFFFFFFull)
            return std::nullopt;
        Manifest m{.appid = static_cast<uint32_t>(*ID), .name = std::string{S->value("name")}, .installdir = std::string{S->value("installdir")}};
        if (m.name.empty())
            return std::nullopt;
        return m;
    }

    // The library paths, both layouts: `"0" { "path" "/x" }` (current) and
    // `"1" "/x"` (before 2021).
    inline std::vector<std::string> parseLibraryFolders(Bytes in) {
        std::vector<std::string> out;
        const auto               DOC = Vdf::parseText(in, Caps::VDF);
        if (!DOC)
            return out;
        const Vdf::Node* L = DOC->child("libraryfolders");
        if (!L)
            return out;
        for (const auto& C : L->children) {
            if (!allDigits(C.key))
                continue;
            const std::string_view P = C.type == Vdf::Node::Type::String ? std::string_view{C.str} : C.value("path");
            if (P.starts_with('/'))
                out.emplace_back(P);
        }
        return out;
    }

    struct ShortcutEntry {
        uint32_t    appid = 0;
        std::string name, exe, startDir, icon, launchOptions;
    };
    inline std::vector<ShortcutEntry> parseShortcuts(Bytes in) {
        std::vector<ShortcutEntry> out;
        const auto                 DOC = Vdf::parseBinary(in, Caps::SHORTCUTS);
        if (!DOC)
            return out;
        const Vdf::Node* S = DOC->child("shortcuts");
        if (!S)
            return out;
        for (const auto& E : S->children) {
            if (E.type != Vdf::Node::Type::Map)
                continue;
            const auto ID = E.number("appid");
            if (!ID || !*ID || *ID > 0xFFFFFFFFull)
                continue;
            ShortcutEntry s{.appid = static_cast<uint32_t>(*ID)};
            s.name          = std::string{E.value("AppName")};
            s.exe           = std::string{E.value("Exe")};
            s.startDir      = std::string{E.value("StartDir")};
            s.icon          = std::string{E.value("icon")};
            s.launchOptions = std::string{E.value("LaunchOptions")};
            // Steam quotes the paths it writes ("\"/usr/bin/lutris\"").
            for (std::string* P : {&s.exe, &s.startDir, &s.icon})
                if (P->size() >= 2 && P->front() == '"' && P->back() == '"')
                    *P = P->substr(1, P->size() - 2);
            out.push_back(std::move(s));
        }
        return out;
    }

    struct GameId {
        enum class Kind : uint8_t { App, Shortcut };
        Kind     kind = Kind::App;
        uint32_t id   = 0;
        bool     operator==(const GameId&) const = default;
    };
    inline std::optional<GameId> parseGameId(std::string_view digits) {
        if (!allDigits(digits) || digits.size() > 20)
            return std::nullopt;
        uint64_t v = 0;
        for (char c : digits) {
            const uint64_t D = static_cast<uint64_t>(c - '0');
            if (v > (UINT64_MAX - D) / 10)
                return std::nullopt;
            v = v * 10 + D;
        }
        if (v == 0)
            return std::nullopt;
        if (v >= (1ull << 32))
            return GameId{GameId::Kind::Shortcut, static_cast<uint32_t>(v >> 32)};
        if (v >= (1ull << 31))
            return GameId{GameId::Kind::Shortcut, static_cast<uint32_t>(v)};
        return GameId{GameId::Kind::App, static_cast<uint32_t>(v)};
    }
    // `steam_app_<digits>` (any case); not steam_app_default, steam_app_0.
    inline std::optional<GameId> fromClass(std::string_view cls) {
        static constexpr std::string_view PREFIX = "steam_app_";
        if (cls.size() <= PREFIX.size() || !iequals(cls.substr(0, PREFIX.size()), PREFIX))
            return std::nullopt;
        return parseGameId(cls.substr(PREFIX.size()));
    }
    // The digits after `steam_app_`, as written ("" if not all digits) --
    // what the desktop shortcuts' rungameid/ is matched against.
    inline std::string classDigits(std::string_view cls) {
        static constexpr std::string_view PREFIX = "steam_app_";
        if (cls.size() <= PREFIX.size() || !iequals(cls.substr(0, PREFIX.size()), PREFIX) || !allDigits(cls.substr(PREFIX.size())))
            return {};
        return std::string{cls.substr(PREFIX.size())};
    }
    // Does this Exec= launch exactly this id? rungameid/2357570, never
    // rungameid/23575701.
    inline bool execLaunches(std::string_view exec, std::string_view digits) {
        if (digits.empty())
            return false;
        const std::string KEY = "rungameid/" + std::string{digits};
        for (size_t at = exec.find(KEY); at != std::string_view::npos; at = exec.find(KEY, at + 1)) {
            const size_t END = at + KEY.size();
            if (END == exec.size() || !(exec[END] >= '0' && exec[END] <= '9'))
                return true;
        }
        return false;
    }
    // The id a `steam steam://rungameid/<digits>` Exec= launches.
    inline std::optional<GameId> fromExec(std::string_view exec) {
        const auto AT = exec.find("steam://rungameid/");
        if (AT == std::string_view::npos)
            return std::nullopt;
        size_t end = AT + 18;
        while (end < exec.size() && exec[end] >= '0' && exec[end] <= '9')
            ++end;
        return parseGameId(exec.substr(AT + 18, end - AT - 18));
    }
    inline std::string keyOf(GameId g) {
        return (g.kind == GameId::Kind::App ? "steam-app-" : "steam-shortcut-") + std::to_string(g.id);
    }
}
