#pragma once

// MOTION BLUR FOR THE STACK FAN (user, 2026-09-24: "add real motion blur to the
// fan animation and see how hard it impacts performance").
//
// Real, not a trail: each frame the fan is laid out at several instants across
// the time that frame covers (the shutter), every sample is SUMMED into an
// offscreen framebuffer at 1/N weight (additive blending -- plain alpha
// layering would leave a slow paper see-through), and the average is
// composited over the scene once. A paper that moved far smears along its
// path; one that barely moved stays sharp. A fan at rest never comes here.

#include "globals.hpp"

#include <hyprland/src/render/Texture.hpp>
#include <hyprland/src/render/pass/PassElement.hpp>
#include <vector>

namespace ScrollOverview::StackBlur {

    struct SSample {
        SP<Render::ITexture> tex;
        CBox                 box; // render px, may carry rot
        float                alpha = 1.F; // already divided by the sample count
    };

    // Queue one pass element that sums `samples` and composites the result.
    // `bounds`: the render-px box everything lands in (the composite's damage).
    void queue(std::vector<SSample> samples, const CBox& bounds);

    // Hyprland's transform on this monitor rules the offscreen route out.
    bool usable(PHLMONITOR monitor);

    // PLUGIN_EXIT: the framebuffer is GL state owned by this object.
    void shutdown();

    class CStackBlurPassElement : public IPassElement {
      public:
        CStackBlurPassElement(std::vector<SSample> samples, const CBox& bounds);
        ~CStackBlurPassElement() override = default;

        std::vector<UP<IPassElement>> draw() override;
        ePassElementType              type() override {
            return EK_CUSTOM;
        }
        bool needsLiveBlur() override {
            return false;
        }
        bool needsPrecomputeBlur() override {
            return false;
        }
        std::optional<CBox> boundingBox() override;
        CRegion             opaqueRegion() override {
            return {};
        }
        const char* passName() override {
            return "CStackBlurPassElement";
        }

      private:
        std::vector<SSample> m_samples;
        CBox                 m_bounds;
    };
}
