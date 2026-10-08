#pragma once

// THE DOCK'S DRIVE ORDER (dock:mount_sort, 2026-10-08). A user with four drives
// named KleOne, KleTwo, KleThree, KleFour found them in the order they happened
// to be mounted, and asked for alphabetical -- then pointed out that
// alphabetical puts Three between One and Two, and that French is worse. So
// there are two sorts beside the mount order:
//
//   name   -- natural: digit runs compare as numbers, letters without case.
//   fstab  -- the order the drives are written in /etc/fstab (fstabRank).
//   smart  -- name, but a name is read as WORDS (split at spaces, punctuation,
//             letter/digit edges and camelCase humps), and a word that spells a
//             number -- English or French, cardinal or ordinal, or a standalone
//             Roman numeral -- compares as that number. KleOne < KleTwo <
//             KleThree; "Disque deux" < "Disque trois"; "Backup II" < "Backup 3".
//
// Pure: no Hyprland, no GLib. scripts/mount_sort_test.cpp builds it on the host.

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ScrollOverview::MountSort {

    struct SToken {
        bool        number = false;
        uint64_t    value  = 0;  // number
        std::string word;        // otherwise: lowercased (ASCII)
    };

    // ASCII only: a non-ASCII byte is part of a UTF-8 letter and is kept.
    inline std::string lower(std::string_view s) {
        std::string out(s);
        for (auto& c : out)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return out;
    }

    // A word that spells a number, or nothing. Lowercased input. Kept to the
    // numbers a person actually names drives with -- up to twenty, and the tens.
    inline std::optional<uint64_t> numberWord(const std::string& w) {
        struct SWord {
            const char* word;
            uint64_t    value;
        };
        static constexpr SWord WORDS[] = {
            // English cardinals and ordinals
            {"zero", 0}, {"one", 1}, {"two", 2}, {"three", 3}, {"four", 4}, {"five", 5}, {"six", 6}, {"seven", 7}, {"eight", 8}, {"nine", 9}, {"ten", 10},
            {"eleven", 11}, {"twelve", 12}, {"thirteen", 13}, {"fourteen", 14}, {"fifteen", 15}, {"sixteen", 16}, {"seventeen", 17}, {"eighteen", 18},
            {"nineteen", 19}, {"twenty", 20}, {"thirty", 30}, {"forty", 40}, {"fifty", 50}, {"hundred", 100},
            {"first", 1}, {"second", 2}, {"third", 3}, {"fourth", 4}, {"fifth", 5}, {"sixth", 6}, {"seventh", 7}, {"eighth", 8}, {"ninth", 9}, {"tenth", 10},
            // French cardinals and ordinals (lowercase UTF-8)
            {"z\xc3\xa9ro", 0}, {"un", 1}, {"une", 1}, {"deux", 2}, {"trois", 3}, {"quatre", 4}, {"cinq", 5}, {"sept", 7}, {"huit", 8}, {"neuf", 9},
            {"dix", 10}, {"onze", 11}, {"douze", 12}, {"treize", 13}, {"quatorze", 14}, {"quinze", 15}, {"seize", 16}, {"vingt", 20}, {"trente", 30},
            {"quarante", 40}, {"cinquante", 50}, {"cent", 100},
            {"premier", 1}, {"premi\xc3\xa8re", 1}, {"deuxi\xc3\xa8me", 2}, {"seconde", 2}, {"troisi\xc3\xa8me", 3}, {"quatri\xc3\xa8me", 4},
            {"cinqui\xc3\xa8me", 5}, {"sixi\xc3\xa8me", 6}, {"septi\xc3\xa8me", 7}, {"huiti\xc3\xa8me", 8}, {"neuvi\xc3\xa8me", 9}, {"dixi\xc3\xa8me", 10},
        };
        for (const auto& W : WORDS)
            if (w == W.word)
                return W.value;
        return std::nullopt;
    }

    // A standalone Roman numeral: only a WHOLE word of its own (never a camelCase
    // hump -- "MIX" in "DiskMIX" is not 1009), upper case, I..MMMM, and valid
    // by the subtractive rules (round-trips through its own formatting).
    inline std::optional<uint64_t> roman(const std::string& raw) {
        if (raw.empty() || raw.size() > 8)
            return std::nullopt;
        uint64_t total = 0, prev = 0;
        for (auto it = raw.rbegin(); it != raw.rend(); ++it) {
            uint64_t v = 0;
            switch (*it) {
                case 'I': v = 1; break;
                case 'V': v = 5; break;
                case 'X': v = 10; break;
                case 'L': v = 50; break;
                case 'C': v = 100; break;
                case 'D': v = 500; break;
                case 'M': v = 1000; break;
                default: return std::nullopt;
            }
            if (v < prev)
                total -= v;
            else {
                total += v;
                prev = v;
            }
        }
        // Canonical form only, so "IIII" or "VX" are words, not numbers.
        static constexpr std::pair<uint64_t, const char*> UNITS[] = {{1000, "M"}, {900, "CM"}, {500, "D"}, {400, "CD"}, {100, "C"}, {90, "XC"}, {50, "L"},
                                                                     {40, "XL"},  {10, "X"},   {9, "IX"},  {5, "V"},   {4, "IV"},  {1, "I"}};
        std::string canon;
        uint64_t    n = total;
        for (const auto& [V, S] : UNITS)
            while (n >= V) {
                canon += S;
                n -= V;
            }
        return canon == raw && total > 0 ? std::optional<uint64_t>{total} : std::nullopt;
    }

    // Split a name into words and numbers. Letters are ASCII letters or any
    // non-ASCII byte (so "deuxième" stays one word); a hump is a lower-case
    // letter followed by an upper-case one.
    inline std::vector<SToken> tokens(const std::string& name) {
        std::vector<SToken> out;
        const auto isLetter = [](unsigned char c) { return std::isalpha(c) || c >= 0x80; };
        size_t     i        = 0;
        const auto N        = name.size();
        while (i < N) {
            const unsigned char C = name[i];
            if (std::isdigit(C)) {
                uint64_t v = 0;
                while (i < N && std::isdigit(static_cast<unsigned char>(name[i]))) {
                    v = v * 10 + static_cast<uint64_t>(name[i] - '0');
                    if (v > (UINT64_MAX / 10) - 10)
                        v = (UINT64_MAX / 10) - 10; // a 30-digit serial is still "very big", not overflow
                    ++i;
                }
                out.push_back({.number = true, .value = v});
                continue;
            }
            if (!isLetter(C)) {
                ++i;
                continue;
            }
            // A word runs to the next non-letter or the next hump.
            const size_t START     = i;
            bool         wholeWord = START == 0 || !isLetter(static_cast<unsigned char>(name[START - 1]));
            ++i;
            while (i < N && isLetter(static_cast<unsigned char>(name[i]))) {
                const unsigned char P = name[i - 1], Q = name[i];
                if (std::islower(P) && std::isupper(Q))
                    break;
                ++i;
            }
            const std::string RAW = name.substr(START, i - START);
            wholeWord             = wholeWord && (i == N || !isLetter(static_cast<unsigned char>(name[i])));
            const std::string LOW = lower(RAW);
            if (const auto V = numberWord(LOW))
                out.push_back({.number = true, .value = *V});
            else if (const auto R = wholeWord ? roman(RAW) : std::nullopt)
                out.push_back({.number = true, .value = *R});
            else
                out.push_back({.word = LOW});
        }
        return out;
    }

    // Word by word: numbers by value and before words; words without case.
    // Equal all the way: the shorter first, then the raw names, so the order
    // is total and stable however the mount table lists them.
    inline bool smartLess(const std::string& a, const std::string& b) {
        const auto A = tokens(a), B = tokens(b);
        for (size_t i = 0; i < std::min(A.size(), B.size()); ++i) {
            const auto& X = A[i];
            const auto& Y = B[i];
            if (X.number != Y.number)
                return X.number;
            if (X.number) {
                if (X.value != Y.value)
                    return X.value < Y.value;
            } else if (X.word != Y.word)
                return X.word < Y.word;
        }
        if (A.size() != B.size())
            return A.size() < B.size();
        return a < b;
    }

    // FSTAB (the same user, 2026-10-08: "smart sort will struggle based on
    // the user language, maybe fstab is our best friend"). The order the drives
    // are written in /etc/fstab is one the user chose, in no language at all.
    // A drive's rank is its line among the fstab mount points; one fstab does
    // not list (a stick, a phone) ranks after every listed drive, and those
    // keep their mount order between themselves (the sort is stable).
    inline size_t fstabRank(const std::vector<std::string>& fstabPaths, const std::string& mountPath) {
        for (size_t i = 0; i < fstabPaths.size(); ++i)
            if (fstabPaths[i] == mountPath)
                return i;
        return fstabPaths.size();
    }

    enum class EOrder : uint8_t { Mounted, Name, Smart, Fstab };

    inline EOrder orderFromName(const std::string& s) {
        if (s == "name")
            return EOrder::Name;
        if (s == "smart")
            return EOrder::Smart;
        if (s == "fstab")
            return EOrder::Fstab;
        return EOrder::Mounted;
    }
}
