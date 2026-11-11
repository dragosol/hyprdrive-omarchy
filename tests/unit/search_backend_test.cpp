#include "SearchBackend.hpp"
#include "SearchIntent.hpp"
#include <cassert>
#include <chrono>
#include <iostream>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

static int s_testsPassed = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        std::cerr << "Assertion failed at line " << __LINE__ << ": " << #cond << std::endl; \
        std::exit(1); \
    } \
    s_testsPassed++; \
} while (0)

int main() {
    // 1. Calculator tests
    {
        auto calc1 = SearchBackend::evaluateCalculator("2 + 2");
        CHECK(calc1.has_value());
        CHECK(calc1->name == "= 4");
        CHECK(calc1->actionCommand == "4");
        CHECK(calc1->category == "Calculator");

        auto calc2 = SearchBackend::evaluateCalculator("10 * 5 - 3");
        CHECK(calc2.has_value());
        CHECK(calc2->name == "= 47");
        CHECK(calc2->actionCommand == "47");

        auto calc3 = SearchBackend::evaluateCalculator("(10 + 20) / 3");
        CHECK(calc3.has_value());
        CHECK(calc3->name == "= 10");
        CHECK(calc3->actionCommand == "10");

        auto calc4 = SearchBackend::evaluateCalculator("2^3");
        CHECK(calc4.has_value());
        CHECK(calc4->name == "= 8");
        CHECK(calc4->actionCommand == "8");

        auto calc5 = SearchBackend::evaluateCalculator("sqrt(16)");
        CHECK(calc5.has_value());
        CHECK(calc5->name == "= 4");
        CHECK(calc5->actionCommand == "4");

        auto calc6 = SearchBackend::evaluateCalculator("sin(0)");
        CHECK(calc6.has_value());
        CHECK(calc6->name == "= 0");
        CHECK(calc6->actionCommand == "0");

        // Non-math queries should return nullopt
        auto calcNone1 = SearchBackend::evaluateCalculator("hello world");
        CHECK(!calcNone1.has_value());

        auto calcNone2 = SearchBackend::evaluateCalculator("firefox");
        CHECK(!calcNone2.has_value());

        // Bare number without operator should return nullopt
        auto calcNone3 = SearchBackend::evaluateCalculator("42");
        CHECK(!calcNone3.has_value());

        // Division by zero should not crash
        auto calcDivZero = SearchBackend::evaluateCalculator("10 / 0");
        if (calcDivZero.has_value()) {
            CHECK(calcDivZero->name.find("Error") != std::string::npos || calcDivZero->name.find("inf") != std::string::npos);
        }
    }

    // 2. AI Agent queries
    {
        CHECK(SearchBackend::isAgentQuery("/grok what is the weather today"));
        CHECK(SearchBackend::isAgentQuery("/agy fix the build"));
        CHECK(SearchBackend::isAgentQuery("/gemini help me"));
        CHECK(SearchBackend::isAgentQuery("/claude review"));
        CHECK(SearchBackend::isAgentQuery("/opencode"));
        CHECK(SearchBackend::isAgentQuery("/codex"));
        CHECK(SearchBackend::isAgentQuery("/copilot"));
        CHECK(!SearchBackend::isAgentQuery("grok what is the weather"));
        CHECK(!SearchBackend::isAgentQuery("firefox"));

        auto grokResults = SearchBackend::searchAgents("/grok what is the weather today");
        CHECK(!grokResults.empty());
        CHECK(grokResults.front().name.find("Grok") != std::string::npos);
        CHECK(grokResults.front().category == "AI Agent");
        CHECK(grokResults.front().agentPrompt == "what is the weather today" && grokResults.front().actionCommand.find("weather") == std::string::npos);

        auto agyResults = SearchBackend::searchAgents("/agy solve this problem");
        CHECK(!agyResults.empty());
        CHECK(agyResults.front().name.find("AGY") != std::string::npos || agyResults.front().name.find("Antigravity") != std::string::npos);
        CHECK(agyResults.front().category == "AI Agent");
        CHECK(agyResults.front().agentPrompt == "solve this problem" && agyResults.front().actionCommand.find("solve") == std::string::npos);

        auto all = SearchBackend::promptAllAgents("what is the weather today");
        // Sentence queries only list agents that are installed and logged in
        // (usage record with ready:true), not the full catalog.
        CHECK(all.size() < 8);
        for (const auto& it : all) {
            CHECK(it.description == "what is the weather today");
            CHECK(it.agentPrompt == "what is the weather today" && it.actionCommand.find("weather") == std::string::npos);
            CHECK(it.kind == SearchBackend::EItemKind::Agent);
        }

        CHECK(SearchBackend::isGlyphIcon("\uF249"));
        CHECK(!SearchBackend::isGlyphIcon("folder"));
        CHECK(!SearchBackend::isGlyphIcon("/usr/share/pixmaps/x.svg"));
    }

    // 3. Omarchy Menu Actions Search — must not fork. The old path ran
    // system("bash -c when") for every menu row on the first query and froze
    // the compositor for seconds.
    {
        const auto t0 = std::chrono::steady_clock::now();
        auto menuResults = SearchBackend::searchMenu("lock", 5);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        CHECK(ms < 250);
        (void)menuResults;

        const auto t1 = std::chrono::steady_clock::now();
        auto again = SearchBackend::searchMenu("firefox", 8);
        const auto ms2 = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t1).count();
        CHECK(ms2 < 100);
        (void)again;

        auto installHits = SearchBackend::searchMenu("install", 8);
        std::string installId;
        for (const auto& it : installHits) {
            if (it.name == "Install" && it.kind == SearchBackend::EItemKind::Menu) {
                installId = it.id;
                CHECK(!it.icon.empty());
                CHECK(SearchBackend::isGlyphIcon(it.icon));
                break;
            }
        }
        CHECK(!installId.empty());
        SearchBackend::navigateMenu(installId);
        auto kids = SearchBackend::searchMenu("", 24);
        bool hasBack = false, hasPackage = false;
        for (const auto& it : kids) {
            if (it.kind == SearchBackend::EItemKind::Back)
                hasBack = true;
            if (it.name == "Package") {
                hasPackage = true;
                CHECK(!it.icon.empty());
            }
        }
        CHECK(hasBack);
        CHECK(hasPackage);
        SearchBackend::resetMenuRoute();
    }

    // 4. File search is idle until a query is issued
    {
        CHECK(!SearchBackend::fileSearchBusy());
    }

    // 5. Space handling: typing intent must never trigger Quick Look
    {
        // While typing, space is always a space character
        CHECK(!overviewSpacePreviews(ESearchIntent::Typing, true, true, true));
        CHECK(!overviewSpacePreviews(ESearchIntent::Typing, false, false, false));
        // While browsing, auto-selected file must not swallow space
        CHECK(!overviewSpacePreviews(ESearchIntent::Browsing, true, true, false));
        // Explicitly selected file can trigger preview with space
        CHECK(overviewSpacePreviews(ESearchIntent::Browsing, false, true, true));
        CHECK(!overviewSpacePreviews(ESearchIntent::Browsing, false, false, true));
        // Hovering a previewable file triggers preview
        CHECK(overviewSpacePreviews(ESearchIntent::Hovering, true, false, false));
        CHECK(!overviewSpacePreviews(ESearchIntent::Hovering, false, true, true));
    }

    // An agent's prompt is TYPED into it by agent-run, never passed as an
    // argument (marketplace review, #11392: /proc/<pid>/cmdline is readable by
    // every local account). No row's command holds any of the prompt; the
    // prompt goes to a 0600 file in a 0700 directory; every agent's real
    // command, run through bash with stand-in launchers and agents and the real
    // agent-run, gets it typed byte for byte and the file is gone after.
    {
        namespace fs = std::filesystem;
        char tmpl[] = "/tmp/hd-agent-test-XXXXXX";
        const fs::path DIR = mkdtemp(tmpl);
        const fs::path USAGE = DIR / "omarchy/agents/usage";
        fs::create_directories(USAGE);
        for (const char* ID : {"grok", "antigravity", "gemini", "claude", "opencode", "codex", "copilot", "crush", "pi", "agent"})
            std::ofstream(USAGE / (std::string(ID) + ".json")) << "{\"ready\": true}";
        setenv("XDG_STATE_HOME", DIR.c_str(), 1);
        const fs::path RUN = DIR / "run";
        fs::create_directories(RUN);
        fs::permissions(RUN, fs::perms::owner_all, fs::perm_options::replace);
        setenv("XDG_RUNTIME_DIR", RUN.c_str(), 1);

        // agent-run beside this binary, where agentRunner() looks.
        const fs::path RUNNER = SearchBackend::agentRunner();
        CHECK(!RUNNER.empty());
        fs::copy_file("ui/bin/agent-run", RUNNER, fs::copy_options::overwrite_existing);
        fs::permissions(RUNNER, fs::perms::owner_all);

        const fs::path BIN = DIR / "bin";
        fs::create_directories(BIN);
        // The launcher drops its --app-id and runs the rest here; every agent
        // asks for bracketed paste and records what it is typed.
        std::ofstream(BIN / "omarchy-launch-tui") << "#!/bin/sh\nshift\nexec \"$@\"\n";
        const fs::path FAKE = DIR / "fake-agent.py";
        std::ofstream(FAKE) << "import os, sys\n"
                               "sys.stdout.write('\\x1b[?2004hagent> '); sys.stdout.flush()\n"
                               "got = b''\n"
                               "while not (b'\\x1b[201~' in got and got.endswith(b'\\n')):\n"
                               "    c = os.read(0, 4096)\n"
                               "    if not c: break\n"
                               "    got += c\n"
                               "open(os.environ['HD_GOT'], 'wb').write(got)\n";
        for (const char* A : {"grok", "agy", "gemini", "claude", "opencode", "codex", "copilot", "crush", "pi", "omarchy-agent"})
            std::ofstream(BIN / A) << "#!/bin/sh\nexec python3 '" << FAKE.string() << "'\n";
        for (const auto& E : fs::directory_iterator(BIN))
            fs::permissions(E.path(), fs::perms::owner_all);

        const fs::path    CANARY = DIR / "pwned";
        const std::string TOUCH  = "$(touch " + CANARY.string() + ")";
        const std::string PROMPT = "SECRET-7f3a fix " + TOUCH + " and `touch " + CANARY.string() + "` for $HOME's \"quoted\" \\ %P% it's\nsecond line";
        const auto        ITEMS  = SearchBackend::promptAllAgents(PROMPT);
        CHECK(ITEMS.size() == 10);
        for (const auto& IT : ITEMS) {
            CHECK(IT.actionCommand.find("SECRET") == std::string::npos);
            CHECK(IT.actionCommand.find("touch") == std::string::npos);
            CHECK(IT.agentPrompt == PROMPT);
            CHECK(IT.actionCommand.find("--prompt-file @HYPRDRIVE_PROMPT_FILE@ -- ") != std::string::npos);

            const std::string PATH = SearchBackend::writePromptFile(IT.agentPrompt);
            CHECK(!PATH.empty());
            struct stat st {};
            CHECK(stat(PATH.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
            CHECK(stat((RUN / "hyprdrive").c_str(), &st) == 0 && (st.st_mode & 0777) == 0700);
            std::stringstream body;
            body << std::ifstream(PATH).rdbuf();
            CHECK(body.str() == PROMPT);

            const std::string CMD = SearchBackend::withPromptFile(IT.actionCommand, PATH);
            CHECK(CMD.find("@HYPRDRIVE_PROMPT_FILE@") == std::string::npos && CMD.find("SECRET") == std::string::npos);
            const fs::path GOT = DIR / "got";
            fs::remove(GOT);
            std::ofstream(DIR / "cmd") << CMD;
            // as activate() runs it: bash -c, the command one argv word
            const int RC = std::system(("env PATH='" + BIN.string() + ":/usr/bin:/bin' HD_GOT='" + GOT.string() + "' /bin/bash -c \"$(cat '" +
                                        (DIR / "cmd").string() + "')\" </dev/null >/dev/null 2>&1")
                                           .c_str());
            CHECK(RC == 0);
            std::stringstream got;
            got << std::ifstream(GOT).rdbuf();
            if (got.str() != "\x1b[200~" + PROMPT + "\x1b[201~\n")
                std::cerr << IT.name << ": got [" << got.str() << "]" << std::endl;
            CHECK(got.str() == "\x1b[200~" + PROMPT + "\x1b[201~\n");
            CHECK(!fs::exists(PATH));
            CHECK(!fs::exists(CANARY));
        }
        // A path the command could not carry as it is: refused.
        CHECK(SearchBackend::withPromptFile(ITEMS.front().actionCommand, "/tmp/a b").empty());
        CHECK(SearchBackend::withPromptFile(ITEMS.front().actionCommand, "/tmp/a'$(id)").empty());
        // A row without a prompt opens the agent as it is, no runner.
        for (const auto& IT : SearchBackend::promptAllAgents(""))
            CHECK(IT.agentPrompt.empty() && IT.actionCommand.find("agent-run") == std::string::npos);
        fs::remove_all(DIR);
    }

    std::cout << "search_backend_test: " << s_testsPassed << " checks OK" << std::endl;
    return 0;
}

// SearchBackend folds the dictionary/translator's generation into its own; the
// tool worker needs the compositor's config, so the host test stands it in.
namespace ToolSearch {
    uint64_t generation() {
        return 0;
    }
}
