#pragma once

// A C++ symbol as a person reads it: abi::__cxa_demangle over a mangled name
// ("_ZN7Monitor8CMonitor13scheduleFrame..." -> "Monitor::CMonitor::
// scheduleFrame(...)"), and the text unchanged when it is not one (a plain
// "renderWorkspace", or a name the demangler refuses). Header-only, no
// Hyprland headers: tests/unit/demangle_test.cpp checks it.

#include <cstdlib>
#include <cxxabi.h>
#include <string>
#include <string_view>

namespace Hyprdrive {

    inline std::string demangle(std::string_view symbol) {
        const std::string IN{symbol};
        // Only a mangled name: __cxa_demangle also "demangles" a bare type
        // name ("i" -> "int"), which a function name like "f" must not become.
        if (!IN.starts_with("_Z"))
            return IN;
        int   status = 0;
        char* out    = abi::__cxa_demangle(IN.c_str(), nullptr, nullptr, &status);
        if (status != 0 || !out) {
            std::free(out);
            return IN;
        }
        std::string result{out};
        std::free(out);
        return result;
    }

    // A type's name from typeid(T).name() / std::type_info::name(), which is
    // mangled WITHOUT the _Z prefix ("St13runtime_error").
    inline std::string demangleType(const char* name) {
        if (!name)
            return {};
        int   status = 0;
        char* out    = abi::__cxa_demangle(name, nullptr, nullptr, &status);
        if (status != 0 || !out) {
            std::free(out);
            return name;
        }
        std::string result{out};
        std::free(out);
        return result;
    }
}
