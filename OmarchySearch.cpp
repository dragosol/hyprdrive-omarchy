#include "OmarchySearch.hpp"

#include <hyprutils/os/Process.hpp>
using namespace Hyprutils::OS;

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <regex>
#include <sstream>
#include <thread>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>

namespace fs = std::filesystem;

namespace OmarchySearch {

// ============================================================================
// Quick Look (GNOME Sushi) State Machine
// ============================================================================
static bool        g_sushiActive = false;
static pid_t       g_sushiPid = 0;
static std::string g_sushiPath;

bool isSushiActive() {
    return g_sushiActive;
}

bool isOverviewSushiWindow(const std::string& windowClass, uint32_t pid) {
    if (!g_sushiActive)
        return false;
    if (windowClass == "org.gnome.NautilusPreviewer")
        return true;
    if (pid > 0 && g_sushiPid > 0 && pid == static_cast<uint32_t>(g_sushiPid))
        return true;
    return false;
}

void closeSushi() {
    if (g_sushiActive) {
        // GNOME Sushi D-Bus Close method
        system("gdbus call --session --dest org.gnome.NautilusPreviewer --object-path /org/gnome/NautilusPreviewer --method org.gnome.NautilusPreviewer2.Close >/dev/null 2>&1 &");
        if (g_sushiPid > 0) {
            kill(g_sushiPid, SIGTERM);
            int status = 0;
            waitpid(g_sushiPid, &status, WNOHANG);
        }
    }
    g_sushiActive = false;
    g_sushiPid = 0;
    g_sushiPath.clear();
}

void toggleSushi(const std::string& filePath) {
    if (filePath.empty())
        return;

    if (g_sushiActive) {
        const bool same = (g_sushiPath == filePath);
        closeSushi();
        if (same)
            return; // Toggled off
    }

    g_sushiActive = true;
    g_sushiPath   = filePath;

    pid_t pid = fork();
    if (pid == 0) {
        execl("/usr/bin/sushi", "sushi", filePath.c_str(), nullptr);
        _exit(1);
    } else if (pid > 0) {
        g_sushiPid = pid;
    }
}

void updateSushiIfActive(const std::string& filePath) {
    if (g_sushiActive && !filePath.empty() && g_sushiPath != filePath) {
        toggleSushi(filePath);
    }
}

// ============================================================================
// Instant Calculator Evaluator
// ============================================================================
namespace Calc {
    struct Token {
        enum Type { Number, Plus, Minus, Mul, Div, Mod, Pow, LParen, RParen, Ident, End } type = End;
        double      numValue = 0.0;
        std::string strValue;
    };

    static std::vector<Token> tokenize(const std::string& expr) {
        std::vector<Token> tokens;
        size_t i = 0;
        while (i < expr.size()) {
            if (std::isspace(static_cast<unsigned char>(expr[i]))) {
                ++i;
                continue;
            }
            char c = expr[i];
            if (c == '+') { tokens.push_back({Token::Plus, 0, "+"}); ++i; }
            else if (c == '-') { tokens.push_back({Token::Minus, 0, "-"}); ++i; }
            else if (c == '*' || c == 'x' || c == 'X') {
                // Check if 'x' is an operator or identifier
                if ((c == 'x' || c == 'X') && !tokens.empty() && 
                    (tokens.back().type == Token::Number || tokens.back().type == Token::RParen)) {
                    tokens.push_back({Token::Mul, 0, "*"});
                    ++i;
                } else if (c == '*') {
                    tokens.push_back({Token::Mul, 0, "*"});
                    ++i;
                } else {
                    goto parse_ident;
                }
            }
            else if (c == '/') { tokens.push_back({Token::Div, 0, "/"}); ++i; }
            else if (c == '%') { tokens.push_back({Token::Mod, 0, "%"}); ++i; }
            else if (c == '^') { tokens.push_back({Token::Pow, 0, "^"}); ++i; }
            else if (c == '(') { tokens.push_back({Token::LParen, 0, "("}); ++i; }
            else if (c == ')') { tokens.push_back({Token::RParen, 0, ")"}); ++i; }
            else if (std::isdigit(static_cast<unsigned char>(c)) || c == '.') {
                size_t next = i;
                double val = std::stod(expr.substr(i), &next);
                tokens.push_back({Token::Number, val, expr.substr(i, next)});
                i += next;
            } else if (std::isalpha(static_cast<unsigned char>(c))) {
            parse_ident:
                size_t start = i;
                while (i < expr.size() && (std::isalnum(static_cast<unsigned char>(expr[i])) || expr[i] == '_'))
                    ++i;
                std::string ident = expr.substr(start, i - start);
                std::string lower;
                for (char ch : ident) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                tokens.push_back({Token::Ident, 0, lower});
            } else {
                return {}; // Invalid character
            }
        }
        tokens.push_back({Token::End, 0, ""});
        return tokens;
    }

    struct Parser {
        const std::vector<Token>& tokens;
        size_t pos = 0;

        const Token& peek() const {
            if (pos < tokens.size()) return tokens[pos];
            static Token endTok{Token::End, 0, ""};
            return endTok;
        }

        Token get() {
            const Token& t = peek();
            if (pos < tokens.size()) ++pos;
            return t;
        }

        bool match(Token::Type t) {
            if (peek().type == t) {
                get();
                return true;
            }
            return false;
        }

