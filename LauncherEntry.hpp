#pragma once

#include <systemd/sd-bus.h>

#include <optional>
#include <string>

// OVERVIEW_CPP_SPEC §13.9 X31. Where the dock's badges come FROM.
//
// The reference (Dash to Dock) feeds its counters from two sources: the
// `com.canonical.Unity.LauncherEntry` D-Bus protocol -- `launcherAPI.js`'s
// remote model, which is what Discord/Thunderbird/Nautilus actually broadcast --
// and GNOME Shell's own message tray. `application-counter-overrides-
// notifications` is TRUE in the user's config, so the LauncherEntry count is the
// one that wins whenever an app publishes one.
//
// This is the LauncherEntry half. There is no GNOME message tray under
// Hyprland, so the tray half has no source to read; see RICE_PROGRESS.md for
// that as a named, open decision rather than a silent gap.
//
// A signal match on the session bus, living on the COMPOSITOR'S event loop
// rather than a worker thread like SearchProviders': the handler's only job is
// to damage monitors so an idle desktop notices a new badge, and that is a
// main-thread act. Costs no thread, no mutex and no wake pipe.
namespace ScrollOverview::LauncherEntry {

// The protocol also carries `progress` / `progress-visible`. Dock stacks read
// them (2026-09-24): Chromium-family browsers publish their download progress
// this way, and the Downloads stack's bar shows it.
struct SState {
    long long count           = 0;
    bool      countVisible    = false;
    bool      urgent          = false;
    double    progress        = 0.0;
    bool      progressVisible = false;
};

// The furthest-along progress any app is publishing right now (0..1), or
// nullopt when none is. Only browsers publish one in practice.
std::optional<double> anyProgress();

// By DESKTOP ID ("org.gnome.Nautilus"), the same unit X28's favourites store
// uses -- the protocol carries "application://org.gnome.Nautilus.desktop" and
// this is the parsed half. An app that has never broadcast reads back zeroed,
// which is the same answer as "nothing to show".
SState state(const std::string& desktopId);

// Idempotent. Called when the dock is first drawn rather than from PLUGIN_INIT,
// so a dock switched off never opens a bus connection at all.
void start();
void stop();

// V11 SPEC V5. THE SESSION BUS ITSELF, because the plugin now has a second
// thing to say on it -- the gnome-sushi preview a fan item's SPACE fires --
// and two connections to the same bus from one process is a connection this
// file already has. start()s the connection if it is not up: badges can be
// switched off (damageIfChanged only opens the bus when badge or urgent-notify
// is on) and a preview must still work.
//
// Null if the bus cannot be opened at all, which every caller must treat as
// the session has no bus, not as an error to report.
sd_bus* sessionBus();

}
