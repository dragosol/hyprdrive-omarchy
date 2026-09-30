#pragma once

// MOTION BLUR FOR THE STACK FAN (user, 2026-09-24: "add real motion blur to the
// fan animation and see how hard it impacts performance").
//
// Real, not a trail: each frame the fan is laid out at several instants across
// the time that frame covers (the shutter). Each sample is COMPOSITED on its
// own (text over its pill, as at rest) in a scratch framebuffer, then SUMMED
// into the accumulator at 1/N -- the true mean of the N instants -- and the
// mean is laid over the scene once. (Summing the pieces themselves, as the
// first version did, cannot put dark text on a light pill -- addition only
// brightens; a running "over" average left the trail too heavy: 2026-09-29.) A paper that moved far smears along its
// path; one that barely moved stays sharp. A fan at rest never comes here.

#include "globals.hpp"

#include <hyprland/src/render/Texture.hpp>
#include <hyprland/src/render/pass/PassElement.hpp>
#include <vector>

namespace ScrollOverview::StackBlur {

    struct SSample {
        SP<Render::ITexture> tex;
        CBox                 box; // render px, may carry rot
        float                alpha = 1.F; // the element's own alpha (no sample weight)
        int                  group = 0;   // which instant (sample) of the shutter it belongs to
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
