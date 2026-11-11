// Host test for TerminateLog.hpp, in child processes that really terminate:
// the record names the exception's demangled type and what() (or that there
// was none) and carries a backtrace; the previous handler still runs and the
// process still dies of SIGABRT (nothing swallowed); the file is created 0600
// in a 0700 directory, never through a symlink, and started over past CAP;
// uninstall() restores the previous handler, and refuses over a newer one.
#include "Demangle.hpp"
#include "TerminateLog.hpp"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace Hyprdrive;

static int g_checks = 0, g_failed = 0;
#define CHECK(c)                                                                                                                                                                   \
    do {                                                                                                                                                                           \
        ++g_checks;                                                                                                                                                                \
        if (!(c)) {                                                                                                                                                                \
            ++g_failed;                                                                                                                                                            \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);                                                                                                               \
        }                                                                                                                                                                          \
    } while (0)

static std::string slurp(const fs::path& p) {
    std::ifstream     f(p);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

static mode_t modeOf(const fs::path& p) {
    struct stat st {};
    return ::lstat(p.c_str(), &st) == 0 ? (st.st_mode & 0777) : 0;
}

static const char* g_marker = nullptr;
[[noreturn]] static void previousHandler() {
    // The handler that was there before: it must still run, and aborts.
    if (g_marker)
        std::ofstream(g_marker) << "previous ran";
    std::abort();
}

[[noreturn]] static void thrower() {
    throw std::runtime_error("boom from a worker");
}
static void throwsThroughNoexcept() noexcept {
    thrower(); // the point: an exception leaving a noexcept function terminates
}

// Runs `body` in a child with XDG_STATE_HOME=`state`; true when the child died
// of SIGABRT.
template <typename F>
static bool abortsIn(const fs::path& state, F body) {
    const pid_t PID = ::fork();
    if (PID == 0) {
        ::setenv("XDG_STATE_HOME", state.c_str(), 1);
        // No core file for an expected abort.
        std::signal(SIGABRT, SIG_DFL);
        body();
        ::_exit(0); // not reached
    }
    int status = 0;
    ::waitpid(PID, &status, 0);
    return WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
}

int main() {
    char       tmpl[] = "/tmp/terminate_log_test.XXXXXX";
    const auto ROOT   = fs::path{::mkdtemp(tmpl)};

    // ---- the path ---------------------------------------------------------
    CHECK(TerminateLog::pathFor("/s", "/h") == "/s/hyprdrive/terminate.log");
    CHECK(TerminateLog::pathFor(nullptr, "/h") == "/h/.local/state/hyprdrive/terminate.log");
    CHECK(TerminateLog::pathFor("relative", "/h") == "/h/.local/state/hyprdrive/terminate.log");
    CHECK(TerminateLog::pathFor("", "") .empty());
    CHECK(TerminateLog::pathFor(nullptr, nullptr).empty());

    // ---- demangling (Demangle.hpp, which the record uses) ----------------
    CHECK(demangleType(typeid(std::runtime_error).name()) == "std::runtime_error");
    CHECK(demangle("_ZN7Monitor8CMonitor13scheduleFrameEv") == "Monitor::CMonitor::scheduleFrame()");
    CHECK(demangle("renderWorkspace") == "renderWorkspace");
    CHECK(demangle("_Znot a symbol") == "_Znot a symbol");
    CHECK(demangle("i") == "i"); // not "int": only _Z names are demangled

    // ---- an uncaught exception -------------------------------------------
    {
        const auto STATE  = ROOT / "a";
        const auto MARKER = ROOT / "a-marker";
        g_marker          = MARKER.c_str();
        CHECK(abortsIn(STATE, [] {
            std::set_terminate(previousHandler);
            TerminateLog::install();
            throwsThroughNoexcept();
        }));
        const auto LOG  = STATE / "hyprdrive" / "terminate.log";
        const auto TEXT = slurp(LOG);
        CHECK(TEXT.find("---- std::terminate in pid") != std::string::npos);
        CHECK(TEXT.find("exception: std::runtime_error") != std::string::npos);
        CHECK(TEXT.find("what(): boom from a worker") != std::string::npos);
        CHECK(TEXT.find("backtrace:\n") != std::string::npos);
        CHECK(TEXT.find("[0x") != std::string::npos); // backtrace_symbols_fd's frames
        CHECK(slurp(MARKER) == "previous ran");                      // chained, not swallowed
        CHECK(modeOf(LOG) == 0600);
        CHECK(modeOf(STATE / "hyprdrive") == 0700);
    }

    // ---- std::terminate with nothing in flight, the default handler -------
    {
        const auto STATE = ROOT / "b";
        CHECK(abortsIn(STATE, [] {
            ::close(2); // the default handler's message is not wanted in the test log
            TerminateLog::install();
            std::terminate();
        }));
        CHECK(slurp(STATE / "hyprdrive" / "terminate.log").find("exception: none in flight") != std::string::npos);
    }

    // ---- the cap: a full file starts over -------------------------------
    {
        const auto STATE = ROOT / "c";
        fs::create_directories(STATE / "hyprdrive");
        const auto LOG = STATE / "hyprdrive" / "terminate.log";
        std::ofstream(LOG) << std::string(TerminateLog::CAP + 10, 'x');
        CHECK(abortsIn(STATE, [] {
            ::close(2);
            TerminateLog::install();
            std::terminate();
        }));
        const auto TEXT = slurp(LOG);
        CHECK(TEXT.size() < TerminateLog::CAP);
        CHECK(TEXT.starts_with("---- std::terminate"));
    }

    // ---- never through a symlink ------------------------------------------
    {
        const auto STATE = ROOT / "d";
        fs::create_directories(STATE / "hyprdrive");
        const auto TARGET = ROOT / "d-target";
        std::ofstream(TARGET) << "keep";
        fs::create_symlink(TARGET, STATE / "hyprdrive" / "terminate.log");
        CHECK(abortsIn(STATE, [] {
            ::close(2);
            TerminateLog::install();
            std::terminate();
        }));
        CHECK(slurp(TARGET) == "keep");
    }

    // ---- install / uninstall, in this process -----------------------------
    {
        const auto BEFORE = std::get_terminate();
        TerminateLog::install();
        CHECK(TerminateLog::installed());
        CHECK(std::get_terminate() != BEFORE);
        CHECK(TerminateLog::uninstall());
        CHECK(std::get_terminate() == BEFORE);
        CHECK(!TerminateLog::installed());
        // Someone else's handler over ours: left alone.
        TerminateLog::install();
        const auto OURS = std::get_terminate();
        std::set_terminate(previousHandler);
        CHECK(!TerminateLog::uninstall());
        CHECK(std::get_terminate() == previousHandler);
        std::set_terminate(OURS);
        CHECK(TerminateLog::uninstall());
        CHECK(std::get_terminate() == BEFORE);
    }

    std::error_code ec;
    fs::remove_all(ROOT, ec);
    std::printf("terminate_log_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
