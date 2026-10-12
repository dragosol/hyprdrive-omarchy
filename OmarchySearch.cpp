#include "OmarchySearch.hpp"
#include "Launch.hpp"
#include "ShellQuote.hpp"
#include "ToolSearch.hpp"

#include <hyprutils/os/Process.hpp>
using namespace Hyprutils::OS;

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <mutex>
#include <poll.h>
#include <regex>
#include <spawn.h>
#include <sstream>
#include <thread>
#include <unordered_set>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <glib.h>
#include <systemd/sd-bus.h>

extern char** environ;

namespace fs = std::filesystem;

namespace OmarchySearch {

// ============================================================================
// Quick Look (GNOME Sushi) State Machine
// Uses asynchronous sd-bus calls to org.gnome.NautilusPreviewer2 (ShowFile / Close).
// Zero fork(), zero blocking waits, completely safe in multithreaded compositor.
// ============================================================================
static bool        g_sushiActive = false;
static std::string g_sushiPath;

static sd_bus* getSushiBus() {
    static sd_bus* s_bus = nullptr;
    if (!s_bus) {
        if (sd_bus_default_user(&s_bus) < 0)
            s_bus = nullptr;
    }
    return s_bus;
}

bool isSushiActive() {
    return g_sushiActive;
}

bool isOverviewSushiWindow(const std::string& windowClass, uint32_t pid) {
    if (!g_sushiActive)
        return false;
    return windowClass == "org.gnome.NautilusPreviewer";
}

void closeSushi() {
    if (g_sushiActive) {
        sd_bus* bus = getSushiBus();
        if (bus) {
            sd_bus_message* m = nullptr;
            if (sd_bus_message_new_method_call(bus, &m, "org.gnome.NautilusPreviewer", "/org/gnome/NautilusPreviewer", "org.gnome.NautilusPreviewer2", "Close") >= 0) {
                sd_bus_message_set_expect_reply(m, 0);
                if (sd_bus_send(bus, m, nullptr) >= 0)
                    sd_bus_flush(bus);
                sd_bus_message_unref(m);
            }
        }
    }
    g_sushiActive = false;
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

    GError* err = nullptr;
    char*   uri = g_filename_to_uri(filePath.c_str(), nullptr, &err);
    if (!uri) {
        if (err)
            g_error_free(err);
        return;
    }

    sd_bus* bus = getSushiBus();
    if (bus) {
        sd_bus_message* m = nullptr;
        if (sd_bus_message_new_method_call(bus, &m, "org.gnome.NautilusPreviewer", "/org/gnome/NautilusPreviewer", "org.gnome.NautilusPreviewer2", "ShowFile") >= 0) {
            if (sd_bus_message_append(m, "ssbs", uri, "", 1, "") >= 0) {
                sd_bus_message_set_expect_reply(m, 0);
                if (sd_bus_send(bus, m, nullptr) >= 0)
                    sd_bus_flush(bus);
            }
            sd_bus_message_unref(m);
        }
    }
    g_free(uri);

    g_sushiActive = true;
    g_sushiPath   = filePath;
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

static std::string agentUsageId(const SAgentDef& AG) {
    if (AG.prefix == "agy")
        return "antigravity";
    return AG.prefix;
}

static std::string agentUsageDir() {
    if (const char* state = std::getenv("XDG_STATE_HOME"); state && *state)
        return std::string(state) + "/omarchy/agents/usage";
    const char* home = std::getenv("HOME");
    if (!home)
        return {};
    return std::string(home) + "/.local/state/omarchy/agents/usage";
}

static bool jsonFieldTrue(const std::string& body, const char* key) {
    const std::string needle = std::string("\"") + key + "\"";
    auto              pos    = body.find(needle);
    if (pos == std::string::npos)
        return false;
    pos = body.find(':', pos + needle.size());
    if (pos == std::string::npos)
        return false;
    ++pos;
    while (pos < body.size() && std::isspace(static_cast<unsigned char>(body[pos])))
        ++pos;
    return body.compare(pos, 4, "true") == 0;
}

static const std::unordered_set<std::string>& readyAgentIds() {
    static std::unordered_set<std::string> ids;
    static time_t                          mtime = 0;
    const std::string                      dir   = agentUsageDir();
    if (dir.empty())
        return ids;

    struct stat st {};
    const time_t nowM = (stat(dir.c_str(), &st) == 0) ? st.st_mtime : 0;
    if (nowM == mtime && !ids.empty())
        return ids;

    ids.clear();
    mtime = nowM;
    std::error_code ec;
    for (const auto& ent : fs::directory_iterator(dir, ec)) {
        if (!ent.is_regular_file(ec))
            continue;
        const auto name = ent.path().filename().string();
        if (!name.ends_with(".json"))
            continue;
        std::ifstream f(ent.path());
        if (!f)
            continue;
        std::string body((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (!jsonFieldTrue(body, "ready"))
            continue;
        ids.insert(name.substr(0, name.size() - 5));
    }
    return ids;
}

static bool agentLoggedIn(const SAgentDef& AG) {
    return readyAgentIds().contains(agentUsageId(AG));
}

static std::string agentIconPath(const SAgentDef& AG) {
    const std::string id = agentUsageId(AG);
    const char*       home = std::getenv("HOME");
    std::vector<std::string> cands;
    if (home)
        cands.push_back(std::string(home) + "/.config/omarchy/plugins/dragos.agents/assets/" + id + ".svg");
    cands.push_back("/usr/share/omarchy/shell/plugins/omarchy.agents/assets/" + id + ".svg");
    std::error_code ec;
    for (const auto& p : cands) {
        if (fs::exists(p, ec))
            return p;
    }
    return "utilities-terminal";
}

static SSearchItem agentItem(const SAgentDef& AG, const std::string& prompt) {
    SSearchItem item;
    item.kind     = EItemKind::Agent;
    item.id       = "agent:" + AG.prefix;
    item.category = "AI Agent";
    item.icon     = agentIconPath(AG);
    if (prompt.empty()) {
        item.name          = "/" + AG.prefix + " <prompt>";
        item.description   = AG.name + " (" + AG.model + ") — " + AG.description;
        item.actionCommand = replaceAll(AG.commandTemplate, "\"%P%\"", "");
        item.actionCommand = replaceAll(item.actionCommand, "%P%", "");
    } else {
        item.name        = "Ask " + AG.name;
        item.description = prompt;
        // The prompt is user text going into a `bash -c` string (activate()):
        // one single-quoted word, so the shell expands none of it -- `$(...)`,
        // backticks and `$VAR` reach the agent as typed (marketplace review,
        // #11012). The templates wrap %P% in double quotes, which would expand;
        // the quoted word replaces the quotes too.
        const std::string QUOTED = ScrollOverview::shellQuote(prompt);
        item.actionCommand       = replaceAll(AG.commandTemplate, "\"%P%\"", QUOTED);
        item.actionCommand       = replaceAll(item.actionCommand, "%P%", QUOTED);
    }
    return item;
}

// The name Omarchy's default-agent file uses for one of our agent prefixes, or
// "" for an agent that cannot be Omarchy's default (the "Default Agent"
// pseudo-row among them). `agy` maps to `gemini`, exactly as this machine's
// omarchy-default-agent overlay does for Antigravity: ~/.local/bin/gemini
// forwards to agy, and the packaged launcher only knows the gemini name -- so
// the Antigravity and Gemini rows ARE one default here.
static std::string omarchyAgentName(const std::string& prefix) {
    if (prefix == "agy" || prefix == "gemini")
        return "gemini";

    for (const char* NAME : {"grok", "claude", "opencode", "codex", "copilot", "crush", "pi"}) {
        if (prefix == NAME)
            return NAME;
    }

    return "";
}

static std::string agentPrefix(const SSearchItem& item) {
    constexpr std::string_view TAG = "agent:";
    return item.kind == EItemKind::Agent && item.id.starts_with(TAG) ? item.id.substr(TAG.size()) : std::string{};
}

static fs::path defaultAgentPath() {
    const char* HOME = std::getenv("HOME");
    return HOME && *HOME ? fs::path{HOME} / ".config" / "omarchy" / "defaults" / "agent" : fs::path{};
}

// Re-read only when the file CHANGES: this is asked for every agent row on every
// frame the results are up, and `omarchy default agent codex` run in a terminal
// must still land without a restart. Same parse as the packaged getter: the
// first line, surrounding whitespace dropped.
static std::string defaultAgentName() {
    static std::string        cached;
    static fs::file_time_type seen{};
    static bool               loaded = false;

    const auto PATH = defaultAgentPath();
    std::error_code ec;
    const auto MTIME = PATH.empty() ? fs::file_time_type{} : fs::last_write_time(PATH, ec);
    if (PATH.empty() || ec) {
        cached.clear();
        loaded = false;
        return cached;
    }

    if (!loaded || MTIME != seen) {
        std::ifstream file(PATH);
        std::string   line;
        std::getline(file, line);
        const auto FIRST = line.find_first_not_of(" \t\r\n");
        const auto LAST  = line.find_last_not_of(" \t\r\n");
        cached = FIRST == std::string::npos ? std::string{} : line.substr(FIRST, LAST - FIRST + 1);
        seen   = MTIME;
        loaded = true;
    }

    return cached;
}

bool canBeDefaultAgent(const SSearchItem& item) {
    return !omarchyAgentName(agentPrefix(item)).empty();
}

bool isDefaultAgent(const SSearchItem& item) {
    const auto NAME = omarchyAgentName(agentPrefix(item));
    return !NAME.empty() && NAME == defaultAgentName();
}

bool setDefaultAgent(const SSearchItem& item) {
    const auto NAME = omarchyAgentName(agentPrefix(item));
    const auto PATH = defaultAgentPath();
    if (NAME.empty() || PATH.empty())
        return false;

    // Written straight to the file, NOT through `omarchy default agent`: that
    // command also installs the agent and LAUNCHES it, and a double tap in a
    // search field must not open a terminal. Temp file + rename, so a reader
    // never sees it half-written.
    std::error_code ec;
    fs::create_directories(PATH.parent_path(), ec);

    auto tmp = PATH;
    tmp += ".hyprdrive-tmp";
    {
        std::ofstream file(tmp, std::ios::trunc);
        if (!(file << NAME << '\n'))
            return false;
    }

    fs::rename(tmp, PATH, ec);
    return !ec;
}

// The default agent's rows first, every other row in the order it already had.
static void defaultAgentFirst(std::vector<SSearchItem>& items) {
    std::stable_partition(items.begin(), items.end(), [](const SSearchItem& item) { return isDefaultAgent(item); });
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
    for (char c : token)
        lowerToken += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    for (const auto& AG : agentDefs()) {
        if (!lowerToken.empty() && AG.prefix.find(lowerToken) != 0 && lowerToken != AG.prefix)
            continue;
        const bool exact = lowerToken == AG.prefix;
        if (!exact && !agentLoggedIn(AG))
            continue;
        items.push_back(agentItem(AG, prompt));
    }
    defaultAgentFirst(items);
    return items;
}

std::vector<SSearchItem> promptAllAgents(const std::string& prompt) {
    std::vector<SSearchItem> items;
    if (prompt.empty())
        return items;
    for (const auto& AG : agentDefs()) {
        if (!agentLoggedIn(AG))
            continue;
        items.push_back(agentItem(AG, prompt));
    }
    defaultAgentFirst(items);
    return items;
}

std::vector<SSearchItem> agentHelpItems() {
    std::vector<SSearchItem> items;
    for (const auto& AG : agentDefs()) {
        if (!agentLoggedIn(AG))
            continue;
        items.push_back(agentItem(AG, ""));
    }
    defaultAgentFirst(items);
    return items;
}

bool isGlyphIcon(const std::string& icon) {
    if (icon.empty() || icon.front() == '/')
        return false;
    for (unsigned char c : icon) {
        if (c >= 0x80)
            return true;
    }
    return false;
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

static void applyMenuVisuals(SSearchItem& item, const SRawMenuItem& raw) {
    item.icon     = raw.icon;
    item.iconFont = raw.iconFont;
    if (item.icon.empty()) {
        std::string cur   = raw.parent;
        int         guard = 0;
        while (item.icon.empty() && !cur.empty() && cur != "root" && guard++ < 16) {
            auto it = g_menuItems.find(cur);
            if (it == g_menuItems.end())
                break;
            item.icon = it->second.icon;
            if (item.iconFont.empty())
                item.iconFont = it->second.iconFont;
            cur = it->second.parent;
        }
    }
    if (item.icon.empty())
        item.icon = (raw.kind == "action") ? "system-run" : "folder";
}

static std::string parentPathFor(const std::unordered_map<std::string, SRawMenuItem>& items, const std::string& id) {
    auto it = items.find(id);
    if (it == items.end() || it->second.parent.empty() || it->second.parent == "root")
        return "";
    return pathFor(items, it->second.parent);
}

static std::string trimCopy(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front())))
        s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back())))
        s.pop_back();
    return s;
}

