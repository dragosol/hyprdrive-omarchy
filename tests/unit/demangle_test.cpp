// Host test for Demangle.hpp: `hyprdrive:debug hooks` names and signatures,
// and the exception types TerminateLog writes, as a person reads them.
#include "Demangle.hpp"

#include <cstdio>
#include <stdexcept>
#include <string>
#include <typeinfo>

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

int main() {
    // Two hook names main.cpp asks for by their mangled symbol.
    const std::string FRAME = "_ZN7Monitor8CMonitor13scheduleFrameEN10Aquamarine7IOutput19scheduleFrameReasonE";
    CHECK(demangle(FRAME) == "Monitor::CMonitor::scheduleFrame(Aquamarine::IOutput::scheduleFrameReason)");
    const std::string SURFACE = "_ZN18CWLSurfaceResource5frameERKNSt6chrono10time_pointINS0_3_V212steady_clockENS0_8durationIlSt5ratioILl1ELl1000000000EEEEEE";
    CHECK(demangle(SURFACE).starts_with("CWLSurfaceResource::frame(std::chrono::time_point<std::chrono::_V2::steady_clock"));
    // A plain name, as most hooks are asked for, stays as it is.
    CHECK(demangle("renderWorkspace") == "renderWorkspace");
    CHECK(demangle("") == "");
    // Not a mangled name: never "int", never a mangled-looking mess.
    CHECK(demangle("i") == "i");
    CHECK(demangle("_Z") == "_Z");
    CHECK(demangle("_Zgarbage!") == "_Zgarbage!");

    // The signature: the symbol demangled wins over nm's line...
    CHECK(readableSignature(FRAME, "") == demangle(FRAME));
    CHECK(readableSignature(FRAME, "something else entirely") == demangle(FRAME));
    // ...nm's line (demangled if it came back mangled) for a C symbol...
    CHECK(readableSignature("wl_display_flush_clients", "wl_display_flush_clients") == "wl_display_flush_clients");
    CHECK(readableSignature("cfn", FRAME) == demangle(FRAME));
    // ...and the symbol when there is nothing else.
    CHECK(readableSignature("cfn", "") == "cfn");

    // Exception types, from type_info::name() (no _Z prefix).
    CHECK(demangleType(typeid(std::runtime_error).name()) == "std::runtime_error");
    CHECK(demangleType(typeid(int).name()) == "int");
    CHECK(demangleType("") == "");
    CHECK(demangleType(nullptr).empty());

    std::printf("demangle_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
