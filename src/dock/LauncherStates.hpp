#pragma once

// THE LAUNCHERENTRY STATE BELONGS TO ITS SENDER (review 2026-11-09). It was
// kept per app only, merged across whoever sent it, and nothing ever removed
// it: a browser killed mid-download left its progress frozen on the
// Downloads stack's bar, and an app relaunched after a crash started from its
// dead instance's count and urgency -- each new Update merged into them. Now
// every entry is keyed by the sender's unique bus name as well, and
// LauncherEntry.cpp drops a sender's entries when NameOwnerChanged says it
// left the bus, as Dash to Dock does. Pure: tests/unit/launcher_states_test.cpp
// builds it on the host.

#include "LauncherEntry.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Hyprdrive::LauncherEntry {

    struct SStates {
        struct SEntry {
            std::string sender; // the unique bus name it came from (":1.42")
            std::string id;     // desktop id
            SState      state;
            uint64_t    seq = 0; // when it last changed: an app's newest sender wins
        };
        std::vector<SEntry> entries;
        uint64_t            seq = 0;

        // What `sender` last published for `id`, zeroed if nothing: a sender may
        // update one property, and the rest are its own earlier ones.
        SState of(const std::string& sender, const std::string& id) const {
            const auto IT = std::ranges::find_if(entries, [&](const SEntry& e) { return e.sender == sender && e.id == id; });
            return IT == entries.end() ? SState{} : IT->state;
        }

        // Stores it. False when it is exactly what that sender had already.
        bool set(const std::string& sender, const std::string& id, const SState& s) {
            const auto IT = std::ranges::find_if(entries, [&](const SEntry& e) { return e.sender == sender && e.id == id; });
            const auto WAS = IT == entries.end() ? SState{} : IT->state;
            if (WAS.count == s.count && WAS.countVisible == s.countVisible && WAS.urgent == s.urgent && WAS.progress == s.progress && WAS.progressVisible == s.progressVisible)
                return false;
            if (IT == entries.end())
                entries.push_back({.sender = sender, .id = id, .state = s, .seq = ++seq});
            else {
                IT->state = s;
                IT->seq   = ++seq;
            }
            return true;
        }

        // The sender left the bus: everything it published goes with it. True if
        // anything did.
        bool drop(const std::string& sender) {
            return std::erase_if(entries, [&](const SEntry& e) { return e.sender == sender; }) > 0;
        }

        // An app's state: its newest sender's, zeroed if it has none.
        SState app(const std::string& id) const {
            const SEntry* best = nullptr;
            for (const auto& E : entries)
                if (E.id == id && (!best || E.seq > best->seq))
                    best = &E;
            return best ? best->state : SState{};
        }

        // The furthest-along progress any live sender is publishing, 0..1.
        std::optional<double> anyProgress() const {
            std::optional<double> out;
            for (const auto& E : entries)
                if (E.state.progressVisible)
                    out = std::max(out.value_or(0.0), std::clamp(E.state.progress, 0.0, 1.0));
            return out;
        }
    };
}
