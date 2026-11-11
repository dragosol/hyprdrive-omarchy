#pragma once

// WHAT TOOK THE COMPOSITOR DOWN, when it was a std::terminate. A SIGABRT at
// Hyprland's exit was seen once and never again (2026-11), with nothing to
// say where it came from. While the plugin is loaded, its std::terminate
// handler appends one record to $XDG_STATE_HOME/hyprdrive/terminate.log
// (~/.local/state/... without it; a state directory, so it survives the
// reboot that usually follows): the exception in flight -- its demangled type
// and what() -- or that there was none, and a backtrace (backtrace_symbols_fd,
// straight to the file, no allocation). Then it calls the handler that was
// installed before it, which aborts as it always did: nothing is swallowed.
//
// The handler is process-wide, so a terminate in Hyprland itself or another
// plugin is logged too, and otherwise goes exactly the way it went. It costs
// nothing until it fires. The file is capped: one past CAP bytes is started
// over before the next record. Only a regular file of ours is written, never
// through a symlink, 0600 in a 0700 directory.
//
// No Hyprland headers: tests/unit/terminate_log_test.cpp runs it in a child
// process that really terminates.

#include <cstddef>
#include <string>

namespace Hyprdrive::TerminateLog {

    inline constexpr size_t CAP = 64 * 1024;

    // The log's path for these XDG_STATE_HOME and HOME values (null = unset):
    // $XDG_STATE_HOME/hyprdrive/terminate.log when it is absolute, else
    // $HOME/.local/state/hyprdrive/terminate.log, else "" (nothing is written).
    std::string pathFor(const char* xdgStateHome, const char* home);

    // PLUGIN_INIT, once nothing after it can throw (Hyprland dlcloses a plugin
    // whose init throws WITHOUT calling PLUGIN_EXIT, which would leave the
    // handler pointing into unmapped code). Remembers the handler it replaces.
    void install();
    // PLUGIN_EXIT: puts the previous handler back. False when someone installed
    // another handler over ours since: theirs stays, and since it may chain to
    // ours, this shared object is kept loaded (RTLD_NODELETE) rather than
    // leave that chain pointing into unmapped code.
    bool uninstall();
    bool installed();
    // Where install() decided to write ("" = nowhere).
    const std::string& path();
}
