// ToolSearch's parser: what the search field asks the dictionary and the
// translator for. Pure, so it is held to its cases here rather than on screen.
#include "../ToolSearchParse.hpp"

#include <cstdio>
#include <string>

static int g_fail = 0, g_checks = 0;

static void check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        ++g_fail;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

static void expect(const std::string& in, ToolSearch::EKind kind, const std::string& term, const std::string& from, const std::string& to) {
    const auto Q = ToolSearch::parse(in);
    check(Q.kind == kind && Q.term == term && Q.from == from && Q.to == to,
          "'" + in + "' -> kind " + std::to_string(int(Q.kind)) + " term '" + Q.term + "' from '" + Q.from + "' to '" + Q.to + "'");
}

int main() {
    using K = ToolSearch::EKind;

    // Not a tool query at all.
    expect("serendipity", K::None, "", "", "");
    expect("firefox", K::None, "", "", "");
    expect("", K::None, "", "", "");
    expect("definetly", K::None, "", "", "");
    expect("trash", K::None, "", "", "");

    // The dictionary.
    expect("define serendipity", K::Define, "serendipity", "", "");
    expect("def serendipity", K::Define, "serendipity", "", "");
    expect("dict  serendipity ", K::Define, "serendipity", "", "");
    expect("Define: ice cream", K::Define, "ice cream", "", "");
    expect("define", K::Define, "", "", "");

    // The translator: a language when text follows it...
    expect("tr es good morning", K::Translate, "good morning", "", "es");
    expect("tr spanish good morning", K::Translate, "good morning", "", "es");
    expect("translate ro hello", K::Translate, "hello", "", "ro");
    expect("tr en:fr good morning", K::Translate, "good morning", "en", "fr");
    expect("tr en>de hello", K::Translate, "hello", "en", "de");
    expect("tr en->ja hello", K::Translate, "hello", "en", "ja");
    expect("tr pt-br obrigado", K::Translate, "obrigado", "", "pt-BR");
    // ...and not when it IS the text, or is not a language.
    expect("tr hello", K::Translate, "hello", "", "");
    expect("tr es", K::Translate, "es", "", "");
    expect("translate good morning", K::Translate, "good morning", "", "");
    expect("tr xx hello", K::Translate, "xx hello", "", "");
    expect("tr", K::Translate, "", "", "");

    // languageCode.
    check(ToolSearch::languageCode("Spanish") == "es", "Spanish -> es");
    check(ToolSearch::languageCode("ro") == "ro", "ro -> ro");
    check(ToolSearch::languageCode("zz").empty(), "zz -> none");
    check(ToolSearch::languageCode("pt-BR") == "pt-BR", "pt-BR kept");
    check(ToolSearch::languageCode("en-12").empty(), "en-12 rejected");

    // A runaway paste is capped.
    check(ToolSearch::parse("define " + std::string(2000, 'a')).term.size() == 400, "term capped at 400");

    if (g_fail) {
        std::printf("tool_search_test: %d of %d checks failed\n", g_fail, g_checks);
        return 1;
    }
    std::printf("tool_search_test: %d checks OK\n", g_checks);
    return 0;
}
