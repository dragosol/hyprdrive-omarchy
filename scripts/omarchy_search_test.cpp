#include "../OmarchySearch.hpp"
#include <cassert>
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
    }

    // 3. Omarchy Menu Actions Search
    {
        auto menuResults = OmarchySearch::searchMenu("lock", 5);
        // "lock" or "theme" or "screenshot" or "sound" should match items if default menu exists
    }

    std::cout << "omarchy_search_test: " << s_testsPassed << " checks OK" << std::endl;
    return 0;
}
