// Mosaic: the sizes it remembers -- per app (persisted) and per window (across a reload).
// Split out of Mosaic.cpp; the state is still there (MosaicParts.hpp).
#include "MosaicParts.hpp"

namespace ScrollOverview::Mosaic {

    using namespace MosaicDetail;

    // ---------------------------------------------------------- remembered sizes --
    // AN APP'S MOSAIC SIZE (user, 2026-10-11: "if i close a particular app at
    // that size DURING MOSAIC then that becomes the app's default size when
    // mosaic is open. dwindle and scrolling modes don't apply"). Keyed on the
    // app's class; learned only when a window CLOSES while its desktop is in the
    // mosaic arrangement (not its tiles), and used only by mosaic. One line per
    // app: class<TAB>width<TAB>height, in $XDG_STATE_HOME/hyprdrive/mosaic-sizes.
    namespace MosaicDetail {

        std::filesystem::path sizesPath() {
            if (const char* S = std::getenv("XDG_STATE_HOME"); S && *S)
                return std::filesystem::path{S} / "hyprdrive" / "mosaic-sizes";
            const char* HOME = std::getenv("HOME");
            return std::filesystem::path{HOME ? HOME : "/tmp"} / ".local" / "state" / "hyprdrive" / "mosaic-sizes";
        }

        void loadSizes() {
            if (g_sizesLoaded)
                return;
            g_sizesLoaded = true;
            std::ifstream in(sizesPath());
            std::string   line;
            while (std::getline(in, line)) {
                const auto T1 = line.find('\t'), T2 = line.find('\t', T1 == std::string::npos ? 0 : T1 + 1);
                if (T1 == std::string::npos || T2 == std::string::npos)
                    continue;
                try {
                    g_sizes[line.substr(0, T1)] = Vector2D{std::stod(line.substr(T1 + 1, T2 - T1 - 1)), std::stod(line.substr(T2 + 1))};
                } catch (...) {}
            }
        }

        void saveSize(const std::string& app, const Vector2D& size) {
            loadSizes();
            const auto OLD = g_sizes.find(app);
            if (OLD != g_sizes.end() && std::abs(OLD->second.x - size.x) < 1 && std::abs(OLD->second.y - size.y) < 1)
                return; // nothing new: no write
            g_sizes[app] = size;
            std::error_code ec;
            std::filesystem::create_directories(sizesPath().parent_path(), ec);
            std::ostringstream out;
            for (const auto& [K, V] : g_sizes)
                out << K << '\t' << std::lround(V.x) << '\t' << std::lround(V.y) << '\n';
            const auto TMP = sizesPath().string() + ".tmp";
            if (std::ofstream f(TMP); f) {
                f << out.str();
                f.close();
                std::filesystem::rename(TMP, sizesPath(), ec);
            }
        }

        std::string appOf(const PHLWINDOW& w) {
            if (!w)
                return {};
            return !w->m_initialClass.empty() ? w->m_initialClass : w->m_class;
        }


        SKept* keptOf(const PHLWINDOW& w) {
            for (auto& e : g_windowSizes)
                if (e.window && e.window.lock() == w)
                    return &e;
            return nullptr;
        }

        void rememberWindow(const PHLWINDOW& w, const Vector2D& size, bool trusted = true) {
            if (!w)
                return;
            std::erase_if(g_windowSizes, [](const SKept& e) { return !e.window; });
            if (auto* e = keptOf(w)) {
                e->size         = size;
                e->floatedSince = false;
                e->trusted      = trusted;
                return;
            }
            g_windowSizes.push_back({.window = w, .size = size, .trusted = trusted});
        }
        bool keptTrusted(const PHLWINDOW& w) {
            const auto* e = keptOf(w);
            return !e || e->trusted;
        }

        void leftMosaic(const SP<Layout::ITarget>& t) {
            if (auto* e = keptOf(t ? t->window() : PHLWINDOW{})) {
                e->floatAtLeave = t->lastFloatingSize();
                e->floatedSince = false;
            }
        }

        void onFloating(const PHLWINDOW& w) {
            if (!w || !w->m_isFloating)
                return;
            // A Super+drag floats the window for the drag: that is not the
            // user floating it (user, 2026-10-15, fedora: dragging Steam --
            // an X11 client that resizes itself when floated -- came back at
            // 490x308, its kept 511x926 thrown away as "floated and resized").
            if (g_layoutManager)
                if (const auto DRAGGED = g_layoutManager->dragController()->target(); DRAGGED && DRAGGED->window() == w)
                    return;
            if (auto* e = keptOf(w))
                e->floatedSince = true;
        }

        // ACROSS A PLUGIN RELOAD (user, 2026-10-15: after a reload desktop 1
        // went to the tiles and never came back). Unloading hands every desktop
        // to dwindle, and a reloaded plugin starts with no kept sizes, so each
        // window re-arrived at its DWINDLE TILE as its own size (Pear Messages:
        // 1903x2094, full height) and the mosaic could never fit again. The
        // kept sizes now go to a file in XDG_RUNTIME_DIR on unload, stamped
        // with this Hyprland's instance, and come back on load before any
        // window re-arrives. Window addresses hold across a plugin reload.
        std::string keptPath() {
            const char* RT = std::getenv("XDG_RUNTIME_DIR");
            return std::string(RT && *RT ? RT : "/tmp") + "/hyprdrive-mosaic-kept";
        }
        std::string keptStamp() {
            const char* SIG = std::getenv("HYPRLAND_INSTANCE_SIGNATURE");
            return SIG ? SIG : "";
        }
        void saveKept() {
            std::ofstream f(keptPath(), std::ios::trunc);
            if (!f)
                return;
            f << keptStamp() << '\n';
            for (const auto& e : g_windowSizes)
                if (const auto W = e.window.lock())
                    f << std::format("{:x} {} {} {}\n", (uintptr_t)W.get(), e.size.x, e.size.y, e.trusted ? 1 : 0);
        }
        void loadKept() {
            const auto     PATH = keptPath();
            std::ifstream  f(PATH);
            std::string    stamp;
            if (!f || !std::getline(f, stamp) || stamp != keptStamp() || stamp.empty())
                return;
            std::unordered_map<uintptr_t, std::pair<Vector2D, bool>> saved;
            std::string                                              line;
            while (std::getline(f, line)) {
                std::istringstream in(line);
                std::string        addr;
                double             w = 0, h = 0;
                int                trusted = 1;
                if (in >> addr >> w >> h) {
                    in >> trusted;
                    saved[std::stoull(addr, nullptr, 16)] = {Vector2D{w, h}, trusted != 0};
                }
            }
            for (const auto& W : Desktop::windowState()->windows())
                if (W)
                    if (const auto IT = saved.find((uintptr_t)W.get()); IT != saved.end())
                        rememberWindow(W, IT->second.first, IT->second.second);
            std::error_code ec;
            std::filesystem::remove(PATH, ec);
        }

        std::optional<Vector2D> rememberedWindow(const SP<Layout::ITarget>& t) {
            const auto* e = keptOf(t ? t->window() : PHLWINDOW{});
            if (!e)
                return std::nullopt;
            const auto F = t->lastFloatingSize();
            if (e->floatedSince && (std::abs(F.x - e->floatAtLeave.x) > 2 || std::abs(F.y - e->floatAtLeave.y) > 2))
                return std::nullopt; // floated and resized there: its floating size decides
            return e->size;
        }
    }

}