        // expr = add
        // add  = mul (('+'|'-') mul)*
        // mul  = unary (('*'|'/'|'%') unary)*
        // unary = ('+'|'-') unary | power
        // power = primary ('^' unary)?
        // primary = number | ident | '(' expr ')' | ident '(' expr ')'

        double parseExpr() { return parseAdd(); }

        double parseAdd() {
            double v = parseMul();
            while (peek().type == Token::Plus || peek().type == Token::Minus) {
                Token op = get();
                double rhs = parseMul();
                if (op.type == Token::Plus) v += rhs;
                else v -= rhs;
            }
            return v;
        }

        double parseMul() {
            double v = parseUnary();
            while (peek().type == Token::Mul || peek().type == Token::Div || peek().type == Token::Mod) {
                Token op = get();
                double rhs = parseUnary();
                if (op.type == Token::Mul) v *= rhs;
                else if (op.type == Token::Div) {
                    if (std::abs(rhs) < 1e-15) throw std::runtime_error("div by zero");
                    v /= rhs;
                } else if (op.type == Token::Mod) {
                    if (std::abs(rhs) < 1e-15) throw std::runtime_error("mod by zero");
                    v = std::fmod(v, rhs);
                }
            }
            return v;
        }

        double parseUnary() {
            if (match(Token::Plus)) return parseUnary();
            if (match(Token::Minus)) return -parseUnary();
            return parsePower();
        }

        double parsePower() {
            double v = parsePrimary();
            if (match(Token::Pow)) {
                double rhs = parseUnary();
                v = std::pow(v, rhs);
            }
            return v;
        }

        double parsePrimary() {
            Token t = get();
            if (t.type == Token::Number) return t.numValue;
            if (t.type == Token::LParen) {
                double v = parseExpr();
                if (!match(Token::RParen)) throw std::runtime_error("missing )");
                return v;
            }
            if (t.type == Token::Ident) {
                if (t.strValue == "pi") return M_PI;
                if (t.strValue == "e") return M_E;
                // Functions
                if (match(Token::LParen)) {
                    double arg = parseExpr();
                    if (!match(Token::RParen)) throw std::runtime_error("missing )");
                    if (t.strValue == "sqrt") {
                        if (arg < 0) throw std::runtime_error("sqrt of negative");
                        return std::sqrt(arg);
                    }
                    if (t.strValue == "cbrt") return std::cbrt(arg);
                    if (t.strValue == "abs") return std::abs(arg);
                    if (t.strValue == "sin") return std::sin(arg);
                    if (t.strValue == "cos") return std::cos(arg);
                    if (t.strValue == "tan") return std::tan(arg);
                    if (t.strValue == "log" || t.strValue == "log10") return std::log10(arg);
                    if (t.strValue == "ln") return std::log(arg);
                    if (t.strValue == "exp") return std::exp(arg);
                    if (t.strValue == "floor") return std::floor(arg);
                    if (t.strValue == "ceil") return std::ceil(arg);
                    if (t.strValue == "round") return std::round(arg);
                    throw std::runtime_error("unknown func");
                }
                throw std::runtime_error("unknown ident");
            }
            throw std::runtime_error("syntax error");
        }
    };
} // namespace Calc

std::optional<SSearchItem> evaluateCalculator(const std::string& rawQuery) {
    if (rawQuery.empty())
        return std::nullopt;

    std::string query = rawQuery;
    // Trim leading whitespace
    while (!query.empty() && std::isspace(static_cast<unsigned char>(query.front())))
        query.erase(query.begin());
    while (!query.empty() && std::isspace(static_cast<unsigned char>(query.back())))
        query.pop_back();

    bool hasPrefix = false;
    if (!query.empty() && query.front() == '=') {
        hasPrefix = true;
        query.erase(query.begin());
    }

    if (query.empty())
        return std::nullopt;

    // Check if it looks like a math expression
    bool hasOperator = false;
    bool hasDigit    = false;
    for (char c : query) {
        if (std::isdigit(static_cast<unsigned char>(c)))
            hasDigit = true;
        if (c == '+' || c == '-' || c == '*' || c == '/' || c == '%' || c == '^' || c == '(' || c == ')')
            hasOperator = true;
    }

    if (!hasPrefix && (!hasOperator || !hasDigit))
        return std::nullopt;

    try {
        auto tokens = Calc::tokenize(query);
        if (tokens.size() <= 1)
            return std::nullopt;

        Calc::Parser parser{tokens};
        double val = parser.parseExpr();
        if (parser.peek().type != Calc::Token::End)
            return std::nullopt;

        if (!std::isfinite(val))
            return std::nullopt;

        char buf[64];
        if (std::abs(val - std::round(val)) < 1e-9 && std::abs(val) < 1e14) {
            std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(std::round(val)));
        } else {
            std::snprintf(buf, sizeof(buf), "%.8g", val);
        }

        std::string resultStr = buf;

        SSearchItem item;
        item.kind          = EItemKind::Calculator;
        item.id            = "calc:" + resultStr;
        item.name          = "= " + resultStr;
        item.description   = query + " (Press Enter to copy result)";
        item.icon          = "accessories-calculator";
        item.category      = "Calculator";
        item.actionCommand = resultStr;
        return item;
    } catch (...) {
        return std::nullopt;
    }
}

// ============================================================================
// Agent Chat Commands
// ============================================================================
struct SAgentDef {
    std::string prefix;
    std::string name;
    std::string model;
    std::string description;
    std::string commandTemplate; // e.g. "omarchy-launch-tui --app-id=org.omarchy.agent grok --permission-mode bypassPermissions -- %P"
};

