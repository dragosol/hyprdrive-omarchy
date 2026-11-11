#pragma once

// THE PLUGIN'S USER-FACING IDENTITY, and the one place its old one still
// exists.
//
// This fork shipped as `scrolloverview`: that string was the plugin's name in
// `hyprctl plugin list`, its Lua namespace (`hl.plugin.scrolloverview.*`), its
// dispatcher prefix (`scrolloverview:overview`), its config-key prefix
// (`plugin:scrolloverview:*`), its keybind submap, its IPC event, its .so and
// its state directory. Phase 9 renames all of it to `hyprdrive`.
//
// A rename that breaks every existing config is not a rename, it is a
// migration the user has to perform. So every old name keeps working FOR ONE
// RELEASE:
//
//   * every `plugin:scrolloverview:*` key is registered a second time, from the
//     same expression as its `plugin:hyprdrive:*` twin -- same type, same
//     default, same range -- and a value the user set on the old name is copied
//     onto the new one at every config reload, before anything reads it. The
//     new name wins if both are set;
//   * the old dispatcher names and the old Lua namespace stay registered and
//     dispatch into exactly the same handlers;
//   * the compositor emits BOTH IPC events, so a shell listening for the old
//     one keeps reacting;
//   * the old favorites store is adopted once, at load.
//
// The first legacy name used in a session -- key, dispatcher or Lua -- prints
// ONE warning naming it. Once per session, not once per call: these names are
// read on every config reload and dispatched on every keypress, and a warning
// that fires per use is a warning nobody reads.
//
// The warning goes to stderr (the session captures it, so it lands in the
// journal) and to $XDG_RUNTIME_DIR/hyprdrive-compat.log (DebugPaths.hpp), the same file-sink
// convention the dock's flight log already uses. It is deliberately NOT a
// notification: this fires on any config that has not been migrated yet --
// which, for one release, is every existing config -- and a notification would
// sit in the middle of every pixel gate's frame for five seconds. The log is
// also the only readback there is: `hyprctl eval` answers "ok" and discards
// whatever the Lua returned, so a gate cannot ask the plugin a question.
//
// EXPIRY. Delete this file, the LEGACY_* constants, the second registration
// pass in Config.cpp, adoptLegacyValues(), adoptLegacyFavorites() and every
// noteLegacyUse() call site one release after the rename ships. Nothing else
// in the plugin knows the old name.

#include <cstddef>
#include <cstdint>
#include <string>
#include <typeinfo>

namespace Hyprdrive::Compat {

// ------------------------------------------------------------- identity --
inline constexpr const char* NAME       = "hyprdrive";
inline constexpr const char* KEY_PREFIX = "plugin:hyprdrive:";

// ------------------------------------------------- compat, ONE RELEASE ---
inline constexpr const char* LEGACY_NAME       = "scrolloverview";
inline constexpr const char* LEGACY_KEY_PREFIX = "plugin:scrolloverview:";

enum class ELegacy : uint8_t {
    KEY,        // plugin:scrolloverview:<x> was set in the config
    DISPATCHER, // the scrolloverview:<x> dispatcher was invoked
    LUA,        // hl.plugin.scrolloverview.<x> was called
};

// Truncates the compat log and stamps it. Called once, at plugin init, before
// anything can record a use.
void   openLog();

// Records one use of an old name -- one `use` line each -- and, on the FIRST of
// the session, the one `WARN` line. That asymmetry IS the contract the gate
// asserts: many uses, one warning.
void   noteLegacyUse(ELegacy kind, const std::string& name);

// Pairs a key's two registrations. `underlying` is the value's own
// IValue::underlying(), which is how the copy below knows what it is copying
// without a second table of types to keep in step.
void   pairKey(const char* modern, const char* legacy, const std::type_info* underlying);

// Copies every legacy key the user set onto its modern twin. Called on every
// config reload, BEFORE anything in the plugin reads a value.
void   adoptLegacyValues();

// ~/.local/state/scrolloverview/favorites -> ~/.local/state/hyprdrive/favorites,
// once, at load, only when the new one does not exist yet. A dock that forgets
// the user's pinned apps because the plugin was renamed is a data loss, not a
// migration.
void   adoptLegacyFavorites();

}
