#pragma once

#include "globals.hpp"

#include <string>
#include <vector>

// THE SCRATCHPAD: Hyprland's special workspaces, as the dock and the overview
// see them. Omarchy binds one (`special:scratchpad`, SUPER+grave on this rice),
// but nothing stops a user having several, so every answer here is keyed off the
// workspace's OWN name rather than that literal -- with one exception, the
// plugin's own `special:minimized` stash, which is not a scratchpad and has
// semantics of its own (Minimize.hpp). Minimize::isStash is what keeps the two
// apart, and it is asked here so no caller has to know the difference.
namespace ScrollOverview::Scratchpad {

// Every special workspace that is not the minimize stash.
std::vector<PHLWORKSPACE> workspaces();
// ...that holds at least one mapped window. The dock slot exists only while
// this is non-empty: an empty scratchpad costs no dock space, which is the same
// rule the app icons follow.
std::vector<PHLWORKSPACE> populated();
// The scratchpad currently pulled UP on this monitor, if any.
PHLWORKSPACE              shownOn(const PHLMONITOR& monitor);
PHLWORKSPACE              byShortName(const std::string& shortName);

bool                      isScratchpad(const PHLWORKSPACE& workspace);
// The scratchpad this window lives on, or null. A MINIMIZED window answers
// null: it is stashed, not scratchpadded, and the dock must not confuse them.
PHLWORKSPACE              workspaceFor(const PHLWINDOW& window);
bool                      holdsWindow(const PHLWINDOW& window);

std::vector<PHLWINDOW>    windowsOn(const PHLWORKSPACE& workspace);

// "special:scratchpad" -> "scratchpad".
std::string               shortName(const PHLWORKSPACE& workspace);
// What the dock tooltip and the strip cell show. Hyprland's unnamed default is
// literally `special`, which is not a word to put in front of a user.
std::string               label(const PHLWORKSPACE& workspace);
// An icon name the LIVE theme actually carries.
std::string               iconName();

// The scratchpad to park a window on, CREATING it if the user has none yet --
// the overview's drop target has to work on a fresh install, where no special
// workspace exists until something is put on one.
PHLWORKSPACE              ensureDefault(const PHLMONITOR& monitor);

void                      show(const PHLWORKSPACE& workspace);
void                      hide(const PHLWORKSPACE& workspace);
// Up if it is down, down if it is up: the dock icon's click.
void                      toggle(const PHLWORKSPACE& workspace);
// Park a window on a scratchpad -- the overview strip cell's drop.
void                      moveWindowTo(const PHLWINDOW& window, const PHLWORKSPACE& workspace);

}