static const std::vector<SAgentDef>& agentDefs() {
    static const std::vector<SAgentDef> AGENTS = {
        {"grok", "Grok", "xAI Grok", "Direct chat with Grok via CLI",
         "omarchy-launch-tui --app-id=org.omarchy.agent grok --permission-mode bypassPermissions -- \"%P%\""},
        {"agy", "Antigravity", "Google DeepMind AGY", "DeepMind agentic coding CLI with yolo permissions",
         "omarchy-launch-tui --app-id=org.omarchy.agent agy --dangerously-skip-permissions --prompt-interactive \"%P%\""},
        {"gemini", "Gemini", "Google Gemini", "Gemini CLI interactive prompt",
         "omarchy-launch-tui --app-id=org.omarchy.agent gemini --yolo --prompt-interactive \"%P%\""},
        {"claude", "Claude", "Anthropic Claude", "Claude Code interactive agent",
         "omarchy-launch-tui --app-id=org.omarchy.agent claude --permission-mode auto -- \"%P%\""},
        {"opencode", "OpenCode", "OpenCode AI", "Open-source coding assistant",
         "omarchy-launch-tui --app-id=org.omarchy.agent opencode --auto --prompt \"%P%\""},
        {"codex", "Codex", "Codex CLI", "AI terminal companion",
         "omarchy-launch-tui --app-id=org.omarchy.agent codex --approve-for-me -- \"%P%\""},
        {"copilot", "Copilot", "GitHub Copilot", "GitHub Copilot in the CLI",
         "omarchy-launch-tui --app-id=org.omarchy.agent copilot --allow-all --interactive \"%P%\""},
        {"crush", "Crush", "Crush CLI", "Fast command runner agent",
         "omarchy-launch-tui --app-id=org.omarchy.agent crush run \"%P%\""},
        {"pi", "Pi", "Inflection Pi", "Conversational AI assistant",
         "omarchy-launch-tui --app-id=org.omarchy.agent pi \"%P%\""},
        {"agent", "Default Agent", "Omarchy Default Agent", "Configured default coding agent",
         "omarchy-agent --prompt \"%P%\""},
    };
    return AGENTS;
}

bool isAgentQuery(const std::string& query) {
    if (query.empty())
        return false;
    return query[0] == '/';
}

static std::string replaceAll(std::string str, const std::string& from, const std::string& to) {
    size_t start_pos = 0;
    while ((start_pos = str.find(from, start_pos)) != std::string::npos) {
        str.replace(start_pos, from.length(), to);
        start_pos += to.length();
    }
    return str;
}

std::vector<SSearchItem> searchAgents(const std::string& rawQuery) {
    std::vector<SSearchItem> items;
    if (rawQuery.empty() || rawQuery[0] != '/')
        return items;

    std::string rest = rawQuery.substr(1);
    std::string token;
    std::string prompt;
    size_t space = rest.find(' ');
    if (space != std::string::npos) {
        token  = rest.substr(0, space);
        prompt = rest.substr(space + 1);
        while (!prompt.empty() && prompt.front() == ' ')
            prompt.erase(prompt.begin());
    } else {
        token = rest;
    }

    std::string lowerToken;
    for (char c : token) lowerToken += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    const auto& DEFS = agentDefs();
    for (const auto& AG : DEFS) {
        if (!lowerToken.empty() && AG.prefix.find(lowerToken) != 0 && lowerToken != AG.prefix)
            continue;

        SSearchItem item;
        item.kind     = EItemKind::Agent;
        item.id       = "agent:" + AG.prefix;
        item.category = "AI Agent";
        item.icon     = "utilities-terminal";

        if (prompt.empty()) {
            item.name        = "/" + AG.prefix + " <prompt>";
            item.description = AG.name + " (" + AG.model + ") - " + AG.description;
            item.actionCommand = replaceAll(AG.commandTemplate, "\"%P%\"", "");
            item.actionCommand = replaceAll(item.actionCommand, "%P%", "");
        } else {
            item.name        = "Ask " + AG.name;
            item.description = prompt;
            // Escape any quotes in prompt for safety
            std::string escPrompt = replaceAll(prompt, "\"", "\\\"");
            item.actionCommand = replaceAll(AG.commandTemplate, "%P%", escPrompt);
        }

        items.push_back(std::move(item));
    }

    return items;
}

// ============================================================================
// Omarchy Menu JSONC Parser & Provider
// ============================================================================
static std::string stripJsoncComments(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    bool inQuote = false;
    bool inLineComment = false;
    bool inBlockComment = false;

    for (size_t i = 0; i < in.size(); ++i) {
        char c = in[i];
        if (inLineComment) {
            if (c == '\n') {
                inLineComment = false;
                out += c;
            }
            continue;
        }
        if (inBlockComment) {
            if (c == '*' && i + 1 < in.size() && in[i + 1] == '/') {
                inBlockComment = false;
                ++i;
            }
            continue;
        }
        if (inQuote) {
            out += c;
            if (c == '\\' && i + 1 < in.size()) {
                out += in[++i];
            } else if (c == '"') {
                inQuote = false;
            }
            continue;
        }

        if (c == '"') {
            inQuote = true;
            out += c;
            continue;
        }
        if (c == '/' && i + 1 < in.size()) {
            if (in[i + 1] == '/') {
                inLineComment = true;
                ++i;
                continue;
            }
            if (in[i + 1] == '*') {
                inBlockComment = true;
                ++i;
                continue;
            }
        }
        out += c;
    }
    return out;
}

