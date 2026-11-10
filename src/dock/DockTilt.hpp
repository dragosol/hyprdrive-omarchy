#pragma once

#include "globals.hpp"

#include <hyprland/src/render/pass/PassElement.hpp>
#include <hyprland/src/render/Texture.hpp>

#include <string>

// THE DOCK ICON'S 3D TILT (user, 2026-09-14): "a 3d tilt like on the cards on
// dragos.shastasia.com". That site's card is
//
//     hover:   perspective(1000px) rotateX(-dy * 10deg) rotateY(dx * 10deg) scale(1.02)
//     leave:   back to flat over 0.5s on cubic-bezier(0.34, 1.56, 0.64, 1)
//     press:   scale(0.95) translateZ(-20px)
//     glare:   a soft white radial highlight that sits under the pointer
//
// with dx/dy the pointer's offset from the card's centre, -1..1 per axis. The
// side under the pointer RECEDES: the card leans in toward where you touch it.
//
// Hyprland's pass elements only draw flat rectangles (a CBox rotates in-plane
// and nothing more), so a real perspective tilt cannot be built from them --
// the earlier press "tilt" was CBox::rot, a 2D spin, and it read as exactly
// that. This draws the icon through a shader of its own: the tilted plane's
// projection is a homography, and a homography is a 3x3 matrix, so it rides in
// the same `proj` uniform every Hyprland texture draw uses. The vertex shader
// hands the matrix's third row to the GPU as w, which makes the texture mapping
// perspective-correct for free.
//
// Two independent halves, each with its own switch in the panel:
//   dock:hover_tilt  the site's hover lean, following the pointer on springs
//   dock:press_tilt  a press leans further in toward the press point, on the
//                    press squish's own curve (rebound included)
// With both off the press is the flat squish it was before either existed.
namespace ScrollOverview::DockTilt {

// Everything that places one icon in 3D for one frame. Angles in radians about
// the icon's centre, CSS conventions: rotY > 0 sends the RIGHT edge away,
// rotX > 0 sends the TOP edge away.
struct SPose {
    float    rotX  = 0.F;
    float    rotY  = 0.F;
    float    scale = 1.F;
    float    sink  = 0.F;         // translateZ, as a share of the icon's size (negative = away)
    Vector2D glareAt{0.5, 0.5};   // texture uv
    float    glare = 0.F;         // peak added whiteness, 0..1

    bool     flat() const;
};

// Advance `subject`'s springs to now and return its pose.
//   monitor    WHOSE icon this is. The spring state is per (monitor, subject)
//              and not per subject, and that is a bug fix rather than
//              tidiness: both docks draw the same icon every frame, so with one
//              shared entry the monitor WITHOUT the pointer advanced it to
//              hovered=false and the monitor WITH the pointer advanced it back
//              to hovered=true, sixty to a hundred times a second each. The
//              target flipped every frame, the spring never settled, and
//              animating() stayed true -- so a pointer merely RESTING on a dock
//              icon held both outputs at full refresh forever (user,
//              2026-09-14).
//   pointer    the pointer, -1..1 per axis from the icon's centre, when it is
//              over this icon and the hover tilt is on; nullopt otherwise
//   pressDepth Dock's press curve: 1 held, 0 rest, negative in the rebound
//   pressAt    where it was pressed, -1..1 per axis
SPose pose(MONITORID monitor, const std::string& subject, std::optional<Vector2D> pointer, float pressDepth, const Vector2D& pressAt);

// Is any icon on THIS monitor's dock still moving? For Dock::needsFrames, which
// is asked per monitor and must not pump one output for the other's springs.
bool animating(MONITORID monitor);

// An output that has gone away owns no springs. Called from Dock::forgetMonitor
// beside the other per-monitor rows.
void forget(MONITORID monitor);

bool hoverEnabled();
bool pressEnabled();

// Queue `tex` drawn into `box` (render px, may carry CBox::rot) with `pose`.
// A flat pose -- or a frame the shader cannot serve -- is an ordinary texture.
void queue(SP<Render::ITexture> tex, const CBox& box, float alpha, const SPose& pose);

// Draw `tex` flat into `box` (render px, may carry rot) with ADDITIVE blending
// (GL_ONE, GL_ONE) into whatever framebuffer is bound -- the stack fan's motion
// blur sums its sub-frame samples this way (StackBlur). Only from inside a
// pass element's draw(). False when this texture cannot go through the shader.
// `sum`: GL_ONE,GL_ONE (pure addition); false: premultiplied OVER, for layers
// that must cover what is under them (dark text on a light pill).
bool drawAdditive(const SP<Render::ITexture>& tex, const CBox& box, float alpha, bool sum = true, const CBox* scissorBox = nullptr);

// PLUGIN_EXIT: the shader program belongs to this object's GL state.
void shutdown();

class CDockTiltPassElement : public IPassElement {
  public:
    struct SData {
        SP<Render::ITexture> tex;
        CBox                 box;
        float                alpha = 1.F;
        SPose                pose;
    };

    CDockTiltPassElement(const SData& data_);
    ~CDockTiltPassElement() override = default;

    std::vector<UP<IPassElement>> draw() override;
    ePassElementType              type() override {
        return EK_CUSTOM;
    }
    bool                needsLiveBlur() override;
    bool                needsPrecomputeBlur() override;
    std::optional<CBox> boundingBox() override;
    CRegion             opaqueRegion() override;

    const char*         passName() override {
        return "CDockTiltPassElement";
    }

  private:
    SData data;
};

} // namespace ScrollOverview::DockTilt
