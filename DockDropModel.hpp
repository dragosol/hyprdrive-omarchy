#pragma once

// WHAT A DROP ONTO THE DOCK MEANS (dock stacks B). Pure and header-only, so
// scripts/dock_drop_test.cpp holds it to its cases on the host: the file list a
// drag source hands over (text/uri-list), a free name in the destination, and
// the decision table (what lands where).

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace ScrollOverview::DockDropModel {

    namespace Detail {
        inline int hexValue(char c) {
            if (c >= '0' && c <= '9')
                return c - '0';
            if (c >= 'a' && c <= 'f')
                return c - 'a' + 10;
            if (c >= 'A' && c <= 'F')
                return c - 'A' + 10;
            return -1;
        }

        inline std::string percentDecode(const std::string& s) {
            std::string out;
            out.reserve(s.size());
            for (size_t i = 0; i < s.size(); ++i) {
                if (s[i] == '%' && i + 2 < s.size()) {
                    const int H = hexValue(s[i + 1]), L = hexValue(s[i + 2]);
                    if (H >= 0 && L >= 0) {
                        out += static_cast<char>(H * 16 + L);
                        i += 2;
                        continue;
                    }
                }
                out += s[i];
            }
            return out;
        }

        inline std::string stripSlashes(std::string s) {
            while (s.size() > 1 && s.back() == '/')
                s.pop_back();
            return s;
        }
    }

    // RFC 2483 text/uri-list: one URI per line (CRLF, but LF is accepted), `#`
    // lines are comments. Only local file URIs become paths -- `file:///p` or
    // `file://localhost/p`; another host or scheme is not a file on this disk.
    inline std::vector<std::string> parseUriList(const std::string& data) {
        std::vector<std::string> out;
        size_t                   at = 0;
        while (at <= data.size()) {
            const auto  NL   = data.find('\n', at);
            std::string line = data.substr(at, NL == std::string::npos ? std::string::npos : NL - at);
            at               = NL == std::string::npos ? data.size() + 1 : NL + 1;
            while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                line.pop_back();
            if (line.empty() || line[0] == '#')
                continue;
            std::string rest;
            if (line.starts_with("file:///"))
                rest = line.substr(7);
            else if (line.starts_with("file://localhost/"))
                rest = line.substr(16);
            else
                continue;
            out.push_back(Detail::percentDecode(rest));
        }
        return out;
    }

    // A name in `dir` that does not exist yet: `base`, else `stem (2).ext`,
    // `stem (3).ext`, ... A dotfile (`.bashrc`) is all stem; a double extension
    // (`x.tar.gz`) keeps both parts together.
    inline std::string uniqueName(const std::string& dir, const std::string& base, const std::function<bool(const std::string&)>& exists) {
        const auto JOIN = [&](const std::string& name) { return Detail::stripSlashes(dir) + "/" + name; };
        if (!exists(JOIN(base)))
            return base;
        std::string stem = base, ext;
        if (const auto DOT = base.find('.', 1); DOT != std::string::npos && base[0] != '.') {
            stem = base.substr(0, DOT);
            ext  = base.substr(DOT);
        }
        for (int n = 2; n < 10000; ++n) {
            const auto NAME = stem + " (" + std::to_string(n) + ")" + ext;
            if (!exists(JOIN(NAME)))
                return NAME;
        }
        return base; // unreachable in practice; the caller's move then fails and is reported
    }

    enum class ECell : uint8_t {
        None,  // off the dock
        Stack, // a stack's (folder) cell
        Trash, // the Trash cell
        App,   // any other cell
        Gap,   // the pill between cells
    };

    enum class EAction : uint8_t {
        None,
        IntoStack, // move/copy the items into the stack's folder
        ToTrash,
        PinAt, // pin the one folder as a stack at the gap
    };

    inline EAction resolve(ECell cell, size_t files, size_t dirs) {
        switch (cell) {
            case ECell::Stack: return files + dirs > 0 ? EAction::IntoStack : EAction::None;
            case ECell::Trash: return files + dirs > 0 ? EAction::ToTrash : EAction::None;
            case ECell::Gap: return (dirs == 1 && files == 0) ? EAction::PinAt : EAction::None;
            case ECell::App:
            case ECell::None: break;
        }
        return EAction::None;
    }

    // `path` is `dir` itself or somewhere under it -- a folder must never be
    // moved into itself.
    inline bool isInside(const std::string& path, const std::string& dir) {
        const auto P = Detail::stripSlashes(path), D = Detail::stripSlashes(dir);
        return P == D || (P.size() > D.size() && P.starts_with(D) && (D == "/" || P[D.size()] == '/'));
    }
}