struct SRawMenuItem {
    std::string id;
    std::string parent;
    std::string kind; // "action", "menu", "link"
    std::string label;
    std::string title;
    std::string target;
    std::string description;
    std::string action;
    std::string icon;
    std::string iconFont;
    std::string provider;
    std::string when;
    std::string checked;
    std::vector<std::string> aliases;
    std::string category = "Omarchy Menu";
    int order = 0;
};

static std::unordered_map<std::string, SRawMenuItem> g_menuItems;
static std::vector<std::string> g_menuItemOrder;
static std::string g_menuCurrentRoute = "root";
static std::vector<std::string> g_menuNavStack;
static time_t g_menuSystemMtime = 0;
static time_t g_menuUserMtime = 0;
static std::mutex g_menuMutex;

void navigateMenu(const std::string& routeId) {
    std::lock_guard<std::mutex> lock(g_menuMutex);
    if (routeId.empty()) return;
    if (routeId != g_menuCurrentRoute) {
        g_menuNavStack.push_back(g_menuCurrentRoute);
        g_menuCurrentRoute = routeId;
    }
}

bool navigateMenuBack() {
    std::lock_guard<std::mutex> lock(g_menuMutex);
    if (!g_menuNavStack.empty()) {
        g_menuCurrentRoute = g_menuNavStack.back();
        g_menuNavStack.pop_back();
        return true;
    }
    if (g_menuCurrentRoute != "root") {
        auto it = g_menuItems.find(g_menuCurrentRoute);
        if (it != g_menuItems.end() && !it->second.parent.empty()) {
            g_menuCurrentRoute = it->second.parent;
        } else {
            g_menuCurrentRoute = "root";
        }
        return true;
    }
    return false;
}

bool canNavigateMenuBack() {
    std::lock_guard<std::mutex> lock(g_menuMutex);
    return g_menuCurrentRoute != "root" || !g_menuNavStack.empty();
}

std::string menuCurrentRoute() {
    std::lock_guard<std::mutex> lock(g_menuMutex);
    return g_menuCurrentRoute;
}

std::string menuCurrentRouteTitle() {
    std::lock_guard<std::mutex> lock(g_menuMutex);
    if (g_menuCurrentRoute == "root")
        return "Omarchy Menu";
    auto it = g_menuItems.find(g_menuCurrentRoute);
    if (it != g_menuItems.end())
        return it->second.label;
    return g_menuCurrentRoute;
}

void resetMenuRoute() {
    std::lock_guard<std::mutex> lock(g_menuMutex);
    g_menuCurrentRoute = "root";
    g_menuNavStack.clear();
}

static std::string pathFor(const std::unordered_map<std::string, SRawMenuItem>& items, const std::string& id) {
    std::vector<std::string> labels;
    std::string curId = id;
    int guard = 0;
    while (!curId.empty() && curId != "root" && guard++ < 16) {
        auto it = items.find(curId);
        if (it == items.end()) break;
        labels.push_back(it->second.label);
        curId = it->second.parent;
    }
    std::reverse(labels.begin(), labels.end());
    std::string result;
    for (size_t i = 0; i < labels.size(); ++i) {
        if (i > 0) result += " › ";
        result += labels[i];
    }
    return result;
}

static std::string parentPathFor(const std::unordered_map<std::string, SRawMenuItem>& items, const std::string& id) {
    auto it = items.find(id);
    if (it == items.end() || it->second.parent.empty() || it->second.parent == "root")
        return "";
    return pathFor(items, it->second.parent);
}

static bool evaluateWhen(const std::string& when) {
    if (when.empty()) return true;
    static std::unordered_map<std::string, bool> memo;
    static std::chrono::steady_clock::time_point lastCheck;
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::seconds>(now - lastCheck).count() > 10) {
        memo.clear();
        lastCheck = now;
    }
    auto it = memo.find(when);
    if (it != memo.end()) return it->second;
    std::string cmd = "bash -c \"" + when + "\" >/dev/null 2>&1";
    bool ok = (system(cmd.c_str()) == 0);
    memo[when] = ok;
    return ok;
}

