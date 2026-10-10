#pragma once
#include "globals.hpp"
#include <hyprland/src/config/shared/complex/ComplexDataTypes.hpp>
#include <hyprland/src/render/pass/Pass.hpp>
#include <hyprland/src/render/pass/PassElement.hpp>

class COverviewBlurPassElement : public IPassElement {
  public:
    struct SData {
        CBox  box;
        int   rounding      = 0;
        float roundingPower = 2.F;
        float alpha         = 1.F;
        bool  xray          = false;
        // Sample Hyprland's own cached background blur (Monitor m_blurFB, what a
        // tiled window's new_optimizations blur reads) instead of blurring
        // live. FlightBlur's moving windows use it so a window frosts the same
        // moving as standing still.
        bool  hyprlandCache = false;

        // X29. The dock's frost is the reference's, not the desktop's. The blur
        // runs inside draw(), long after whoever queued this element returned, so
        // the override has to travel WITH it -- setting the config around
        // queueBlur would restore it before a single pixel had been blurred.
        // Negative means "leave the global value alone".
        int   blurSize       = -1;
        int   blurPasses     = -1;
        float blurBrightness = -1.F;
        float blurNoise      = -1.F;
        float blurContrast   = -1.F;
        float blurVibrancy   = -1.F;
    };

    COverviewBlurPassElement(const SData& data_);
    ~COverviewBlurPassElement() override = default;

    std::vector<UP<IPassElement>> draw() override;
    ePassElementType              type() override {
        return EK_CUSTOM;
    }
    bool                          needsLiveBlur() override;
    bool                          needsPrecomputeBlur() override;
    std::optional<CBox>           boundingBox() override;
    CRegion                       opaqueRegion() override;

    const char*                   passName() override {
        return "COverviewBlurPassElement";
    }

  private:
    SData data;
};

// X3 bug (user, 2026-08-02): the strip's cells round their OUTER corners large
// and every facing corner tight, but CBorderPassElement carries one radius, so
// the focused-desktop outline traced a shape the cell underneath does not have.
// The fill already solves this by drawing the same box twice with the two radii
// and clipping to opposite halves; a border has no clipBox, so the halves are
// separated by intersecting the damage instead -- the idiom the shadow element
// below already uses. Equal radii render in a single pass.
class COverviewSplitBorderPassElement : public IPassElement {
  public:
    struct SData {
        CBox                       box;
        Config::CGradientValueData color;
        int                        roundLeft     = 0;
        int                        roundRight    = 0;
        float                      roundingPower = 2.F;
        int                        borderSize    = 1;
        float                      a             = 1.F;
    };

    COverviewSplitBorderPassElement(const SData& data_);
    ~COverviewSplitBorderPassElement() override = default;

    std::vector<UP<IPassElement>> draw() override;
    ePassElementType              type() override {
        return EK_CUSTOM;
    }
    bool                          needsLiveBlur() override;
    bool                          needsPrecomputeBlur() override;
    std::optional<CBox>           boundingBox() override;
    CRegion                       opaqueRegion() override;

    const char*                   passName() override {
        return "COverviewSplitBorderPassElement";
    }

  private:
    SData data;
};

class COverviewShadowPassElement : public IPassElement {
  public:
    struct SData {
        PHLMONITORREF monitor;
        CBox          fullBox;
        CBox          cutoutBox;
        int           rounding      = 0;
        float         roundingPower = 2.F;
        int           range         = 0;
        int           renderPower   = 0;
        Config::CGradientValueData color;
        float         alpha        = 1.F;
        bool          ignoreWindow = true;
    };

    COverviewShadowPassElement(const SData& data_);
    ~COverviewShadowPassElement() override = default;

    std::vector<UP<IPassElement>> draw() override;
    ePassElementType              type() override {
        return EK_CUSTOM;
    }
    bool                          needsLiveBlur() override;
    bool                          needsPrecomputeBlur() override;
    std::optional<CBox>           boundingBox() override;
    CRegion                       opaqueRegion() override;

    const char*                   passName() override {
        return "COverviewShadowPassElement";
    }

  private:
    SData data;
};

// Dock hover thumbnails at RENDER_POST_WINDOWS share Hyprland's pass. A second
// CSurfacePassElement for the same window keeps the REAL bounding box (scale is
// a draw-time hint), so pass simplify treats it as occluding the desktop copy
// and the live window vanishes. This wrapper's box is the cell, it does not
// occlude, and it draws the nested pass only when the real window is already on
// the framebuffer.
class CDockLivePreviewPassElement : public IPassElement {
  public:
    struct SData {
        UP<Render::CRenderPass> pass;
        CBox                    box;
    };

    CDockLivePreviewPassElement(SData&& data_);
    ~CDockLivePreviewPassElement() override = default;

    std::vector<UP<IPassElement>> draw() override;
    ePassElementType              type() override {
        return EK_CUSTOM;
    }
    bool                          needsLiveBlur() override;
    bool                          needsPrecomputeBlur() override;
    std::optional<CBox>           boundingBox() override;
    CRegion                       opaqueRegion() override;
    bool                          disableSimplification() override;
    bool                          undiscardable() override;

    const char*                   passName() override {
        return "CDockLivePreviewPassElement";
    }

  private:
    SData data;
};
