#pragma once

// Everything AppIdentity reads from disk goes through a Sources: bounded
// reads (a file over its cap is refused, never truncated into a misparse),
// directory listings capped at Caps::DIR_ENTRIES, stat, Lutris's database and
// the one write it makes (an extracted icon as a PNG). The resolvers and the
// library indexes take a `const Sources&`, so a test hands them a fixture tree
// and the plugin hands them liveSources().
//
// FileSources is both: paths are the real absolute paths ("/home/user/..."),
// read under `root` -- "" for the live system, a fixture directory in tests
// (tests/fixtures/identity/steam-native + "/home/user/.local/share/Steam").
// Every std::filesystem call takes an error_code, every read is open/fstat/
// read with O_NONBLOCK (a FIFO named like a manifest cannot stall it), and
// nothing throws past these functions.

#include "Caps.hpp"
#include "Ico.hpp"
#include "LutrisDb.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Hyprdrive::AppIdentity {
    struct FileInfo {
        uint64_t size    = 0;
        int64_t  mtimeNs = 0;
        bool     dir     = false;
    };

    // Where a user's things are. fromProcess(): $HOME, $XDG_DATA_HOME, ...
    struct Env {
        std::string        home, dataHome, stateHome;
        static Env         fromProcess();
        static Env         forHome(const std::string& home); // the XDG defaults under `home`
    };

    class Sources {
      public:
        virtual ~Sources() = default;
        virtual const Env& env() const = 0;
        // The whole file, or nullopt when it is missing, not a regular file or
        // larger than `cap`.
        virtual std::optional<std::vector<uint8_t>> read(const std::string& path, size_t cap) const = 0;
        // Bytes [off, off + n) of a regular file (fewer at its end); n is
        // capped at Caps::RSRC.
        virtual std::vector<uint8_t> readRange(const std::string& path, uint64_t off, size_t n) const = 0;
        virtual std::optional<FileInfo> stat(const std::string& path) const = 0;
        // Entry names of a directory, sorted, at most `max`.
        virtual std::vector<std::string> list(const std::string& dir, size_t max = Caps::DIR_ENTRIES) const = 0;
        // Lutris's `games` rows from a pga.db, or nullopt.
        virtual std::optional<std::vector<Lutris::Row>> lutrisGames(const std::string& dbPath) const = 0;
        // An icon kept as `<icon dir>/<name>.png`: the path when it exists or
        // was written, "" when this Sources writes no icons.
        virtual std::string storeIcon(const std::string& name, const Ico::Picked& image) const = 0;
        // That icon's path if it is already there ("" if not): lets a caller
        // skip the decode.
        virtual std::string storedIcon(const std::string& name) const = 0;

        bool exists(const std::string& path) const {
            return stat(path).has_value();
        }
    };

    class FileSources final : public Sources {
      public:
        struct Options {
            std::string root;     // prefix every path is read under ("" = the real system)
            Env         env;      // the paths' own HOME and XDG dirs (not prefixed)
            std::string iconDir;  // where storeIcon writes (real path, prefixed by root); "" = dataHome/hyprdrive/icons
            // pga.db: through this Db, else (null) from "<db>.tsv" (Lutris::parsePgaTsv).
            std::shared_ptr<const Lutris::Db>                               lutrisDb;
            std::function<bool(const Ico::Picked&, const std::string& out)> writePng; // null: no icons written
        };
        explicit FileSources(Options o);

        const Env&                              env() const override;
        std::optional<std::vector<uint8_t>>     read(const std::string& path, size_t cap) const override;
        std::vector<uint8_t>                    readRange(const std::string& path, uint64_t off, size_t n) const override;
        std::optional<FileInfo>                 stat(const std::string& path) const override;
        std::vector<std::string>                list(const std::string& dir, size_t max = Caps::DIR_ENTRIES) const override;
        std::optional<std::vector<Lutris::Row>> lutrisGames(const std::string& dbPath) const override;
        std::string                             storeIcon(const std::string& name, const Ico::Picked& image) const override;
        std::string                             storedIcon(const std::string& name) const override;

        // The on-disk path for a real path (root + path).
        std::string mapped(const std::string& path) const;

      private:
        Options m_o;
    };

    // The live system's Sources (LiveSources.cpp): the process's environment,
    // pga.db through the system's libsqlite3 (dlopen'd), icons written as PNG
    // through cairo into $XDG_DATA_HOME/hyprdrive/icons.
    const Sources& liveSources();
}