static void parseMenuFile(const std::string& path, 
                          std::unordered_map<std::string, SRawMenuItem>& outMap, 
                          std::vector<std::string>& outOrder) {
    std::ifstream f(path);
    if (!f) return;

    std::stringstream buffer;
    buffer << f.rdbuf();
    std::string clean = stripJsoncComments(buffer.str());

    size_t pos = 0;
    const size_t len = clean.size();

    auto skipWs = [&]() {
        while (pos < len && (std::isspace(static_cast<unsigned char>(clean[pos])) || clean[pos] == ','))
            ++pos;
    };

    auto parseString = [&]() -> std::string {
        if (pos >= len || clean[pos] != '"') return "";
        ++pos; // skip opening "
        std::string s;
        while (pos < len) {
            char c = clean[pos++];
            if (c == '\\' && pos < len) {
                char esc = clean[pos++];
                if (esc == '"') s += '"';
                else if (esc == '\\') s += '\\';
                else if (esc == 'n') s += '\n';
                else if (esc == 't') s += '\t';
                else { s += '\\'; s += esc; }
            } else if (c == '"') {
                break;
            } else {
                s += c;
            }
        }
        return s;
    };

    while (pos < len && clean[pos] != '{') ++pos;
    if (pos >= len) return;
    ++pos; // skip '{'

    while (pos < len) {
        skipWs();
        if (pos >= len || clean[pos] == '}') break;

        if (clean[pos] != '"') {
            ++pos;
            continue;
        }
        std::string id = parseString();
        skipWs();
        if (pos >= len || clean[pos] != ':') continue;
        ++pos; // skip ':'
        skipWs();

        if (pos >= len || clean[pos] != '{') continue;
        ++pos; // skip '{'

        SRawMenuItem item;
        item.id = id;

        while (pos < len) {
            skipWs();
            if (pos >= len || clean[pos] == '}') {
                if (pos < len) ++pos;
                break;
            }
            if (clean[pos] != '"') {
                ++pos;
                continue;
            }
            std::string propKey = parseString();
            skipWs();
            if (pos >= len || clean[pos] != ':') continue;
            ++pos; // skip ':'
            skipWs();

            if (pos < len && clean[pos] == '"') {
                std::string propVal = parseString();
                if (propKey == "label") item.label = propVal;
                else if (propKey == "icon") item.icon = propVal;
                else if (propKey == "action") item.action = propVal;
                else if (propKey == "description") item.description = propVal;
                else if (propKey == "target") item.target = propVal;
                else if (propKey == "provider") item.provider = propVal;
                else if (propKey == "when") item.when = propVal;
                else if (propKey == "checked") item.checked = propVal;
                else if (propKey == "parent") item.parent = propVal;
                else if (propKey == "title") item.title = propVal;
                else if (propKey == "iconFont") item.iconFont = propVal;
            } else if (pos < len && clean[pos] == '[') {
                ++pos; // skip '['
                while (pos < len) {
                    skipWs();
                    if (pos >= len || clean[pos] == ']') {
                        if (pos < len) ++pos;
                        break;
                    }
                    if (clean[pos] == '"') {
                        std::string elem = parseString();
                        if (propKey == "aliases") item.aliases.push_back(elem);
                    } else {
                        ++pos;
                    }
                }
            } else {
                int depth = 0;
                while (pos < len) {
                    char c = clean[pos];
                    if (c == '{' || c == '[') ++depth;
                    else if (c == '}' || c == ']') {
                        if (depth == 0) break;
                        --depth;
                    } else if (c == ',' && depth == 0) break;
                    ++pos;
                }
            }
        }

        if (item.parent.empty()) {
            size_t lastDot = item.id.rfind('.');
            if (lastDot != std::string::npos) {
                item.parent = item.id.substr(0, lastDot);
            } else {
                item.parent = "root";
            }
        }
        if (item.id == "root")
            item.parent = "";

        if (!item.action.empty())
            item.kind = "action";
        else if (!item.target.empty())
            item.kind = "link";
        else
            item.kind = "menu";

        if (item.label.empty())
            item.label = item.id;

        item.category = "Omarchy Menu";

        if (outMap.find(item.id) == outMap.end()) {
            item.order = static_cast<int>(outOrder.size());
            outOrder.push_back(item.id);
        } else {
            item.order = outMap[item.id].order;
        }
        outMap[item.id] = item;
    }
}

static void reloadMenuIfNeeded() {
    std::lock_guard<std::mutex> lock(g_menuMutex);

    const std::string sysPath = "/usr/share/omarchy/default/omarchy/omarchy-menu.jsonc";
    const char* home = std::getenv("HOME");
    const std::string userPath = home ? std::string(home) + "/.config/omarchy/extensions/omarchy-menu.jsonc" : "";

    struct stat stSys {};
    struct stat stUser {};
    time_t sysMtime = (stat(sysPath.c_str(), &stSys) == 0) ? stSys.st_mtime : 0;
    time_t userMtime = (!userPath.empty() && stat(userPath.c_str(), &stUser) == 0) ? stUser.st_mtime : 0;

    if (sysMtime == g_menuSystemMtime && userMtime == g_menuUserMtime && !g_menuItems.empty())
        return;

    g_menuItems.clear();
    g_menuItemOrder.clear();
    parseMenuFile(sysPath, g_menuItems, g_menuItemOrder);
    if (!userPath.empty())
        parseMenuFile(userPath, g_menuItems, g_menuItemOrder);

    g_menuSystemMtime = sysMtime;
    g_menuUserMtime   = userMtime;
}

