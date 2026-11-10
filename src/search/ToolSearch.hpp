#pragma once

// THE DICTIONARY AND THE TRANSLATOR in the overview search (user, 2026-09-22):
// "add a dictionary tool in the 'AI & Tools' that turns the results box into
// dictionary definitions. And another tool to translate that turns the results
// window into a from/to translator."
//
//   define serendipity        def serendipity        dict serendipity
//   tr es good morning        tr en:es good morning   tr spanish good morning
//   translate good morning    (into search:translate_target, else $LANG)
//
// Both are OPT-IN (search:dictionary, search:translate): they send what was
// typed to a third party -- en.wiktionary.org, and MyMemory unless
// search:translate_endpoint names a (self-hosted) LibreTranslate.
//
// The network never touches the compositor thread. A lookup is handed to one
// worker, which runs `curl | jq` through posix_spawn with the user's text as a
// positional ARGUMENT -- never spliced into a shell string -- and the answer is
// cached per query. `generation()` moves whenever an answer lands, which is what
// lets the memoised result rows notice.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ToolSearch {

    enum class EKind : uint8_t {
        None,
        Define,
        Translate,
    };

    struct SQuery {
        EKind       kind = EKind::None;
        std::string term;         // the word, or the text to translate
        std::string from = "";    // "" = detect
        std::string to   = "";    // "" = the configured default
        bool        operator==(const SQuery&) const = default;
    };

    // parse() and languageCode() -- the pure half -- are in ToolSearchParse.hpp,
    // included at the bottom of this file.

    struct SSense {
        std::string tag;     // part of speech ("Noun"); "" for a translation
        std::string text;    // the definition, or the translated text
        std::string example; // a usage example, if the source gave one
    };

    struct SResult {
        enum class EState : uint8_t {
            Loading,
            Ready,
            Empty, // looked up, nothing there
            Error, // the service could not be reached
        } state = EState::Loading;

        std::string         title;    // the headword / the source text
        std::string         subtitle; // "Wiktionary" / "en → es · MyMemory"
        std::vector<SSense> senses;
        std::string         message;  // Error: what the service said (quota, bad pair); "" = unreachable
    };

    // Is this kind switched on in the config.
    bool enabled(EKind kind);

    // The current answer for `query`, starting a lookup if there is none yet.
    // Never blocks. A query not yet answered comes back as Loading.
    SResult lookup(const SQuery& query);

    // Moves every time an answer lands.
    uint64_t generation();

    // Where `translate <text>` goes with no language named: the last one used
    // (remembered across restarts), else search:translate_target, else $LANG.
    std::string defaultTargetLanguage();

    // Called (from the worker thread) when an answer lands; main.cpp turns it
    // into a frame on the compositor thread.
    void setReadyCallback(std::function<void()> cb);

    void shutdown();

} // namespace ToolSearch

#include "ToolSearchParse.hpp"
