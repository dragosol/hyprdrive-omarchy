#pragma once

// The one place that finds a Hyprland function and puts a hook on it. Every
// CFunctionHook the plugin owns is created here and recorded, in creation
// order, so `hyprdrive:debug hooks` can say exactly what the plugin hooks.
//
// Only the find-and-create step lives here. Each module still decides WHEN it
// hooks, calls hook() itself, keeps its own pointer and trampoline, says in its
// own words what a missing function means, and unhooks (or leaves the hook to
// Hyprland's unload) on its own schedule.
//
// One hook per address: a second create on an address already hooked is
// refused -- nullptr and a log line from the optional finders, the load-failure
// toast and an exception from required() -- because two trampolines on one
// function leave one destination silently uncalled. CWindowTarget::
// setPositionGlobal is deliberately ONE hook (main.cpp) that DockRoom and
// OwnSizeTile both ride.

#include <hyprland/src/plugins/HookSystem.hpp>

#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace Hyprdrive::FunctionHooks {
    // The rule main.cpp's findFnOrThrow had, unchanged: the functions found by
    // `name`; none -> throw. No needle (or one empty needle) -> the first of them. Otherwise
    // exactly one whose demangled signature contains any needle; none or more
    // than one -> throw. Every throw first shows the load-failure toast; the
    // exception is std::runtime_error with the text the plugin always used.
    // HYPRDRIVE_TEST_MISSING_HOOK=<name> makes `name` read as missing (A15).
    CFunctionHook* required(const char* owner, const std::string& name, std::initializer_list<std::string_view> demangledNeedles, void* destination);

    // The FIRST function found by `name` (a mangled name finds exactly its
    // own) whose demangled signature contains `demangledNeedle` (an empty
    // needle matches the first one found). nullptr when there is none; then
    // `*found` (when given) is false. Says nothing about a miss -- the caller
    // does, in its own words.
    CFunctionHook* firstMatch(const char* owner, const std::string& name, void* destination, std::string_view demangledNeedle = {}, bool* found = nullptr);

    // One line per hook created, in creation order: owner, name, address,
    // signature -- the name and the signature both demangled (Demangle.hpp:
    // a hook asked for by its mangled name printed it mangled).
    std::vector<std::string> dump();
}
