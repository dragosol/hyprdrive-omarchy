#pragma once

// ToolSearch's PURE half: what the search field's text asks for. No config, no
// Hyprland, no I/O -- header-only so scripts/tool_search_test.cpp can hold it
// to its cases without linking the compositor.

#include "ToolSearch.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_map>
#include <utility>

namespace ToolSearch {

    namespace detail {

        // ------------------------------------------------------------ parsing --

        inline std::string lower(std::string s) {
            std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return s;
        }

        inline std::string trim(const std::string& s) {
            const auto A = s.find_first_not_of(" \t");
            if (A == std::string::npos)
                return "";
            const auto B = s.find_last_not_of(" \t");
            return s.substr(A, B - A + 1);
        }

        // First whitespace-delimited word, and everything after it.
        inline std::pair<std::string, std::string> splitFirst(const std::string& s) {
            const auto T = trim(s);
            const auto SP = T.find_first_of(" \t");
            if (SP == std::string::npos)
                return {T, ""};
            return {T.substr(0, SP), trim(T.substr(SP + 1))};
        }

        // A term is at most this long: a lookup is a word or a phrase, and a
        // runaway paste should not become a request.
        constexpr size_t MAX_TERM = 400;

        // The codes both services take for the languages anyone is likely to
        // type. A whitelist rather than "any two letters", so `translate it now`
        // does not quietly become Italian more often than it has to.
        inline const std::unordered_map<std::string, std::string>& languages() {
            static const std::unordered_map<std::string, std::string> MAP = {
                {"english", "en"},   {"spanish", "es"},    {"french", "fr"},     {"german", "de"},    {"italian", "it"},   {"portuguese", "pt"},
                {"romanian", "ro"},  {"russian", "ru"},    {"japanese", "ja"},   {"chinese", "zh"},   {"korean", "ko"},    {"arabic", "ar"},
                {"hindi", "hi"},     {"dutch", "nl"},      {"polish", "pl"},     {"swedish", "sv"},   {"turkish", "tr"},   {"ukrainian", "uk"},
                {"greek", "el"},     {"czech", "cs"},      {"danish", "da"},     {"finnish", "fi"},   {"norwegian", "no"}, {"hungarian", "hu"},
                {"hebrew", "he"},    {"indonesian", "id"}, {"thai", "th"},       {"vietnamese", "vi"}, {"persian", "fa"},  {"bulgarian", "bg"},
                {"croatian", "hr"},  {"serbian", "sr"},    {"slovak", "sk"},     {"slovenian", "sl"}, {"lithuanian", "lt"}, {"latvian", "lv"},
                {"estonian", "et"},  {"catalan", "ca"},    {"irish", "ga"},      {"malay", "ms"},     {"tagalog", "tl"},   {"swahili", "sw"},
                {"bengali", "bn"},   {"urdu", "ur"},       {"tamil", "ta"},      {"latin", "la"},
            };
            return MAP;
        }

    } // namespace detail

    inline std::string languageCode(const std::string& word) {
        const auto W = detail::lower(detail::trim(word));
        if (W.empty())
            return "";
        if (const auto IT = detail::languages().find(W); IT != detail::languages().end())
            return IT->second;

        // A bare code, optionally regioned: "es", "pt-br" -> "pt-BR".
        const auto DASH = W.find('-');
        const auto BASE = W.substr(0, DASH);
        const bool KNOWN = std::ranges::any_of(detail::languages(), [&](const auto& kv) { return kv.second == BASE; });
        if (!KNOWN)
            return "";
        if (DASH == std::string::npos)
            return BASE;
        const auto REGION = W.substr(DASH + 1);
        if (REGION.size() < 2 || REGION.size() > 4 || !std::ranges::all_of(REGION, [](unsigned char c) { return std::isalpha(c); }))
            return "";
        std::string up = REGION;
        std::ranges::transform(up, up.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        return BASE + "-" + up;
    }

    inline SQuery parse(const std::string& text) {
        SQuery     q;
        const auto [HEAD, REST] = detail::splitFirst(text);
        auto       cmd          = detail::lower(HEAD);
        if (!cmd.empty() && cmd.back() == ':')
            cmd.pop_back();

        if (cmd == "define" || cmd == "def" || cmd == "dict") {
            q.kind = EKind::Define;
            q.term = REST.substr(0, detail::MAX_TERM);
            return q;
        }

        if (cmd == "tr" || cmd == "translate") {
            q.kind = EKind::Translate;

            // The first word may be the language: "es", "spanish", "en:es",
            // "en>es", "en->es". It only counts as one when text follows --
            // `tr hello` is a word to translate, not a language named hello.
            const auto [FIRST, AFTER] = detail::splitFirst(REST);
            if (!AFTER.empty()) {
                for (const std::string SEP : {"->", ":", ">"}) {
                    const auto AT = FIRST.find(SEP);
                    if (AT == std::string::npos)
                        continue;
                    const auto FROM = languageCode(FIRST.substr(0, AT));
                    const auto TO   = languageCode(FIRST.substr(AT + SEP.size()));
                    if (!FROM.empty() && !TO.empty()) {
                        q.from = FROM;
                        q.to   = TO;
                        q.term = AFTER.substr(0, detail::MAX_TERM);
                        return q;
                    }
                }
                if (const auto TO = languageCode(FIRST); !TO.empty()) {
                    q.to   = TO;
                    q.term = AFTER.substr(0, detail::MAX_TERM);
                    return q;
                }
            }

            q.term = REST.substr(0, detail::MAX_TERM);
            return q;
        }

        return q;
    }


} // namespace ToolSearch
