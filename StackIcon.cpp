// THE STACK'S DOCK CELL (dock stacks A): the 4 newest items as a slightly
// messy pile inside a thin outline -- or the folder's icon (Display as Folder),
// a dimmed icon for a folder that is gone, the folder inside the outline for an
// empty one. Everything here draws from cached textures; the listing comes from
// StackReader, never from the disk on this thread.
#include "BuiltinIcons.hpp"
#include "DockStacks.hpp"
#include "Thumbs.hpp"
#include <chrono>
#include <hyprland/src/render/pass/RectPassElement.hpp>
#include "LauncherEntry.hpp"
#include "Config.hpp"
#include "OverviewRender.hpp"
#include "StackReader.hpp"

#include <gio/gio.h>

#include <unordered_map>

#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/BorderPassElement.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>

namespace ScrollOverview::Stacks {

    namespace {
        // The freedesktop thumbnail cache: ~/.cache/thumbnails/{large,normal}/md5(uri).png.
        // Only what is ALREADY there -- this never generates a thumbnail.
        std::string cachedThumbnail(const std::string& path) {
            char* uri = g_filename_to_uri(path.c_str(), nullptr, nullptr);
            if (!uri)
                return {};
            char* md5 = g_compute_checksum_for_string(G_CHECKSUM_MD5, uri, -1);
            g_free(uri);
            std::string out;
            for (const char* SIZE : {"large", "normal"}) {
                const std::string P = std::string(g_get_user_cache_dir()) + "/thumbnails/" + SIZE + "/" + md5 + ".png";
                if (g_file_test(P.c_str(), G_FILE_TEST_EXISTS)) {
                    out = P;
                    break;
                }
            }
            g_free(md5);
            return out;
        }

        // The pile's four slots, bottom first: angle (degrees), x/y offset as a
        // fraction of the cell, scale. The newest item is drawn last, on top.
        struct SSlot {
            float deg, dx, dy, scale;
        };
        constexpr SSlot PILE[4] = {{-9.F, -0.10F, 0.06F, 0.70F}, {6.F, 0.09F, 0.03F, 0.72F}, {-3.F, -0.04F, -0.02F, 0.74F}, {2.F, 0.02F, -0.05F, 0.76F}};

        const SStack* findStack(const std::string& path) {
            for (const auto& X : all())
                if (X.path == path)
                    return &X;
            return nullptr;
        }
    }

    // A thumbnail from the freedesktop cache when one EXISTS -- never a decode of
    // the original (review 2026-09-24: Hyprgraphics ignores the size for raster
    // images, so a 24 MP photo was decoded full-size on the render thread, per
    // frame, into a ~96 MB texture). Cached thumbnails are <= 256 px, and the
    // lookup (uri + md5 + two stats) is remembered per path + mtime, so the
    // render path does it once per file, not once per frame.
    SP<Render::ITexture> iconTexture(const StackModel::SEntry& e, int px, bool padded) {
        (void)px; // textures are drawn scaled; one size (the thumbnail's own) per file
        if (!e.dir && (e.kind.starts_with("image/") || e.kind.starts_with("video/") || e.kind == "application/pdf")) {
            // path|mtime -> thumbnail ("" = none yet). A "none" is asked again
            // once Thumbs has generated something (its generation moved).
            struct SMemo {
                std::string thumb;
                uint64_t    gen = 0;
            };
            static std::unordered_map<std::string, SMemo> thumbFor;
            const std::string KEY = e.path + "|" + std::to_string(e.mtime);
            auto              IT  = thumbFor.find(KEY);
            if (IT == thumbFor.end() || (IT->second.thumb.empty() && IT->second.gen != Thumbs::generation())) {
                if (thumbFor.size() > 4096)
                    thumbFor.clear();
                IT = thumbFor.insert_or_assign(KEY, SMemo{Thumbs::lookup(e.path, e.kind), Thumbs::generation()}).first;
            }
            if (!IT->second.thumb.empty())
                if (auto tex = OverviewRender::imageFile(IT->second.thumb, 256, true, padded); tex)
                    return tex;
        }
        return OverviewRender::appIcon(themedIcon(e.icon), false, padded);
    }

