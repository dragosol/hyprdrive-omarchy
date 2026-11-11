#include "Sources.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>

namespace fs = std::filesystem;

namespace Hyprdrive::AppIdentity {
    namespace {
        std::string envOr(const char* name, const std::string& fallback) {
            const char* V = std::getenv(name);
            return V && *V == '/' ? std::string{V} : fallback;
        }

        // A regular file, opened without blocking (a FIFO or a device named like
        // a manifest must not stall the reader).
        int openRegular(const std::string& p, struct stat& st) {
            const int FD = ::open(p.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOCTTY);
            if (FD < 0)
                return -1;
            if (::fstat(FD, &st) != 0 || !S_ISREG(st.st_mode)) {
                ::close(FD);
                return -1;
            }
            return FD;
        }
    }

    Env Env::forHome(const std::string& home) {
        return {.home = home, .dataHome = home + "/.local/share", .stateHome = home + "/.local/state"};
    }

    Env Env::fromProcess() {
        const char* H = std::getenv("HOME");
        Env         e = forHome(H && *H ? H : "");
        e.dataHome    = envOr("XDG_DATA_HOME", e.dataHome);
        e.stateHome   = envOr("XDG_STATE_HOME", e.stateHome);
        return e;
    }

    FileSources::FileSources(Options o) : m_o(std::move(o)) {
        if (m_o.iconDir.empty() && !m_o.env.dataHome.empty())
            m_o.iconDir = m_o.env.dataHome + "/hyprdrive/icons";
    }

    const Env& FileSources::env() const {
        return m_o.env;
    }

    std::string FileSources::mapped(const std::string& path) const {
        return m_o.root + path;
    }

    std::optional<std::vector<uint8_t>> FileSources::read(const std::string& path, size_t cap) const {
        try {
            if (!path.starts_with('/'))
                return std::nullopt;
            struct stat st{};
            const int   FD = openRegular(mapped(path), st);
            if (FD < 0)
                return std::nullopt;
            std::vector<uint8_t> out;
            // st_size is 0 for /proc files: read until EOF, never past cap + 1.
            const size_t HINT = st.st_size > 0 ? static_cast<size_t>(st.st_size) : 4096;
            if (st.st_size > 0 && HINT > cap) {
                ::close(FD);
                return std::nullopt;
            }
            out.resize(std::min(HINT, cap) + 1);
            size_t got = 0;
            for (;;) {
                if (got == out.size()) {
                    if (out.size() > cap)
                        break;
                    out.resize(std::min(out.size() * 2, cap + 1));
                }
                const ssize_t N = ::read(FD, out.data() + got, out.size() - got);
                if (N <= 0)
                    break;
                got += static_cast<size_t>(N);
            }
            ::close(FD);
            if (got > cap)
                return std::nullopt;
            out.resize(got);
            return out;
        } catch (...) { return std::nullopt; }
    }

    std::vector<uint8_t> FileSources::readRange(const std::string& path, uint64_t off, size_t n) const {
        try {
            if (!path.starts_with('/'))
                return {};
            struct stat st{};
            const int   FD = openRegular(mapped(path), st);
            if (FD < 0)
                return {};
            std::vector<uint8_t> out;
            const uint64_t       SIZE = static_cast<uint64_t>(std::max<off_t>(st.st_size, 0));
            if (off < SIZE) {
                out.resize(static_cast<size_t>(std::min<uint64_t>({n, Caps::RSRC, SIZE - off})));
                size_t got = 0;
                while (got < out.size()) {
                    const ssize_t N = ::pread(FD, out.data() + got, out.size() - got, static_cast<off_t>(off + got));
                    if (N <= 0)
                        break;
                    got += static_cast<size_t>(N);
                }
                out.resize(got);
            }
            ::close(FD);
            return out;
        } catch (...) { return {}; }
    }

    std::optional<FileInfo> FileSources::stat(const std::string& path) const {
        if (!path.starts_with('/'))
            return std::nullopt;
        struct stat st{};
        if (::stat(mapped(path).c_str(), &st) != 0)
            return std::nullopt;
        return FileInfo{.size    = static_cast<uint64_t>(std::max<off_t>(st.st_size, 0)),
                        .mtimeNs = static_cast<int64_t>(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec,
                        .dir     = S_ISDIR(st.st_mode)};
    }

    std::vector<std::string> FileSources::list(const std::string& dir, size_t max) const {
        std::vector<std::string> out;
        try {
            if (!dir.starts_with('/'))
                return out;
            std::error_code ec;
            for (fs::directory_iterator it(mapped(dir), fs::directory_options::skip_permission_denied, ec), end; !ec && it != end && out.size() < max; it.increment(ec))
                out.push_back(it->path().filename().string());
            std::ranges::sort(out);
        } catch (...) { out.clear(); }
        return out;
    }

    std::optional<std::vector<Lutris::Row>> FileSources::lutrisGames(const std::string& dbPath) const {
        try {
            if (m_o.lutrisDb) {
                if (!exists(dbPath))
                    return std::nullopt;
                return m_o.lutrisDb->games(mapped(dbPath));
            }
            const auto TSV = read(dbPath + ".tsv", Caps::VDF);
            if (!TSV)
                return std::nullopt;
            return Lutris::parsePgaTsv(*TSV);
        } catch (...) { return std::nullopt; }
    }

    std::string FileSources::storedIcon(const std::string& name) const {
        if (m_o.iconDir.empty() || name.empty() || name.find('/') != std::string::npos)
            return "";
        const std::string P = m_o.iconDir + "/" + name + ".png";
        return exists(P) ? P : "";
    }

    std::string FileSources::storeIcon(const std::string& name, const Ico::Picked& image) const {
        try {
            if (!m_o.writePng || m_o.iconDir.empty() || name.empty() || name.find('/') != std::string::npos)
                return "";
            if (auto have = storedIcon(name); !have.empty())
                return have;
            std::error_code ec;
            fs::create_directories(mapped(m_o.iconDir), ec);
            const std::string P = m_o.iconDir + "/" + name + ".png";
            return m_o.writePng(image, mapped(P)) ? P : "";
        } catch (...) { return ""; }
    }
}
