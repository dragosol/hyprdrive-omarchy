#include "../OmarchySearch.hpp"
#include "../SearchIntent.hpp"
#include <cassert>
#include <chrono>
#include <iostream>
#include <cmath>

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
        auto calc1 = OmarchySearch::evaluateCalculator("2 + 2");
        CHECK(calc1.has_value());
        CHECK(calc1->name == "= 4");
        CHECK(calc1->actionCommand == "4");
        CHECK(calc1->category == "Calculator");

        auto calc2 = OmarchySearch::evaluateCalculator("10 * 5 - 3");
        CHECK(calc2.has_value());
        CHECK(calc2->name == "= 47");
        CHECK(calc2->actionCommand == "47");

        auto calc3 = OmarchySearch::evaluateCalculator("(10 + 20) / 3");
        CHECK(calc3.has_value());
        CHECK(calc3->name == "= 10");
        CHECK(calc3->actionCommand == "10");

        auto calc4 = OmarchySearch::evaluateCalculator("2^3");
        CHECK(calc4.has_value());
        CHECK(calc4->name == "= 8");
        CHECK(calc4->actionCommand == "8");

        auto calc5 = OmarchySearch::evaluateCalculator("sqrt(16)");
        CHECK(calc5.has_value());
        CHECK(calc5->name == "= 4");
        CHECK(calc5->actionCommand == "4");

        auto calc6 = OmarchySearch::evaluateCalculator("sin(0)");
        CHECK(calc6.has_value());
        CHECK(calc6->name == "= 0");
        CHECK(calc6->actionCommand == "0");

        // Non-math queries should return nullopt
        auto calcNone1 = OmarchySearch::evaluateCalculator("hello world");
        CHECK(!calcNone1.has_value());

        auto calcNone2 = OmarchySearch::evaluateCalculator("firefox");
        CHECK(!calcNone2.has_value());

        // Bare number without operator should return nullopt
        auto calcNone3 = OmarchySearch::evaluateCalculator("42");
        CHECK(!calcNone3.has_value());

        // Division by zero should not crash
        auto calcDivZero = OmarchySearch::evaluateCalculator("10 / 0");
        if (calcDivZero.has_value()) {
            CHECK(calcDivZero->name.find("Error") != std::string::npos || calcDivZero->name.find("inf") != std::string::npos);
        }
    }

    // 2. AI Agent queries
    {
        CHECK(OmarchySearch::isAgentQuery("/grok what is the weather today"));
        CHECK(OmarchySearch::isAgentQuery("/agy fix the build"));
        CHECK(OmarchySearch::isAgentQuery("/gemini help me"));
        CHECK(OmarchySearch::isAgentQuery("/claude review"));
        CHECK(OmarchySearch::isAgentQuery("/opencode"));
        CHECK(OmarchySearch::isAgentQuery("/codex"));
        CHECK(OmarchySearch::isAgentQuery("/copilot"));
        CHECK(!OmarchySearch::isAgentQuery("grok what is the weather"));
        CHECK(!OmarchySearch::isAgentQuery("firefox"));

        auto grokResults = OmarchySearch::searchAgents("/grok what is the weather today");
        CHECK(!grokResults.empty());
        CHECK(grokResults.front().name.find("Grok") != std::string::npos);
        CHECK(grokResults.front().category == "AI Agent");
        CHECK(grokResults.front().actionCommand.find("what is the weather today") != std::string::npos);

        auto agyResults = OmarchySearch::searchAgents("/agy solve this problem");
        CHECK(!agyResults.empty());
        CHECK(agyResults.front().name.find("AGY") != std::string::npos || agyResults.front().name.find("Antigravity") != std::string::npos);
        CHECK(agyResults.front().category == "AI Agent");
        CHECK(agyResults.front().actionCommand.find("solve this problem") != std::string::npos);

        auto all = OmarchySearch::promptAllAgents("what is the weather today");
        // Sentence queries only list agents that are installed and logged in
        // (usage record with ready:true), not the full catalog.
        CHECK(all.size() < 8);
        for (const auto& it : all) {
            CHECK(it.description == "what is the weather today");
            CHECK(it.actionCommand.find("what is the weather today") != std::string::npos);
            CHECK(it.kind == OmarchySearch::EItemKind::Agent);
        }

        CHECK(OmarchySearch::isGlyphIcon("\uF249"));
        CHECK(!OmarchySearch::isGlyphIcon("folder"));
        CHECK(!OmarchySearch::isGlyphIcon("/usr/share/pixmaps/x.svg"));
    }

    // 3. Omarchy Menu Actions Search — must not fork. The old path ran
    // system("bash -c when") for every menu row on the first query and froze
    // the compositor for seconds.
    {
        const auto t0 = std::chrono::steady_clock::now();
        auto menuResults = OmarchySearch::searchMenu("lock", 5);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        CHECK(ms < 250);
        (void)menuResults;

        const auto t1 = std::chrono::steady_clock::now();
        auto again = OmarchySearch::searchMenu("firefox", 8);
        const auto ms2 = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t1).count();
        CHECK(ms2 < 100);
        (void)again;

        auto installHits = OmarchySearch::searchMenu("install", 8);
        std::string installId;
        for (const auto& it : installHits) {
            if (it.name == "Install" && it.kind == OmarchySearch::EItemKind::Menu) {
                installId = it.id;
                CHECK(!it.icon.empty());
                CHECK(OmarchySearch::isGlyphIcon(it.icon));
                break;
            }
        }
        CHECK(!installId.empty());
        OmarchySearch::navigateMenu(installId);
        auto kids = OmarchySearch::searchMenu("", 24);
        bool hasBack = false, hasPackage = false;
        for (const auto& it : kids) {
            if (it.kind == OmarchySearch::EItemKind::Back)
                hasBack = true;
            if (it.name == "Package") {
                hasPackage = true;
                CHECK(!it.icon.empty());
            }
        }
        CHECK(hasBack);
        CHECK(hasPackage);
        OmarchySearch::resetMenuRoute();
    }

    // 4. File search is idle until a query is issued
    {
        CHECK(!OmarchySearch::fileSearchBusy());
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

    std::cout << "omarchy_search_test: " << s_testsPassed << " checks OK" << std::endl;
    return 0;
}
