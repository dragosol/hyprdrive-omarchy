#include "FunctionHooks.hpp"
#include "globals.hpp"
#include "Demangle.hpp"

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/debug/log/Logger.hpp>

#include <cstdlib>
#include <format>
#include <stdexcept>

namespace PluginDetail {
    void failNotif(const std::string& reason); // Hooks.cpp
}

namespace Hyprdrive::FunctionHooks {
    namespace {
        struct SRecord {
            std::string owner;
            std::string name;
            std::string signature; // the raw symbol
            std::string demangled; // nm's line for it, as Hyprland read it
            void*       address = nullptr;
        };
        std::vector<SRecord> g_records;

        bool alreadyHooked(void* address, const std::string& name, const char* owner) {
            for (const auto& r : g_records) {
                if (r.address != address)
                    continue;
                Log::logger->log(Log::ERR, "[hyprdrive] {}: refusing a second hook on {} ({}): {} already hooks it", owner, name, address, r.owner);
                return true;
            }
            return false;
        }

        CFunctionHook* create(const char* owner, const std::string& name, const SFunctionMatch& fn, void* destination) {
            auto* hook = HyprlandAPI::createFunctionHook(HYPRDRIVE_HANDLE, fn.address, destination);
            if (hook)
                g_records.push_back({.owner = owner, .name = name, .signature = fn.signature, .demangled = fn.demangled, .address = fn.address});
            return hook;
        }
    }

    CFunctionHook* required(const char* owner, const std::string& name, std::initializer_list<std::string_view> demangledNeedles, void* destination) {
        auto fns = HyprlandAPI::findFunctionsByName(HYPRDRIVE_HANDLE, name);
        // A15's test hook: the hook named here reads as missing, as it would on a
        // Hyprland that renamed it -- the one way to see the failure toast, and
        // the unload after a PARTIAL init (the hooks before it already live),
        // without building against a mismatched Hyprland. Headless only:
        //   hl.sh start --env HYPRDRIVE_TEST_MISSING_HOOK=setPositionGlobal
        if (const char* MISSING = std::getenv("HYPRDRIVE_TEST_MISSING_HOOK"); MISSING && name == MISSING)
            fns.clear();
        if (fns.empty()) {
            PluginDetail::failNotif(std::format("no fns for hook {}", name));
            throw std::runtime_error(std::format("[hyprdrive] No fns for hook {}", name));
        }

        const SFunctionMatch* chosen = &fns[0];
        std::vector<SFunctionMatch> matches;
        if (!(demangledNeedles.size() == 0 || (demangledNeedles.size() == 1 && demangledNeedles.begin()->empty()))) {
            matches.reserve(fns.size());
            for (const auto& fn : fns) {
                for (const auto& needle : demangledNeedles) {
                    if (needle.empty() || fn.demangled.find(needle) != std::string::npos) {
                        matches.push_back(fn);
                        break;
                    }
                }
            }

            if (matches.empty()) {
                PluginDetail::failNotif(std::format("no matching overload for hook {}", name));
                throw std::runtime_error(std::format("[hyprdrive] No matching overload for hook {}", name));
            }

            if (matches.size() > 1) {
                PluginDetail::failNotif(std::format("ambiguous overload for hook {} ({} matches)", name, matches.size()));
                throw std::runtime_error(std::format("[hyprdrive] Ambiguous overload for hook {}", name));
            }
            chosen = &matches[0];
        }

        // Never reached by today's hooks (each required name resolves to its
        // own function); a plugin that cannot hook what it requires does not load.
        if (alreadyHooked(chosen->address, name, owner)) {
            PluginDetail::failNotif(std::format("hook {} would hook an address twice", name));
            throw std::runtime_error(std::format("[hyprdrive] Second hook on the address of {}", name));
        }
        return create(owner, name, *chosen, destination);
    }

    CFunctionHook* firstMatch(const char* owner, const std::string& name, void* destination, std::string_view demangledNeedle, bool* found) {
        if (found)
            *found = false;
        for (const auto& FN : HyprlandAPI::findFunctionsByName(HYPRDRIVE_HANDLE, name)) {
            if (!demangledNeedle.empty() && FN.demangled.find(demangledNeedle) == std::string::npos)
                continue;
            if (found)
                *found = true;
            if (alreadyHooked(FN.address, name, owner))
                return nullptr;
            return create(owner, name, FN, destination);
        }
        return nullptr;
    }

    std::vector<std::string> dump() {
        std::vector<std::string> lines;
        lines.reserve(g_records.size());
        for (const auto& r : g_records)
            lines.push_back(std::format("{} {} at {} -- {}", r.owner, demangle(r.name), r.address, readableSignature(r.signature, r.demangled)));
        return lines;
    }
}
