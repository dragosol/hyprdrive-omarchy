#pragma once

// Valve's KeyValues, both spellings: the text one of libraryfolders.vdf and
// appmanifest_*.acf, and the binary one of userdata/*/config/shortcuts.vdf.
// A document is a tree of Nodes; keys compare case-insensitively, as Steam's
// own reader does. Bounded: the input cap, Caps::DEPTH nesting and
// Caps::VDF_NODES keys -- anything past them, or malformed, is nullopt.

#include "Caps.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Hyprdrive::AppIdentity::Vdf {
    struct Node {
        enum class Type : uint8_t { Map, String, Int, UInt64, Float };
        std::string       key;
        Type              type = Type::Map;
        std::string       str;     // String (text values are all strings)
        uint64_t          num = 0; // Int (as uint32), UInt64
        std::vector<Node> children;

        const Node* child(std::string_view k) const {
            for (const auto& C : children)
                if (iequals(C.key, k))
                    return &C;
            return nullptr;
        }
        // A string child's value, "" when absent or not a string.
        std::string_view value(std::string_view k) const {
            const Node* C = child(k);
            return C && C->type == Type::String ? std::string_view{C->str} : std::string_view{};
        }
        // A number child: binary Int/UInt64, or a text value that is all digits.
        std::optional<uint64_t> number(std::string_view k) const {
            const Node* C = child(k);
            if (!C)
                return std::nullopt;
            if (C->type == Type::Int || C->type == Type::UInt64)
                return C->num;
            if (C->type != Type::String || !allDigits(C->str) || C->str.size() > 19)
                return std::nullopt;
            uint64_t v = 0;
            for (char c : C->str)
                v = v * 10 + static_cast<uint64_t>(c - '0');
            return v;
        }
    };

    namespace Detail {
        struct SText {
            Bytes  b;
            size_t i = 0, nodes = 0;
            enum ETok : uint8_t { END, STR, OPEN, CLOSE, BAD };

            void skip() {
                while (i < b.size()) {
                    const uint8_t C = b[i];
                    if (C == ' ' || C == '\t' || C == '\r' || C == '\n' || C == 0xEF || C == 0xBB || C == 0xBF) // a UTF-8 BOM too
                        ++i;
                    else if (C == '/' && i + 1 < b.size() && b[i + 1] == '/') {
                        while (i < b.size() && b[i] != '\n')
                            ++i;
                    } else if (C == '[') { // a platform conditional, [$WIN32]: ignored
                        while (i < b.size() && b[i] != ']' && b[i] != '\n')
                            ++i;
                        if (i < b.size() && b[i] == ']')
                            ++i;
                    } else
                        return;
                }
            }
            ETok next(std::string& out) {
                out.clear();
                skip();
                if (i >= b.size())
                    return END;
                const uint8_t C = b[i];
                if (C == '{') {
                    ++i;
                    return OPEN;
                }
                if (C == '}') {
                    ++i;
                    return CLOSE;
                }
                if (C == '"') {
                    for (++i; i < b.size() && b[i] != '"'; ++i) {
                        if (b[i] == '\\' && i + 1 < b.size()) {
                            const uint8_t E = b[++i];
                            out += E == 'n' ? '\n' : E == 't' ? '\t' : static_cast<char>(E);
                        } else
                            out += static_cast<char>(b[i]);
                    }
                    if (i >= b.size())
                        return BAD; // unterminated
                    ++i;
                    return STR;
                }
                while (i < b.size() && b[i] != ' ' && b[i] != '\t' && b[i] != '\r' && b[i] != '\n' && b[i] != '{' && b[i] != '}' && b[i] != '"')
                    out += static_cast<char>(b[i++]);
                return STR;
            }
            bool map(Node& into, int depth) {
                std::string key, val;
                for (;;) {
                    const ETok K = next(key);
                    if (K == CLOSE)
                        return depth > 0;
                    if (K == END)
                        return depth == 0;
                    if (K != STR)
                        return false;
                    if (++nodes > Caps::VDF_NODES)
                        return false;
                    const ETok V = next(val);
                    Node       n;
                    n.key = std::move(key);
                    if (V == STR) {
                        n.type = Node::Type::String;
                        n.str  = std::move(val);
                    } else if (V == OPEN) {
                        if (depth + 1 > Caps::DEPTH || !map(n, depth + 1))
                            return false;
                    } else
                        return false;
                    into.children.push_back(std::move(n));
                    key.clear();
                }
            }
        };

        struct SBinary {
            Bytes  b;
            size_t i = 0, nodes = 0;

            bool cstr(std::string& out) {
                if (i >= b.size())
                    return false;
                const auto* P = static_cast<const uint8_t*>(std::memchr(b.data() + i, 0, b.size() - i));
                if (!P)
                    return false;
                const size_t N = static_cast<size_t>(P - (b.data() + i));
                out.assign(reinterpret_cast<const char*>(b.data() + i), N);
                i += N + 1;
                return true;
            }
            bool map(Node& into, int depth) {
                for (;;) {
                    if (i >= b.size())
                        return false; // truncated: no end marker
                    const uint8_t T = b[i++];
                    if (T == 8 || T == 11) // end of this map (11: the alternative end some writers use)
                        return true;
                    if (++nodes > Caps::VDF_NODES)
                        return false;
                    Node n;
                    if (!cstr(n.key))
                        return false;
                    switch (T) {
                        case 0:
                            if (depth + 1 > Caps::DEPTH || !map(n, depth + 1))
                                return false;
                            break;
                        case 1:
                            n.type = Node::Type::String;
                            if (!cstr(n.str))
                                return false;
                            break;
                        case 2: // int32
                        case 3: // float32
                        case 4: // pointer
                        case 6: { // colour
                            uint32_t v = 0;
                            if (!rd(b, i, v))
                                return false;
                            i += 4;
                            n.type = T == 3 ? Node::Type::Float : Node::Type::Int;
                            n.num  = v;
                            break;
                        }
                        case 7:    // uint64
                        case 10: { // int64
                            uint64_t v = 0;
                            if (!rd(b, i, v))
                                return false;
                            i += 8;
                            n.type = Node::Type::UInt64;
                            n.num  = v;
                            break;
                        }
                        default: return false; // 5 (wide string) and anything unknown
                    }
                    into.children.push_back(std::move(n));
                }
            }
        };
    }

    // The text form: a Map whose children are the document's top-level pairs.
    inline std::optional<Node> parseText(Bytes in, size_t cap = Caps::VDF) {
        if (in.size() > cap)
            return std::nullopt;
        try {
            Detail::SText p{.b = in};
            Node          root;
            if (!p.map(root, 0))
                return std::nullopt;
            return root;
        } catch (...) { return std::nullopt; }
    }

    // The binary form. The whole document is one map whose end marker closes
    // it (shortcuts.vdf: `00 "shortcuts" ... 08 08`).
    inline std::optional<Node> parseBinary(Bytes in, size_t cap = Caps::SHORTCUTS) {
        if (in.size() > cap)
            return std::nullopt;
        try {
            Detail::SBinary p{.b = in};
            Node            root;
            if (!p.map(root, 0))
                return std::nullopt;
            return root;
        } catch (...) { return std::nullopt; }
    }
}
