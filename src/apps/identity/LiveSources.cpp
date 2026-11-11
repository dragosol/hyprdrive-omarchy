// The live system's Sources: the process's own environment, Lutris's pga.db
// through the system's libsqlite3 opened at run time (no build dependency; a
// system without it has no Lutris games here, as in GameApps.cpp), and icons
// written as PNG through cairo (IconPng.hpp).

#include "IconPng.hpp"
#include "Sources.hpp"

#include <dlfcn.h>

namespace Hyprdrive::AppIdentity {
    namespace {
        struct SSqlite {
            void* lib = nullptr;
            int (*open)(const char*, void**, int, const char*)           = nullptr;
            int (*prepare)(void*, const char*, int, void**, const char**) = nullptr;
            int (*step)(void*)                                            = nullptr;
            const unsigned char* (*text)(void*, int)                      = nullptr;
            long long (*integer)(void*, int)                              = nullptr;
            int (*finalize)(void*)                                        = nullptr;
            int (*close)(void*)                                           = nullptr;
            bool ok() const {
                return open && prepare && step && text && integer && finalize && close;
            }
        };
        const SSqlite& sqlite() {
            static const SSqlite S = [] {
                SSqlite s;
                for (const char* NAME : {"libsqlite3.so.0", "libsqlite3.so"})
                    if ((s.lib = dlopen(NAME, RTLD_NOW | RTLD_LOCAL)))
                        break;
                if (!s.lib)
                    return s;
                s.open     = reinterpret_cast<decltype(s.open)>(dlsym(s.lib, "sqlite3_open_v2"));
                s.prepare  = reinterpret_cast<decltype(s.prepare)>(dlsym(s.lib, "sqlite3_prepare_v2"));
                s.step     = reinterpret_cast<decltype(s.step)>(dlsym(s.lib, "sqlite3_step"));
                s.text     = reinterpret_cast<decltype(s.text)>(dlsym(s.lib, "sqlite3_column_text"));
                s.integer  = reinterpret_cast<decltype(s.integer)>(dlsym(s.lib, "sqlite3_column_int64"));
                s.finalize = reinterpret_cast<decltype(s.finalize)>(dlsym(s.lib, "sqlite3_finalize"));
                s.close    = reinterpret_cast<decltype(s.close)>(dlsym(s.lib, "sqlite3_close"));
                return s;
            }();
            return S;
        }

        struct SSqliteDb final : Lutris::Db {
            std::optional<std::vector<Lutris::Row>> games(const std::string& path) const override {
                const auto& Q = sqlite();
                if (!Q.ok())
                    return std::nullopt;
                void* db = nullptr;
                if (Q.open(path.c_str(), &db, 0x00000001 /* SQLITE_OPEN_READONLY */, nullptr) != 0 || !db) {
                    if (db)
                        Q.close(db);
                    return std::nullopt;
                }
                std::vector<Lutris::Row> out;
                void*                    st = nullptr;
                // service/service_id are newer columns: without them, the rest.
                bool withService = Q.prepare(db, "SELECT id, slug, name, runner, directory, configpath, installed, service, service_id FROM games", -1, &st, nullptr) == 0 && st;
                if (!withService) {
                    st = nullptr;
                    if (Q.prepare(db, "SELECT id, slug, name, runner, directory, configpath, installed FROM games", -1, &st, nullptr) != 0)
                        st = nullptr;
                }
                if (st) {
                    while (out.size() < Lutris::MAX_ROWS && Q.step(st) == 100 /* SQLITE_ROW */) {
                        const auto TXT = [&](int c) {
                            const auto* T = Q.text(st, c);
                            return T ? std::string{reinterpret_cast<const char*>(T)} : std::string{};
                        };
                        Lutris::Row r;
                        r.id         = Q.integer(st, 0);
                        r.slug       = TXT(1);
                        r.name       = TXT(2);
                        r.runner     = TXT(3);
                        r.directory  = TXT(4);
                        r.configpath = TXT(5);
                        r.installed  = Q.integer(st, 6) == 1;
                        if (withService) {
                            r.service   = TXT(7);
                            r.serviceId = TXT(8);
                        }
                        out.push_back(std::move(r));
                    }
                    Q.finalize(st);
                }
                Q.close(db);
                return out;
            }
        };
    }

    const Sources& liveSources() {
        static const FileSources S{FileSources::Options{
            .env      = Env::fromProcess(),
            .lutrisDb = std::make_shared<SSqliteDb>(),
            .writePng = [](const Ico::Picked& p, const std::string& out) { return IconPng::write(p, out); },
        }};
        return S;
    }
}