std::vector<SSearchItem> searchMenu(const std::string& rawQuery, size_t limit) {
    reloadMenuIfNeeded();
    std::lock_guard<std::mutex> lock(g_menuMutex);

    std::vector<SSearchItem> matches;

    if (rawQuery.empty()) {
        // Browse mode: show children of g_menuCurrentRoute
        if (g_menuCurrentRoute != "root") {
            SSearchItem backItem;
            backItem.kind        = EItemKind::Back;
            backItem.id          = "__back__";
            backItem.name        = "‹ Back";
            backItem.category    = "Omarchy Menu";
            backItem.icon        = "go-previous";

            std::string parentTitle = "Omarchy Menu";
            auto curIt = g_menuItems.find(g_menuCurrentRoute);
            if (curIt != g_menuItems.end() && !curIt->second.parent.empty() && curIt->second.parent != "root") {
                auto pIt = g_menuItems.find(curIt->second.parent);
                if (pIt != g_menuItems.end())
                    parentTitle = pIt->second.label;
            }
            backItem.description = "Up to " + parentTitle;
            matches.push_back(std::move(backItem));
        }

        for (const auto& id : g_menuItemOrder) {
            auto it = g_menuItems.find(id);
            if (it == g_menuItems.end()) continue;
            const auto& raw = it->second;

            if (raw.parent != g_menuCurrentRoute) continue;
            if (!raw.when.empty() && !evaluateWhen(raw.when)) continue;

            SSearchItem item;
            item.id       = raw.id;
            item.name     = raw.label;
            item.category = "Omarchy Menu";

            if (raw.kind == "menu") {
                item.kind        = EItemKind::Menu;
                item.icon        = raw.icon.empty() ? "folder" : raw.icon;
                item.description = raw.description.empty() ? "Submenu ›" : raw.description + " ›";
            } else if (raw.kind == "link") {
                item.kind        = EItemKind::Menu;
                item.id          = raw.target.empty() ? raw.id : raw.target;
                item.icon        = raw.icon.empty() ? "folder" : raw.icon;
                item.description = "Submenu ›";
            } else {
                item.kind          = EItemKind::Action;
                item.icon          = raw.icon.empty() ? "system-run" : raw.icon;
                item.description   = raw.description.empty() ? raw.action : raw.description;
                item.actionCommand = raw.action;
            }

            matches.push_back(std::move(item));
            if (matches.size() >= limit) break;
        }

        if (matches.empty()) {
            static const std::vector<std::pair<std::string, std::pair<std::string, std::string>>> ROOT_DEFAULTS = {
                {"Lock Screen", {"system.lock", "omarchy-system-lock"}},
                {"Screenshot", {"trigger.capture.screenshot", "omarchy-capture-screenshot"}},
                {"Nightlight", {"trigger.toggle.nightlight", "omarchy-toggle-nightlight"}},
                {"Theme Switcher", {"style.theme", "omarchy-theme-switcher"}},
                {"Shutdown", {"system.shutdown", "omarchy-system-shutdown"}},
            };
            for (const auto& [name, meta] : ROOT_DEFAULTS) {
                SSearchItem it;
                it.kind          = EItemKind::Action;
                it.id            = meta.first;
                it.name          = name;
                it.actionCommand = meta.second;
                it.icon          = "system-run";
                it.category      = "Omarchy Menu";
                it.description   = meta.second;
                matches.push_back(std::move(it));
            }
        }

        return matches;
    }

    // Search mode: match query across all items
    std::string query;
    for (char c : rawQuery) query += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    struct ScoredItem {
        int score = 0;
        const SRawMenuItem* item = nullptr;
    };
    std::vector<ScoredItem> scored;

    for (const auto& [_, it] : g_menuItems) {
        if (it.id == "root") continue;
        if (!it.when.empty() && !evaluateWhen(it.when)) continue;

        int score = 0;
        std::string lowerLabel;
        for (char c : it.label) lowerLabel += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        if (lowerLabel == query) score += 120;
        else if (lowerLabel.find(query) == 0) score += 70;
        else if (lowerLabel.find(query) != std::string::npos) score += 40;

        for (const auto& al : it.aliases) {
            std::string lowerAl;
            for (char c : al) lowerAl += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (lowerAl == query) score += 100;
            else if (lowerAl.find(query) == 0) score += 60;
            else if (lowerAl.find(query) != std::string::npos) score += 30;
        }

        std::string lowerId;
        for (char c : it.id) lowerId += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lowerId.find(query) != std::string::npos) score += 20;

        if (!it.description.empty()) {
            std::string lowerDesc;
            for (char c : it.description) lowerDesc += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (lowerDesc.find(query) != std::string::npos) score += 15;
        }

        if (score > 0) {
            scored.push_back({score, &it});
        }
    }

    std::sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.item->order < b.item->order;
    });

    for (size_t i = 0; i < std::min(limit, scored.size()); ++i) {
        const auto* raw = scored[i].item;
        SSearchItem item;
        item.id   = raw->id;
        item.name = raw->label;
        item.category = "Omarchy Menu";
        item.icon = raw->icon.empty() ? "system-run" : raw->icon;

        std::string breadcrumb = parentPathFor(g_menuItems, raw->id);

        if (raw->kind == "menu" || raw->kind == "link") {
            item.kind        = EItemKind::Menu;
            item.description = breadcrumb.empty() ? "Submenu" : breadcrumb + " (Submenu)";
            if (raw->kind == "link" && !raw->target.empty()) item.id = raw->target;
        } else {
            item.kind          = EItemKind::Action;
            item.description   = breadcrumb.empty() ? raw->description : breadcrumb;
            item.actionCommand = raw->action;
        }

        matches.push_back(std::move(item));
    }

    return matches;
}

// ============================================================================
// Asynchronous File Search (`fd`)
// ============================================================================
static std::thread             g_fileThread;
static std::atomic<bool>       g_fileRunning{false};
static std::atomic<uint64_t>   g_fileGen{0};
static std::mutex              g_fileWorkerMutex;
static std::condition_variable g_fileWorkerCv;
static std::string             g_fileQueryText;

static std::mutex              g_fileResultsMutex;
static std::vector<SSearchItem> g_fileResults;

