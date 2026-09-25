// DockDropModel: what a drop onto the dock means -- the file list a drag
// source hands over, a free name in the destination, and the decision table.
#include "../DockDropModel.hpp"

#include <cstdio>
#include <set>
#include <string>

using namespace ScrollOverview::DockDropModel;

static int g_fail = 0, g_checks = 0;
static void check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) {
        ++g_fail;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

int main() {
    // uri-list: CRLF (the spec's), LF, comments, escapes, non-file schemes.
    auto P = parseUriList("file:///home/u/a.txt\r\nfile:///home/u/My%20File.pdf\r\n# a comment\r\nhttp://example.com/x\r\n\r\nfile:///home/u/caf%C3%A9.md\n");
    check(P.size() == 3, "3 paths, got " + std::to_string(P.size()));
    check(P.size() > 0 && P[0] == "/home/u/a.txt", "plain path");
    check(P.size() > 1 && P[1] == "/home/u/My File.pdf", "%20 decoded");
    check(P.size() > 2 && P[2] == "/home/u/café.md", "UTF-8 escape decoded");
    check(parseUriList("").empty(), "empty");
    check(parseUriList("file:///x").size() == 1 && parseUriList("file:///x")[0] == "/x", "no trailing newline");
    check(parseUriList("file://localhost/etc/hosts").size() == 1 && parseUriList("file://localhost/etc/hosts")[0] == "/etc/hosts", "localhost host");
    check(parseUriList("file://otherhost/etc/hosts").empty(), "remote host skipped");

    // uniqueName: never an existing name.
    std::set<std::string> have = {"/d/a.txt", "/d/a (2).txt", "/d/b", "/d/.bashrc", "/d/x.tar.gz"};
    const auto EXISTS = [&](const std::string& p) { return have.contains(p); };
    check(uniqueName("/d", "new.txt", EXISTS) == "new.txt", "free name kept");
    check(uniqueName("/d", "a.txt", EXISTS) == "a (3).txt", "a.txt -> a (3).txt, got " + uniqueName("/d", "a.txt", EXISTS));
    check(uniqueName("/d", "b", EXISTS) == "b (2)", "no extension");
    check(uniqueName("/d", ".bashrc", EXISTS) == ".bashrc (2)", "dotfile keeps its name whole");
    check(uniqueName("/d", "x.tar.gz", EXISTS) == "x (2).tar.gz", "double extension");

    // The decision table.
    using C = ECell;
    using A = EAction;
    check(resolve(C::Stack, 2, 0) == A::IntoStack, "files onto a stack");
    check(resolve(C::Stack, 0, 1) == A::IntoStack, "a folder onto a stack");
    check(resolve(C::Stack, 0, 0) == A::None, "nothing onto a stack");
    check(resolve(C::Trash, 1, 1) == A::ToTrash, "anything onto trash");
    check(resolve(C::Gap, 0, 1) == A::PinAt, "one folder onto a gap pins");
    check(resolve(C::Gap, 1, 0) == A::None, "a file onto a gap does nothing");
    check(resolve(C::Gap, 0, 2) == A::None, "two folders onto a gap do nothing");
    check(resolve(C::Gap, 1, 1) == A::None, "mixed onto a gap does nothing");
    check(resolve(C::App, 1, 0) == A::None, "onto an app: out of scope");
    check(resolve(C::None, 1, 0) == A::None, "nowhere");

    // isInside: a folder must never move into itself.
    check(isInside("/a/b", "/a"), "/a/b in /a");
    check(isInside("/a", "/a"), "/a in /a");
    check(!isInside("/ab", "/a"), "/ab not in /a");
    check(!isInside("/a", "/a/b"), "/a not in /a/b");
    check(isInside("/a/b/", "/a/"), "trailing slashes");

    std::printf("dock_drop_test: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
