// THE STACK'S DOCK CELL (dock stacks A): the 4 newest items as a slightly
// messy pile inside a thin outline -- or the folder's icon (Display as Folder),
// a dimmed icon for a folder that is gone, the folder inside the outline for an
// empty one. Everything here draws from cached textures; the listing comes from
// StackReader, never from the disk on this thread.
#include "DockStacks.hpp"
#include "Config.hpp"
#include "OverviewRender.hpp"
#include "StackReader.hpp"

#include <gio/gio.h>

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

    SP<Render::ITexture> iconTexture(const StackModel::SEntry& e, int px) {
        if (!e.dir && (e.kind.starts_with("image/") || e.kind.starts_with("video/"))) {
            if (const auto T = cachedThumbnail(e.path); !T.empty())
                if (auto tex = OverviewRender::imageFile(T, px); tex)
                    return tex;
            // An image with no cached thumbnail is its own best picture.
            if (e.kind.starts_with("image/") && e.kind != "image/svg+xml")
                if (auto tex = OverviewRender::imageFile(e.path, px); tex)
                    return tex;
        }
        return OverviewRender::appIcon(e.icon);
    }

    std::vector<SPileItem> pileLayout(const std::string& stackPath, const CBox& iconBox) {
        std::vector<SPileItem> out;
        const auto*            S = findStack(stackPath);
        if (!S || !S->isDir || S->display != StackModel::EDisplay::Stack)
            return out;
        auto L = StackReader::listing(S->path);
        if (!L.ready || L.missing)
            return out;
        std::erase_if(L.entries, [](const StackModel::SEntry& e) { return e.dir; });
        StackModel::sortEntries(L.entries, StackModel::ESort::Added);
        const size_t N = std::min<size_t>(4, L.entries.size());
        for (size_t k = 0; k < N; ++k)
            L.entries[k].icon = pickIcon(L.entries[k].icon); // the reader hands every candidate name
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

    bool drawCell(PHLMONITOR monitor, const Dock::SItem& item, const CBox& iconBox, float alpha) {
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
            CTexPassElement::SRenderData d;
            d.tex     = tex;
            d.box     = box.round();
            d.box.rot = rot;
            d.a       = a;
            g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(d));
        };

        const auto L = StackReader::listing(S->path);
        // A FOLDER THAT IS GONE: dimmed, and the tooltip says why (tooltipSuffix).
        if (L.ready && L.missing) {
            add(OverviewRender::appIcon(S->icon), iconBox, 0.F, alpha * 0.35F);
            return true;
        }
        if (S->display == StackModel::EDisplay::Folder) {
            add(OverviewRender::appIcon(S->icon), iconBox, 0.F, alpha);
            return true;
        }

        // The outline: a thin rounded box in the theme's muted colour.
        CBox outline = iconBox;
        outline.expand(-iconBox.width * 0.06);
        CBorderPassElement::SBorderData border;
        border.box        = outline.round();
        border.grad1      = ::Config::CGradientValueData(Config::themeMutedText().modifyA(alpha * 0.8F));
        border.round      = sc<int>(std::round(outline.width * 0.18));
        border.borderSize = std::max(1, sc<int>(std::round(1.5F * SC)));
        if (drawableBox(border.box))
            g_pHyprRenderer->m_renderPass.add(makeUnique<CBorderPassElement>(border));

        // While its fan is open (or closing) the pile's items are IN the fan:
        // only the outline stays behind, so they visibly left it.
        if (popupShowsStack(S->path))
            return true;

        const auto PILEITEMS = pileLayout(S->path, iconBox);
        if (PILEITEMS.empty()) {
            // EMPTY (or not read yet): the folder inside the outline, until
            // sub-project C draws its own empty-stack artwork.
            CBox inner = outline;
            inner.expand(-outline.width * 0.18);
            add(OverviewRender::appIcon(S->icon), inner, 0.F, alpha * 0.85F);
            return true;
        }
        const int PX = sc<int>(std::round(iconBox.width));
        for (const auto& P : PILEITEMS)
            add(iconTexture(P.entry, PX), P.box, P.rot, alpha);
        return true;
    }
}