static std::string formatBytes(uint64_t bytes) {
    if (bytes < 1024) return std::to_string(bytes) + " B";
    if (bytes < 1024 * 1024) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1f KB", static_cast<double>(bytes) / 1024.0);
        return buf;
    }
    if (bytes < 1024 * 1024 * 1024) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
        return buf;
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f GB", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    return buf;
}

static void categorizeFile(const std::string& path, SSearchItem& item) {
    size_t dot = path.find_last_of('.');
    std::string ext = (dot != std::string::npos) ? path.substr(dot + 1) : "";
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    struct stat st {};
    if (stat(path.c_str(), &st) == 0) {
        item.fileSizeBytes = st.st_size;
        item.fileSizeStr   = formatBytes(st.st_size);
        char dateBuf[64];
        std::strftime(dateBuf, sizeof(dateBuf), "%b %d, %Y", std::localtime(&st.st_mtime));
        item.fileModifiedStr = dateBuf;
        if (S_ISDIR(st.st_mode)) {
            item.icon        = "folder";
            item.fileTypeStr = "Folder";
            return;
        }
    }

    item.canQuickLook = true;

    // Categorization
    if (ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "webp" || ext == "svg" || ext == "gif" || ext == "avif" || ext == "bmp") {
        item.icon = "image-x-generic";
        item.fileTypeStr = ext + " Image";
    } else if (ext == "mp4" || ext == "mkv" || ext == "webm" || ext == "avi" || ext == "mov" || ext == "flv") {
        item.icon = "video-x-generic";
        item.fileTypeStr = ext + " Video";
    } else if (ext == "mp3" || ext == "flac" || ext == "wav" || ext == "ogg" || ext == "m4a" || ext == "opus") {
        item.icon = "audio-x-generic";
        item.fileTypeStr = ext + " Audio";
    } else if (ext == "pdf") {
        item.icon = "application-pdf";
        item.fileTypeStr = "PDF Document";
    } else if (ext == "docx" || ext == "doc" || ext == "odt" || ext == "rtf") {
        item.icon = "x-office-document";
        item.fileTypeStr = "Word Document";
    } else if (ext == "xlsx" || ext == "xls" || ext == "csv" || ext == "tsv") {
        item.icon = "x-office-spreadsheet";
        item.fileTypeStr = "Spreadsheet";
    } else if (ext == "md" || ext == "markdown" || ext == "txt") {
        item.icon = "text-x-generic";
        item.fileTypeStr = "Text Document";
    } else if (ext == "cpp" || ext == "hpp" || ext == "c" || ext == "h" || ext == "rs" || ext == "go" || ext == "py" || ext == "js" || ext == "ts" || ext == "sh" || ext == "lua") {
        item.icon = "text-x-script";
        item.fileTypeStr = ext + " Source";
    } else if (ext == "zip" || ext == "tar" || ext == "gz" || ext == "7z" || ext == "rar" || ext == "zst") {
        item.icon = "package-x-generic";
        item.fileTypeStr = "Archive";
    } else {
        item.icon = "text-plain";
        item.fileTypeStr = ext.empty() ? "File" : ext + " File";
    }
}

static int scoreTerm(const std::string& name, const std::string& term) {
    if (name == term) return 0;
    if (name.find(term) == 0) return 1;
    auto idx = name.find(term);
    if (idx != std::string::npos && idx > 0) {
        char prev = name[idx - 1];
        if (prev == ' ' || prev == '-' || prev == '_' || prev == '.' || prev == '/') return 2;
        return 3;
    }
    return -1;
}

static int scoreFileItem(const std::string& name, const std::string& path, const std::vector<std::string>& terms) {
    std::string lowerName;
    for (char c : name) lowerName += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::string lowerPath;
    for (char c : path) lowerPath += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    int total = 0;
    for (const auto& term : terms) {
        int s = scoreTerm(lowerName, term);
        if (s < 0) {
            if (lowerPath.find("/" + term) != std::string::npos || lowerPath.find("/." + term) != std::string::npos ||
                lowerPath.find(" " + term) != std::string::npos || lowerPath.find("_" + term) != std::string::npos ||
                lowerPath.find("-" + term) != std::string::npos) s = 4;
            else if (lowerPath.find(term) != std::string::npos) s = 5;
            else return -1;
        }
        total += s;
    }
    return total;
}

