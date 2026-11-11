#pragma once

// AppIdentity's limits and byte helpers, shared by every parser in this folder
// (docs: the AppIdentity design, "Failure behaviour"). A parser here takes a
// span, never a path, and refuses anything over its cap: a plugin fault takes
// the compositor down with it, so no file on disk -- a truncated manifest, a
// multi-GB exe, a hostile shortcuts.vdf -- may cost more than these bytes or
// this nesting. Pure: no Hyprland, no I/O, no exceptions thrown on purpose.

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>

namespace Hyprdrive::AppIdentity {
    namespace Caps {
        inline constexpr size_t VDF         = 4u << 20;   // libraryfolders.vdf, appmanifest_*.acf
        inline constexpr size_t SHORTCUTS   = 1u << 20;   // userdata/*/config/shortcuts.vdf
        inline constexpr size_t YML         = 256u << 10; // a Lutris game yml
        inline constexpr size_t ICO         = 4u << 20;   // an .ico file, one icon image
        inline constexpr size_t RSRC        = 16u << 20;  // a PE resource section
        inline constexpr size_t NET_WM_ICON = 1u << 20;   // _NET_WM_ICON (next part's gatherer)
        inline constexpr size_t DESKTOP     = 256u << 10; // a .desktop file
        inline constexpr size_t ENVIRON     = 256u << 10; // /proc/<pid>/environ
        inline constexpr size_t DIR_ENTRIES = 4096;       // names read from one directory
        inline constexpr size_t VDF_NODES   = 1u << 18;   // keys in one KeyValues document
        inline constexpr int    ICON_PX     = 1024;       // widest icon image decoded
        inline constexpr int    DEPTH       = 32;         // KeyValues nesting
    }

    using Bytes = std::span<const uint8_t>;

    inline Bytes asBytes(std::string_view s) {
        return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
    }
    inline std::string_view asText(Bytes b) {
        return {reinterpret_cast<const char*>(b.data()), b.size()};
    }

    // A little-endian field at `at`, or false when it does not fit (overflow-safe).
    template <typename T>
    inline bool rd(Bytes b, size_t at, T& out) {
        if (at > b.size() || sizeof(T) > b.size() - at)
            return false;
        std::memcpy(&out, b.data() + at, sizeof(T));
        return true;
    }

    inline std::string lower(std::string_view s) {
        std::string o(s);
        std::ranges::transform(o, o.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return o;
    }
    inline bool iequals(std::string_view a, std::string_view b) {
        return a.size() == b.size() && std::ranges::equal(a, b, [](unsigned char x, unsigned char y) { return std::tolower(x) == std::tolower(y); });
    }
    inline bool allDigits(std::string_view s) {
        return !s.empty() && std::ranges::all_of(s, [](char c) { return c >= '0' && c <= '9'; });
    }
    // The last path component, either separator ("C:\\a\\b.exe" -> "b.exe").
    inline std::string_view baseName(std::string_view p) {
        const auto AT = p.find_last_of("/\\");
        return AT == std::string_view::npos ? p : p.substr(AT + 1);
    }
}
