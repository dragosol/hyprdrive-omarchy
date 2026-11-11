#include "ProcFacts.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <format>

namespace Hyprdrive::AppIdentity::ProcFacts {
    namespace {
        constexpr size_t CMDLINE_CAP = 64u << 10;
        constexpr size_t STAT_CAP    = 4096;

        Bytes bytes(const std::string& s) {
            return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
        }

        std::string link(const std::string& path) {
            std::array<char, 4096> buf{};
            const ssize_t          N = ::readlink(path.c_str(), buf.data(), buf.size() - 1);
            return N > 0 ? std::string(buf.data(), static_cast<size_t>(N)) : std::string{};
        }

        bool exists(const std::string& path) {
            struct stat sb{};
            return ::stat(path.c_str(), &sb) == 0;
        }
    }

    std::optional<std::string> readCapped(const std::string& path, size_t cap) {
        const int FD = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOCTTY);
        if (FD < 0)
            return std::nullopt;
        std::string out;
        try {
            out.resize(cap);
            size_t got = 0;
            while (got < cap) {
                const ssize_t N = ::read(FD, out.data() + got, cap - got);
                if (N < 0) {
                    ::close(FD);
                    return std::nullopt;
                }
                if (N == 0)
                    break;
                got += static_cast<size_t>(N);
            }
            out.resize(got);
        } catch (...) {
            ::close(FD);
            return std::nullopt;
        }
        ::close(FD);
        return out;
    }

    bool keepForLaunch(std::string_view k) {
        // Wine's own bookkeeping between its processes, not a setting.
        for (const std::string_view NO : {"WINELOADERNOEXEC", "WINESERVERSOCKET", "WINEPRELOADRESERVE", "WINELOADER", "WINEDLLDIR0", "WINEPREFIX"})
            if (k == NO)
                return false;
        for (const std::string_view P : {"WINE", "DXVK_", "VKD3D_", "PROTON_", "STAGING_", "__GL_", "MESA_", "GAMEID", "STORE", "LD_LIBRARY_PATH", "SteamAppId", "SteamGameId"})
            if (k.starts_with(P))
                return true;
        return false;
    }

    uint64_t startOf(pid_t pid, const std::string& procRoot) {
        if (pid <= 0)
            return 0;
        const auto STAT = readCapped(std::format("{}/{}/stat", procRoot, pid), STAT_CAP);
        const auto S    = STAT ? parseStat(bytes(*STAT)) : std::nullopt;
        return S ? S->startTime : 0;
    }

    ProcessFacts read(pid_t pid, const std::string& procRoot) {
        ProcessFacts p;
        try {
            if (pid <= 0)
                return p;
            const std::string DIR  = std::format("{}/{}", procRoot, pid);
            const auto        STAT = readCapped(DIR + "/stat", STAT_CAP);
            const auto        S    = STAT ? parseStat(bytes(*STAT)) : std::nullopt;
            if (!S)
                return p;
            p.pid        = pid;
            p.comm       = S->comm;
            p.startTicks = S->startTime;
            if (const auto CMD = readCapped(DIR + "/cmdline", CMDLINE_CAP))
                p.argv0 = argv0Of(bytes(*CMD));
            if (const auto ENV = readCapped(DIR + "/environ", Caps::ENVIRON)) {
                p.env      = parseEnviron(bytes(*ENV));
                p.readable = true;
                // The relaunch settings, for a Wine process only.
                const bool WINE = !p.getenv("WINEPREFIX").empty() || (p.argv0.size() > 2 && p.argv0[1] == ':');
                for (size_t at = 0; WINE && at < ENV->size() && p.launchEnv.size() < 128;) {
                    size_t END = ENV->find('\0', at);
                    if (END == std::string::npos)
                        END = ENV->size();
                    const std::string_view KV{ENV->data() + at, END - at};
                    const auto             EQ = KV.find('=');
                    if (EQ != std::string_view::npos && EQ > 0 && KV.size() < 4096 && keepForLaunch(KV.substr(0, EQ)))
                        p.launchEnv.emplace_back(std::string{KV.substr(0, EQ)}, std::string{KV.substr(EQ + 1)});
                    at = END + 1;
                }
            } else
                p.readable = !p.argv0.empty(); // hidepid or another user: the cmdline may still be there
            p.exe = link(DIR + "/exe");
            if (!p.exe.empty() && p.exe.starts_with('/') && !exists(p.exe))
                if (const std::string IN = DIR + "/root" + p.exe; exists(IN))
                    p.exe = IN;
            // The parent chain, parent first.
            pid_t up = S->ppid;
            for (size_t hop = 0; hop < MAX_HOPS && up > 1; ++hop) {
                const std::string HD = std::format("{}/{}", procRoot, up);
                const auto        HS = readCapped(HD + "/stat", STAT_CAP);
                const auto        H  = HS ? parseStat(bytes(*HS)) : std::nullopt;
                if (!H)
                    break;
                ProcessFacts::Hop h{.pid = up, .comm = H->comm};
                if (const auto CMD = readCapped(HD + "/cmdline", CMDLINE_CAP))
                    h.cmdline = joinCmdline(bytes(*CMD));
                p.chain.push_back(std::move(h));
                up = H->ppid;
            }
        } catch (...) { p.readable = false; }
        return p;
    }
}