static void fileWorkerLoop() {
    static const char* OMARCHY_FIND_EXCLUDES[] = {
        ".git", "node_modules", ".cache", "__pycache__", ".npm", ".nvm",
        ".cargo", ".rustup", ".gradle", ".local/share/Trash",
        ".local/share/flatpak", ".venv", "venv", ".steam",
        "*Cache*", "Local Storage", "Session Storage", "Service Worker",
        "blob_storage", "Dawn*", "logs", "*Storage*", "History",
        "data", "Crashpad", "Dictionaries", "Partitions",
        "Shared Dictionary", "Backups", "google-chrome*",
        "microsoft-edge*", "chromium", "BraveSoftware", "mozilla",
        "dconf", "ibus", "fcitx", "pulse", "procps", ".*.bak.*"
    };

    while (g_fileRunning) {
        std::string queryStr;
        uint64_t    gen = 0;
        {
            std::unique_lock<std::mutex> lock(g_fileWorkerMutex);
            g_fileWorkerCv.wait(lock, [&]() {
                return !g_fileRunning || g_fileGen > gen;
            });
            if (!g_fileRunning) break;
            queryStr = g_fileQueryText;
            gen      = g_fileGen.load();
        }

        if (queryStr.size() < 2 || queryStr[0] == '/') {
            std::lock_guard<std::mutex> lock(g_fileResultsMutex);
            g_fileResults.clear();
            continue;
        }

        const char* home = std::getenv("HOME");
        if (!home) continue;

        std::vector<std::string> terms;
        std::istringstream iss(queryStr);
        std::string token;
        while (iss >> token) {
            std::string lower;
            for (char c : token) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            terms.push_back(lower);
        }

        if (terms.empty())
            continue;

        // Build fd command identical to Omarchy Find (FindBackend.js)
        std::string cmd = "fd --color=never -i --no-ignore --follow --max-results 80 --full-path";
        for (const auto* exc : OMARCHY_FIND_EXCLUDES) {
            cmd += " -E '";
            cmd += exc;
            cmd += "'";
        }

        if (terms.size() > 1) {
            for (size_t t = 1; t < terms.size(); ++t) {
                cmd += " --and '" + terms[t] + "'";
            }
        }
        cmd += " -- '" + terms[0] + "' '" + home + "'";

        FILE* pipe = popen(cmd.c_str(), "r");
        if (!pipe) continue;

        struct ScoredFile {
            int score = 0;
            SSearchItem item;
        };
        std::vector<ScoredFile> scoredFiles;
        char lineBuf[2048];
        while (fgets(lineBuf, sizeof(lineBuf), pipe)) {
            if (gen != g_fileGen.load()) break; // Query changed, abandon

            size_t len = std::strlen(lineBuf);
            while (len > 0 && (lineBuf[len - 1] == '\n' || lineBuf[len - 1] == '\r'))
                lineBuf[--len] = '\0';
            if (len == 0) continue;

            std::string path = lineBuf;
            size_t slash = path.find_last_of('/');
            std::string name = (slash != std::string::npos) ? path.substr(slash + 1) : path;
            std::string dir  = (slash != std::string::npos) ? path.substr(0, slash) : "";

            int score = scoreFileItem(name, path, terms);
            if (score < 0)
                score = 10;

            SSearchItem item;
            item.kind          = EItemKind::File;
            item.id            = "file:" + path;
            item.name          = name;
            item.description   = dir;
            item.filePath      = path;
            item.category      = "Files & Folders";
            item.actionCommand = path;

            categorizeFile(path, item);
            scoredFiles.push_back({score, std::move(item)});
        }
        pclose(pipe);

        std::sort(scoredFiles.begin(), scoredFiles.end(), [](const auto& a, const auto& b) {
            return a.score < b.score;
        });

        std::vector<SSearchItem> results;
        for (size_t i = 0; i < std::min<size_t>(30, scoredFiles.size()); ++i) {
            results.push_back(std::move(scoredFiles[i].item));
        }

        if (gen == g_fileGen.load()) {
            std::lock_guard<std::mutex> lock(g_fileResultsMutex);
            g_fileResults = std::move(results);
        }
    }
}

std::vector<SSearchItem> fileResults() {
    std::lock_guard<std::mutex> lock(g_fileResultsMutex);
    return g_fileResults;
}

std::vector<SSearchItem> recentFiles() {
    std::vector<SSearchItem> items;
    const char* home = std::getenv("HOME");
    if (!home) return items;

    static const std::vector<std::string> CANDIDATES = {
        "Projects", "Downloads", "Documents", "Pictures", "Videos", "Desktop",
        ".config/hypr", ".config/omarchy"
    };

    for (const auto& rel : CANDIDATES) {
        std::string full = std::string(home) + "/" + rel;
        if (fs::exists(full)) {
            SSearchItem it;
            it.kind          = EItemKind::File;
            it.id            = "file:" + full;
            it.name          = rel;
            it.description   = full;
            it.filePath      = full;
            it.category      = "Files & Folders";
            it.actionCommand = full;
            categorizeFile(full, it);
            items.push_back(std::move(it));
        }
    }
    return items;
}

// ============================================================================
// Public API
// ============================================================================
void init() {
    if (!g_fileRunning) {
        g_fileRunning = true;
        g_fileThread  = std::thread(fileWorkerLoop);
    }
    reloadMenuIfNeeded();
}

void shutdown() {
    closeSushi();
    if (g_fileRunning) {
        g_fileRunning = false;
        g_fileWorkerCv.notify_all();
        if (g_fileThread.joinable())
            g_fileThread.join();
    }
}

void query(const std::string& text) {
    init();
    {
        std::lock_guard<std::mutex> lock(g_fileWorkerMutex);
        g_fileQueryText = text;
        g_fileGen++;
    }
    g_fileWorkerCv.notify_one();
}

bool activate(const SSearchItem& item) {
    if (item.kind == EItemKind::Menu) {
        navigateMenu(item.id);
        return false;
    }
    if (item.kind == EItemKind::Back) {
        navigateMenuBack();
        return false;
    }
    if (item.kind == EItemKind::Calculator) {
        CProcess proc("/usr/bin/wl-copy", {item.actionCommand});
        return proc.runAsync();
    }
    if (item.kind == EItemKind::File) {
        CProcess proc("/usr/bin/xdg-open", {item.filePath});
        return proc.runAsync();
    }
    if (item.kind == EItemKind::Agent || item.kind == EItemKind::Action) {
        CProcess proc("/bin/bash", {"-c", item.actionCommand});
        return proc.runAsync();
    }
    return false;
}

} // namespace OmarchySearch