static std::string expandPath(std::string p) {
    p = trimCopy(std::move(p));
    if (p.size() >= 2 && ((p.front() == '"' && p.back() == '"') || (p.front() == '\'' && p.back() == '\'')))
        p = p.substr(1, p.size() - 2);
    const char* home = std::getenv("HOME");
    if (home && !p.empty()) {
        if (p.starts_with("$HOME"))
            p.replace(0, 5, home);
        else if (p.front() == '~')
            p.replace(0, 1, home);
    }
    return p;
}

static const std::unordered_set<std::string>& pacmanLocalNames() {
    static const std::unordered_set<std::string> NAMES = [] {
        std::unordered_set<std::string> out;
        std::error_code ec;
        for (const auto& ent : fs::directory_iterator("/var/lib/pacman/local", ec)) {
            if (!ent.is_directory(ec))
                continue;
            const std::string dir = ent.path().filename().string();
            const auto r1 = dir.rfind('-');
            if (r1 == std::string::npos || r1 == 0)
                continue;
            const auto r2 = dir.rfind('-', r1 - 1);
            if (r2 == std::string::npos)
                continue;
            out.insert(dir.substr(0, r2));
        }
        return out;
    }();
    return NAMES;
}

static bool commandOnPath(const std::string& cmd) {
    static std::unordered_map<std::string, bool> cache;
    if (auto it = cache.find(cmd); it != cache.end())
        return it->second;
    bool ok = false;
    if (cmd.find('/') != std::string::npos) {
        ok = access(cmd.c_str(), X_OK) == 0;
    } else if (const char* path = std::getenv("PATH")) {
        std::string paths = path;
        size_t pos = 0;
        while (pos <= paths.size()) {
            const auto sep = paths.find(':', pos);
            const auto dir = paths.substr(pos, sep == std::string::npos ? std::string::npos : sep - pos);
            if (!dir.empty()) {
                const std::string full = dir + "/" + cmd;
                if (access(full.c_str(), X_OK) == 0) {
                    ok = true;
                    break;
                }
            }
            if (sep == std::string::npos)
                break;
            pos = sep + 1;
        }
    }
    cache[cmd] = ok;
    return ok;
}