    // The pile's four newest files, per folder, recomputed only when the reader
    // reports a change -- not by copying and sorting the whole listing (up to
    // 5 000 entries) every frame for every cell (review 2026-09-24).
    const std::vector<StackModel::SEntry>& newestFour(const std::string& dir) {
        struct SCached {
            uint64_t                        gen = 0;
            std::vector<StackModel::SEntry> top;
        };
        static std::unordered_map<std::string, SCached> cache;
        auto&                                            C = cache[dir];
        if (C.gen != StackReader::generation() || C.gen == 0) {
            auto L = StackReader::listing(dir);
            C.top.clear();
            if (L.ready && !L.missing) {
                std::erase_if(L.entries, [](const StackModel::SEntry& e) { return e.dir; });
                StackModel::sortEntries(L.entries, StackModel::ESort::Added);
                for (size_t k = 0; k < std::min<size_t>(4, L.entries.size()); ++k) {
                    C.top.push_back(L.entries[k]);
                    C.top.back().icon = pickIcon(C.top.back().icon); // the reader hands every candidate name
                }
                C.gen = StackReader::generation();
            }
        }
        return C.top;
    }

    CBox pileSlot(const CBox& iconBox, float depth, float& rot) {
        // 0 is the top slot (PILE[3]), 3 the bottom; between two slots, between
        // their boxes and angles, so a paper settling deeper slides there.
        const float D  = std::clamp(depth, 0.F, 3.F);
        const int   LO = sc<int>(std::floor(D));
        const int   HI = std::min(3, LO + 1);
        const float F  = D - sc<float>(LO);
        const auto& A = PILE[3 - LO];
        const auto& B = PILE[3 - HI];
        const float DEG = A.deg + (B.deg - A.deg) * F, DX = A.dx + (B.dx - A.dx) * F, DY = A.dy + (B.dy - A.dy) * F, SCALE = A.scale + (B.scale - A.scale) * F;
        const float W   = sc<float>(iconBox.width) * SCALE;
        rot             = DEG * 3.14159265F / 180.F;
        return CBox{iconBox.x + iconBox.width * (0.5 + DX) - W / 2.0, iconBox.y + iconBox.height * (0.5 + DY) - W / 2.0, W, W};
    }

    std::vector<SPileItem> pileLayout(const std::string& stackPath, const CBox& iconBox) {
        std::vector<SPileItem> out;
        const auto*            S = findStack(stackPath);
        if (!S || !S->isDir || S->display != StackModel::EDisplay::Stack)
            return out;
        struct SView {
            std::vector<StackModel::SEntry> entries;
        } L{.entries = newestFour(S->path)};
        const size_t N = std::min<size_t>(4, L.entries.size());
        for (size_t k = 0; k < N; ++k) {
            const auto& E  = L.entries[N - 1 - k]; // the oldest of the four first, the newest on top
            const auto& SL = PILE[4 - N + k];
            const float W  = sc<float>(iconBox.width) * SL.scale;
            out.push_back(SPileItem{
                .entry = E,
                .box   = CBox{iconBox.x + iconBox.width * (0.5 + SL.dx) - W / 2.0, iconBox.y + iconBox.height * (0.5 + SL.dy) - W / 2.0, W, W},
                .rot   = SL.deg * 3.14159265F / 180.F,
            });
        }
        return out;
    }

    // THE DOWNLOAD BAR (2026-09-24, the macOS Downloads stack): a thin bar along
    // the bottom of the cell while a browser is downloading into the folder --
    // the browser's own progress when it publishes one (Chromium-family, over
    // the LauncherEntry protocol), else a segment sliding back and forth.
    void drawDownloadBar(PHLMONITOR monitor, const std::string& path, const CBox& iconBox, float alpha) {
        if (downloadsIn(path) == 0 || alpha <= 0.F)
            return;
        const double W     = iconBox.width;
        const double H     = std::max(3.0, std::round(W * 0.075));
        const CBox   TRACK = CBox{iconBox.x + W * 0.14, iconBox.y + iconBox.height - H - W * 0.02, W * 0.72, H}.round();
        const int    ROUND = sc<int>(std::round(H / 2.0));

        CRectPassElement::SRectData track;
        track.box   = TRACK;
        track.color = CHyprColor{0.F, 0.F, 0.F, 0.55F * alpha};
        track.round = ROUND;
        if (drawableBox(track.box))
            g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(track));

