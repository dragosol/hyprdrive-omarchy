// LauncherStates.hpp: LauncherEntry state is its SENDER's (review
// 2026-11-09). A browser killed mid-download left its progress on the
// Downloads stack's bar for good, and an app relaunched after a crash took
// back its dead instance's count and urgency. Senders are unique bus names;
// drop() is what NameOwnerChanged calls when one leaves the bus.
#include "LauncherStates.hpp"

#include <cstdio>
#include <string>

using namespace ScrollOverview::LauncherEntry;

static int checks = 0, failed = 0;

static void expect(bool ok, const char* what) {
    ++checks;
    if (!ok) {
        ++failed;
        std::printf("FAIL %s\n", what);
    }
}

int main() {
    // The browser downloads, and is killed.
    {
        SStates s;
        SState  st = s.of(":1.10", "firefox");
        st.progress        = 0.4;
        st.progressVisible = true;
        expect(s.set(":1.10", "firefox", st), "an update is stored");
        expect(s.anyProgress() && *s.anyProgress() == 0.4, "its progress shows");
        expect(!s.set(":1.10", "firefox", st), "the same update again changes nothing");
        expect(s.drop(":1.10"), "the sender leaving drops its entries");
        expect(!s.anyProgress(), "a killed downloader's progress is gone with it");
        expect(!s.drop(":1.10"), "a sender with nothing stored drops nothing");
    }

    // A crash and a relaunch: the new instance starts clean.
    {
        SStates s;
        SState  st = s.of(":1.20", "thunderbird");
        st.count        = 5;
        st.countVisible = true;
        st.urgent       = true;
        s.set(":1.20", "thunderbird", st);
        s.drop(":1.20"); // killed
        SState fresh = s.of(":1.21", "thunderbird");
        fresh.count  = 2; // the relaunch publishes only its count
        s.set(":1.21", "thunderbird", fresh);
        const SState NOW = s.app("thunderbird");
        expect(NOW.count == 2, "the relaunched instance's count");
        expect(!NOW.urgent && !NOW.countVisible, "nothing merged back from the dead instance");
    }

    // One sender's partial updates build on its own earlier ones.
    {
        SStates s;
        SState  st = s.of(":1.30", "discord");
        st.count   = 3;
        s.set(":1.30", "discord", st);
        SState more = s.of(":1.30", "discord");
        more.urgent = true; // an update carrying only `urgent`
        s.set(":1.30", "discord", more);
        expect(s.app("discord").count == 3 && s.app("discord").urgent, "a partial update keeps the sender's other properties");
    }

    // Two live senders for one app: the newest update wins; when it leaves,
    // the other's shows again.
    {
        SStates s;
        SState  a{.count = 1};
        SState  b{.count = 7};
        s.set(":1.40", "app", a);
        s.set(":1.41", "app", b);
        expect(s.app("app").count == 7, "the newest sender's state");
        s.drop(":1.41");
        expect(s.app("app").count == 1, "the other sender's state after the newest leaves");
        expect(s.app("nobody").count == 0 && !s.app("nobody").urgent, "an app nobody published for reads zeroed");
    }

    std::printf("launcher_states_test: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