static bool pathExists(const std::string& p) {
    std::error_code ec;
    return fs::exists(p, ec);
}

static bool pathIsFile(const std::string& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

static bool pathIsDir(const std::string& p) {
    std::error_code ec;
    return fs::is_directory(p, ec);
}

static bool globExists(const std::string& pattern) {
    const std::string pat = expandPath(pattern);
    const auto star = pat.find('*');
    if (star == std::string::npos)
        return pathExists(pat);
    const auto slash = pat.rfind('/', star);
    const fs::path dir = slash == std::string::npos ? fs::path(".") : fs::path(pat.substr(0, slash));
    const std::string prefix = pat.substr(slash == std::string::npos ? 0 : slash + 1, star - (slash == std::string::npos ? 0 : slash + 1));
    const std::string rest = pat.substr(star + 1);
    std::error_code ec;
    if (!fs::is_directory(dir, ec))
        return false;
    for (const auto& ent : fs::directory_iterator(dir, ec)) {
        const auto name = ent.path().filename().string();
        if (!prefix.empty() && !name.starts_with(prefix))
            continue;
        if (rest.empty())
            return true;
        if (rest.front() == '/') {
            if (pathExists((ent.path() / rest.substr(1)).string()))
                return true;
        } else if (name.ends_with(rest)) {
            return true;
        }
    }
    return false;
}

static bool evalTestBracket(std::string inner) {
    inner = trimCopy(std::move(inner));
    bool neg = false;
    if (inner.starts_with("!")) {
        neg = true;
        inner = trimCopy(inner.substr(1));
    }
    bool ok = false;
    if (inner.starts_with("-f "))
        ok = pathIsFile(expandPath(inner.substr(3)));
    else if (inner.starts_with("-d "))
        ok = pathIsDir(expandPath(inner.substr(3)));
    else if (inner.starts_with("-x ")) {
        const auto p = expandPath(inner.substr(3));
        ok = access(p.c_str(), X_OK) == 0;
    } else if (inner.starts_with("-e "))
        ok = pathExists(expandPath(inner.substr(3)));
    else if (inner.find("findmnt") != std::string::npos && inner.find("btrfs") != std::string::npos) {
        std::ifstream mounts("/proc/mounts");
        std::string line;
        while (std::getline(mounts, line)) {
            std::istringstream iss(line);
            std::string dev, mp, type;
            if (iss >> dev >> mp >> type && mp == "/" && type == "btrfs") {
                ok = true;
                break;
            }
        }
    } else if (inner.find("omarchy-network-status") != std::string::npos && inner.find("wifi") != std::string::npos) {
        std::error_code ec;
        for (const auto& ent : fs::directory_iterator("/sys/class/net", ec)) {
            if (fs::exists(ent.path() / "wireless", ec)) {
                ok = true;
                break;
            }
        }
    } else {
        return true; // unknown [[ ]] — fail open, never fork
    }
    return neg ? !ok : ok;
}

static bool evalWhenAtom(std::string expr) {
    expr = trimCopy(std::move(expr));
    if (expr.empty())
        return true;

    if (expr.starts_with("[[") && expr.ends_with("]]"))
        return evalTestBracket(expr.substr(2, expr.size() - 4));

    if (expr.starts_with("omarchy-pkg-present ")) {
        std::istringstream iss(expr.substr(20));
        std::string pkg;
        const auto& pkgs = pacmanLocalNames();
        while (iss >> pkg) {
            if (!pkgs.contains(pkg))
                return false;
        }
        return true;
    }
    if (expr.starts_with("omarchy-cmd-present ")) {
        std::istringstream iss(expr.substr(20));
        std::string cmd;
        while (iss >> cmd) {
            if (!commandOnPath(cmd))
                return false;
        }
        return true;
    }
    if (expr.starts_with("omarchy-toggle-enabled "))
        return pathIsFile(expandPath("$HOME/.local/state/omarchy/toggles/" + trimCopy(expr.substr(23))));
    if (expr == "omarchy-hw-laptop") {
        if (pathExists("/proc/acpi/button/lid"))
            return true;
        std::ifstream chassis("/sys/class/dmi/id/chassis_type");
        int type = 0;
        if (chassis >> type)
            return type == 8 || type == 9 || type == 10 || type == 14 || type == 30 || type == 31 || type == 32;
        return false;
    }
    if (expr == "omarchy-hw-webcam") {
        std::error_code ec;
        for (const auto& ent : fs::directory_iterator("/dev", ec)) {
            const auto name = ent.path().filename().string();
            if (name.starts_with("video") && name.size() > 5 && std::isdigit(static_cast<unsigned char>(name[5])))
                return true;
        }
        return false;
    }
    if (expr == "omarchy-hibernation-available")
        return pathExists("/sys/power/image_size") && pathExists("/etc/mkinitcpio.conf.d/omarchy_resume.conf");
    if (expr == "omarchy-theme-extras") {
        std::error_code ec;
        for (const auto& ent : fs::directory_iterator(expandPath("$HOME/.config/omarchy/themes"), ec)) {
            if (!ent.is_directory(ec) || ent.is_symlink(ec))
                continue;
            if (fs::is_directory(ent.path() / ".git", ec))
                return true;
        }
        return false;
    }
    if (expr.starts_with("compgen -G "))
        return globExists(expr.substr(11));
    if (expr.starts_with("pgrep ")) {
        const auto needle = expr.substr(expr.find('\'') == std::string::npos ? expr.rfind(' ') + 1 : expr.find('\'') + 1);
        std::string want = needle;
        if (!want.empty() && want.back() == '\'')
            want.pop_back();
        if (want.starts_with("^"))
            want.erase(want.begin());
        std::error_code ec;
        for (const auto& ent : fs::directory_iterator("/proc", ec)) {
            const auto id = ent.path().filename().string();
            if (id.empty() || !std::isdigit(static_cast<unsigned char>(id.front())))
                continue;
            std::ifstream cmd(ent.path() / "cmdline");
            std::string buf((std::istreambuf_iterator<char>(cmd)), {});
            if (buf.find(want) != std::string::npos)
                return true;
        }
        return false;
    }
    if (expr.starts_with("systemctl is-enabled"))
        return pathExists("/etc/systemd/system/multi-user.target.wants/sshd.service") ||
            pathExists("/etc/systemd/system/sshd.service");
    if (expr.starts_with("flatpak info ")) {
        const auto app = trimCopy(expr.substr(13));
        return pathExists("/var/lib/flatpak/app/" + app) || pathExists(expandPath("$HOME/.local/share/flatpak/app/" + app));
    }

    // Unknown predicate: show the row. Never fork — system()/pacman from the
    // compositor thread is what froze the overview for seconds on first type.
    return true;
}

static bool evalWhenExpr(std::string expr) {
    expr = trimCopy(std::move(expr));
    if (expr.empty())
        return true;
    const auto andPos = expr.find("&&");
    if (andPos != std::string::npos)
        return evalWhenExpr(expr.substr(0, andPos)) && evalWhenExpr(expr.substr(andPos + 2));
    if (expr.starts_with("!"))
        return !evalWhenExpr(trimCopy(expr.substr(1)));
    return evalWhenAtom(expr);
}

static std::unordered_map<std::string, bool> g_whenCache;

static bool evaluateWhen(const std::string& when) {
    if (when.empty())
        return true;
    if (const auto it = g_whenCache.find(when); it != g_whenCache.end())
        return it->second;
    const bool ok = evalWhenExpr(when);
    g_whenCache[when] = ok;
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
    g_whenCache.clear();
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
            applyMenuVisuals(item, raw);

            if (raw.kind == "menu") {
                item.kind        = EItemKind::Menu;
                item.description = raw.description.empty() ? "Submenu ›" : raw.description + " ›";
            } else if (raw.kind == "link") {
                item.kind        = EItemKind::Menu;
                item.id          = raw.target.empty() ? raw.id : raw.target;
                item.description = "Submenu ›";
            } else {
                item.kind          = EItemKind::Action;
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
        item.id       = raw->id;
        item.name     = raw->label;
        item.category = "Omarchy Menu";
        applyMenuVisuals(item, *raw);

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
static std::atomic<uint64_t>   g_fileDoneGen{0};
static std::mutex              g_fileWorkerMutex;
static std::condition_variable g_fileWorkerCv;
static std::string             g_fileQueryText;

static std::mutex              g_fileResultsMutex;
static std::vector<SSearchItem> g_fileResults;

static std::function<void()> g_resultsCb;

void setResultsCallback(std::function<void()> cb) {
    g_resultsCb = std::move(cb);
}

static void notifyFileResultsReady() {
    if (g_resultsCb)
        g_resultsCb();
}

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

bool fileSearchBusy() {
    return g_fileGen.load(std::memory_order_acquire) != g_fileDoneGen.load(std::memory_order_acquire);
}

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

static void killPid(pid_t pid) {
    if (pid <= 0)
        return;
    kill(pid, SIGKILL);
    int status = 0;
    waitpid(pid, &status, 0);
}

// posix_spawn rather than popen: a shell string cannot be killed promptly, and
// `--follow` walked symlink cycles / network mounts behind every keystroke.
static pid_t spawnFd(int outFd, const char* home, const std::vector<std::string>& terms) {
    std::vector<std::string> args;
    args.reserve(16 + (sizeof(OMARCHY_FIND_EXCLUDES) / sizeof(OMARCHY_FIND_EXCLUDES[0])) * 2 + terms.size());
    args.emplace_back("fd");
    args.emplace_back("--color=never");
    args.emplace_back("--one-file-system");
    args.emplace_back("-i");
    args.emplace_back("--no-ignore");
    args.emplace_back("--max-results");
    args.emplace_back("50");
    args.emplace_back("--full-path");
    for (const auto* exc : OMARCHY_FIND_EXCLUDES) {
        args.emplace_back("-E");
        args.emplace_back(exc);
    }
    for (size_t t = 1; t < terms.size(); ++t) {
        args.emplace_back("--and");
        args.emplace_back(terms[t]);
    }
    args.emplace_back("--");
    args.emplace_back(terms[0]);
    args.emplace_back(home);

    std::vector<char*> argv;
    argv.reserve(args.size() + 1);
    for (auto& a : args)
        argv.push_back(a.data());
    argv.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, outFd, STDOUT_FILENO);
    const int DEVNULL = open("/dev/null", O_WRONLY);
    if (DEVNULL >= 0) {
        posix_spawn_file_actions_adddup2(&actions, DEVNULL, STDERR_FILENO);
        posix_spawn_file_actions_addclose(&actions, DEVNULL);
    }
    posix_spawn_file_actions_addclose(&actions, outFd);

    pid_t pid = -1;
    const int RC = posix_spawnp(&pid, "fd", &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (DEVNULL >= 0)
        close(DEVNULL);
    return RC == 0 ? pid : -1;
}

static void fileWorkerLoop() {
    uint64_t gen = 0;
    while (g_fileRunning) {
        std::string queryStr;
        {
            std::unique_lock<std::mutex> lock(g_fileWorkerMutex);
            g_fileWorkerCv.wait(lock, [&]() {
                return !g_fileRunning || g_fileGen > gen;
            });
            if (!g_fileRunning)
                break;
            queryStr = g_fileQueryText;
            gen      = g_fileGen.load();
        }

        if (queryStr.size() < 2 || queryStr[0] == '/') {
            {
                std::lock_guard<std::mutex> lock(g_fileResultsMutex);
                g_fileResults.clear();
            }
            g_fileDoneGen.store(gen, std::memory_order_release);
            notifyFileResultsReady();
            continue;
        }

        const char* home = std::getenv("HOME");
        if (!home) {
            {
                std::lock_guard<std::mutex> lock(g_fileResultsMutex);
                g_fileResults.clear();
            }
            g_fileDoneGen.store(gen, std::memory_order_release);
            notifyFileResultsReady();
            continue;
        }

        std::vector<std::string> terms;
        std::istringstream iss(queryStr);
        std::string token;
        while (iss >> token) {
            std::string lower;
            for (char c : token)
                lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            terms.push_back(lower);
        }

        if (terms.empty()) {
            {
                std::lock_guard<std::mutex> lock(g_fileResultsMutex);
                g_fileResults.clear();
            }
            g_fileDoneGen.store(gen, std::memory_order_release);
            notifyFileResultsReady();
            continue;
        }

        int pipefd[2];
        if (pipe(pipefd) != 0) {
            g_fileDoneGen.store(gen, std::memory_order_release);
            notifyFileResultsReady();
            continue;
        }

        const pid_t FD_PID = spawnFd(pipefd[1], home, terms);
        close(pipefd[1]);
        if (FD_PID < 0) {
            close(pipefd[0]);
            g_fileDoneGen.store(gen, std::memory_order_release);
            notifyFileResultsReady();
            continue;
        }

        struct ScoredFile {
            int         score = 0;
            SSearchItem item;
        };
        std::vector<ScoredFile> scoredFiles;
        std::string             lineBuf;
        char                    chunk[1024];
        bool                    abandoned = false;

        pollfd pfd{pipefd[0], POLLIN, 0};
        while (!abandoned) {
            if (gen != g_fileGen.load(std::memory_order_relaxed) || !g_fileRunning) {
                abandoned = true;
                break;
            }
            int pr = poll(&pfd, 1, 20); // 20ms poll
            if (pr < 0) {
                if (errno == EINTR)
                    continue;
                break;
            }
            if (pr == 0) {
                // Poll timeout, loop around to re-check gen / g_fileRunning
                continue;
            }
            ssize_t n = read(pipefd[0], chunk, sizeof(chunk) - 1);
            if (n <= 0)
                break;
            chunk[n] = '\0';
            lineBuf.append(chunk, static_cast<size_t>(n));

            size_t nl;
            while ((nl = lineBuf.find('\n')) != std::string::npos) {
                std::string line = lineBuf.substr(0, nl);
                lineBuf.erase(0, nl + 1);
                while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                    line.pop_back();
                if (line.empty())
                    continue;

                std::string path = line;
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
                item.canQuickLook  = true;
                scoredFiles.push_back({score, std::move(item)});
                if (scoredFiles.size() >= 50)
                    break;
            }
            if (scoredFiles.size() >= 50)
                break;
        }

        killPid(FD_PID);
        close(pipefd[0]);

        if (abandoned || gen != g_fileGen.load(std::memory_order_relaxed)) {
            continue;
        }

        std::sort(scoredFiles.begin(), scoredFiles.end(), [](const auto& a, const auto& b) {
            return a.score < b.score;
        });

        std::vector<SSearchItem> results;
        // The DISPLAY ceiling, not a taste one: overviewTrimSectionsToFit is
        // what decides how many of these are actually drawn, and it sizes that
        // against the room the panel has. Keeping 8 here made that decision
        // first and invisibly -- a `jpg` query on a disk full of images still
        // answered with eight rows in a panel with space for fourteen (user,
        // 2026-09-23). Moves with OVERVIEW_RESULT_ROWS_ALONE.
        const size_t KEEP = std::min<size_t>(20, scoredFiles.size());
        results.reserve(KEEP);
        for (size_t i = 0; i < KEEP; ++i) {
            categorizeFile(scoredFiles[i].item.filePath, scoredFiles[i].item);
            results.push_back(std::move(scoredFiles[i].item));
        }

        {
            std::lock_guard<std::mutex> lock(g_fileResultsMutex);
            g_fileResults = std::move(results);
        }
        g_fileDoneGen.store(gen, std::memory_order_release);
        notifyFileResultsReady();
    }
}

// See the header. The stat()s are the whole reason this is polled rather than
// computed: they are the same three the readers below already make, and making
// them per frame per monitor is exactly what the memo exists to stop.
uint64_t searchInputsGeneration() {
    using clock = std::chrono::steady_clock;
    static clock::time_point lastPoll{};
    static uint64_t          fsFingerprint = 0;
    static bool              polled        = false;

    const auto NOW = clock::now();
    if (!polled || NOW - lastPoll >= std::chrono::milliseconds(250)) {
        polled   = true;
        lastPoll = NOW;

        uint64_t h = 1469598103934665603ULL;
        const auto mix = [&h](uint64_t v) {
            h ^= v;
            h *= 1099511628211ULL;
        };

        const char* HOME = std::getenv("HOME");

        struct stat st {};
        mix(stat("/usr/share/omarchy/default/omarchy/omarchy-menu.jsonc", &st) == 0 ? static_cast<uint64_t>(st.st_mtime) : 0);
        if (HOME) {
            const std::string USERMENU = std::string(HOME) + "/.config/omarchy/extensions/omarchy-menu.jsonc";
            mix(stat(USERMENU.c_str(), &st) == 0 ? static_cast<uint64_t>(st.st_mtime) : 0);

            // Which agents are logged in: one directory mtime, the same one
            // readyAgentIds() keys its own cache on.
            const std::string USAGE = agentUsageDir();
            mix(!USAGE.empty() && stat(USAGE.c_str(), &st) == 0 ? static_cast<uint64_t>(st.st_mtime) : 0);

            // ...and which of them is Omarchy's default, because that decides
            // the ORDER of the AI & Tools column.
            const auto AGENT = defaultAgentPath();
            mix(!AGENT.empty() && stat(AGENT.c_str(), &st) == 0 ? static_cast<uint64_t>(st.st_mtime) : 0);
        }

        fsFingerprint = h;
    }

    // The worker's own counter needs no poll and must not wait for one.
    // ...and the dictionary/translator's, for the same reason: an answer lands
    // between frames and the memoised rows have to notice on the next one.
    return fsFingerprint ^ (g_fileDoneGen.load(std::memory_order_acquire) * 1099511628211ULL) ^ (ToolSearch::generation() * 0x9E3779B97F4A7C15ULL);
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
        // The opened app is a child of this: its own scope (Launch.hpp).
        std::string              binary = "/usr/bin/xdg-open";
        std::vector<std::string> args   = {item.filePath};
        ScrollOverview::Launch::wrap(binary, args);
        CProcess proc(binary, args);
        return proc.runAsync();
    }
    if (item.kind == EItemKind::Agent || item.kind == EItemKind::Action) {
        // An action can start an app (an agent opens a terminal): its own scope.
        std::string              binary = "/bin/bash";
        std::vector<std::string> args   = {"-c", item.actionCommand};
        ScrollOverview::Launch::wrap(binary, args);
        CProcess proc(binary, args);
        return proc.runAsync();
    }
    return false;
}

} // namespace OmarchySearch