        CBox fill = TRACK;
        fill.expand(-std::max(1.0, std::round(sc<double>(monitor->m_scale))));
        if (const auto P = LauncherEntry::anyProgress()) {
            fill.width = std::max(fill.height, fill.width * *P);
        } else {
            const double MS  = sc<double>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
            const double T   = std::fmod(MS, 1600.0) / 1600.0;           // one sweep there and back
            const double TRI = T < 0.5 ? T * 2.0 : 2.0 - T * 2.0;        // 0..1..0
            const double E   = TRI * TRI * (3.0 - 2.0 * TRI);           // eased at both ends
            const double SEG = fill.width * 0.32;
            fill.x += (fill.width - SEG) * E;
            fill.width = SEG;
        }
        CRectPassElement::SRectData bar;
        bar.box   = fill.round();
        bar.color = Config::highlightColor().modifyA(alpha);
        bar.round = std::max(1, ROUND - 1);
        if (drawableBox(bar.box))
            g_pHyprRenderer->m_renderPass.add(makeUnique<CRectPassElement>(bar));
    }

    static bool drawCellBody(PHLMONITOR monitor, const Dock::SItem& item, const CBox& iconBox, float alpha, const DockTilt::SPose& pose);

    bool drawCell(PHLMONITOR monitor, const Dock::SItem& item, const CBox& iconBox, float alpha, const DockTilt::SPose& pose) {
        const bool DREW = drawCellBody(monitor, item, iconBox, alpha, pose);
        if (DREW && drawableBox(iconBox))
            drawDownloadBar(monitor, item.stackPath, iconBox, alpha);
        return DREW;
    }

    static bool drawCellBody(PHLMONITOR monitor, const Dock::SItem& item, const CBox& iconBox, float alpha, const DockTilt::SPose& pose) {
        if (!item.stackDir)
            return false;
        const auto* S = findStack(item.stackPath);
        if (!S)
            return false;
        if (!drawableBox(iconBox))
            return true; // a cell still animating in from nothing: nothing to draw yet

        const float SC  = std::max(monitor->m_scale, 0.01F);
        const auto  add = [&](const SP<Render::ITexture>& tex, CBox box, float rot, float a) {
            if (!tex || !tex->ok() || !drawableBox(box))
                return;
            // The same 3D hover card as every other dock icon (user,
            // 2026-09-24), one pose for the whole cell.
            CBox b = box.round();
            b.rot  = rot;
            DockTilt::queue(tex, b, a, pose);
        };

        const auto L = StackReader::status(S->path);
        // A FOLDER THAT IS GONE: dimmed, and the tooltip says why (tooltipSuffix).
        if (L.ready && L.missing) {
            add(OverviewRender::appIcon(themedIcon(S->icon)), iconBox, 0.F, alpha * 0.35F);
            return true;
        }
        if (S->display == StackModel::EDisplay::Folder) {
            add(OverviewRender::appIcon(themedIcon(S->icon)), iconBox, 0.F, alpha);
            return true;
        }

        // While its fan is open (or closing) the pile's items are IN the fan:
        // only an outline stays behind, so they visibly left it. A closed stack
        // is just the pile, no frame (user, 2026-09-24).
        CBox outline = iconBox;
        outline.expand(-iconBox.width * 0.06);
        if (popupShowsStack(S->path)) {
            CBorderPassElement::SBorderData border;
            border.box        = outline.round();
            border.grad1      = ::Config::CGradientValueData(Config::themeMutedText().modifyA(alpha * 0.8F));
            border.round      = sc<int>(std::round(outline.width * 0.18));
            border.borderSize = std::max(1, sc<int>(std::round(1.5F * SC)));
            if (drawableBox(border.box))
                g_pHyprRenderer->m_renderPass.add(makeUnique<CBorderPassElement>(border));
            return true;
        }

        const auto PILEITEMS = pileLayout(S->path, iconBox);
        if (PILEITEMS.empty()) {
            // EMPTY (or not read yet). Dock stacks C: hyprdrive's open tray with
            // two ghost cards in the accent, and the folder's own icon small on
            // its front, so an empty Downloads is still Downloads. With
            // builtin_icons off: the theme's folder inside the outline.
            if (Config::getValue<bool>("plugin:hyprdrive:dock:builtin_icons")) {
                CBox tray = outline;
                tray.expand(-outline.width * 0.04);
                add(OverviewRender::appIcon(std::string(BuiltinIcons::STACK_EMPTY)), tray, 0.F, alpha);
                const double S2 = tray.width * 0.30;
                add(OverviewRender::appIcon(themedIcon(S->icon)), CBox{tray.x + (tray.width - S2) / 2.0, tray.y + tray.height * 0.705 - S2 / 2.0, S2, S2}, 0.F, alpha);
                return true;
            }
            CBox inner = outline;
            inner.expand(-outline.width * 0.18);
            add(OverviewRender::appIcon(themedIcon(S->icon)), inner, 0.F, alpha * 0.85F);
            return true;
        }
        const int PX = sc<int>(std::round(iconBox.width));
        for (const auto& P : PILEITEMS)
            // Padded: the pile's papers are turned, and a hard texture edge
            // would stair-step (2026-09-24).
            if (const auto TEX = iconTexture(P.entry, PX, true))
                add(TEX, OverviewRender::padBox(P.box, TEX), P.rot, alpha);
        return true;
    }
}
