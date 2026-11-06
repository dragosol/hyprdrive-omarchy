#include "DockTilt.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#define private public
#define protected public
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Shader.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>
#include <hyprland/src/helpers/cm/ColorManagement.hpp>
#include <hyprland/src/output/Monitor.hpp>
#undef protected
#undef private
#include <hyprland/src/helpers/time/Time.hpp>
#include "Config.hpp"

namespace ScrollOverview::DockTilt {

namespace {

constexpr const char* KEY_HOVER = "plugin:hyprdrive:dock:hover_tilt";
constexpr const char* KEY_PRESS = "plugin:hyprdrive:dock:press_tilt";

constexpr float DEG = 3.14159265F / 180.F;

// The site's card is ~350px wide under perspective(1000px): the eye sits about
// three widths away. Keeping the RATIO rather than the 1000px is what makes a
// 54px icon lean like the card does -- 1000px from something that small would
// flatten the perspective to nothing.
constexpr float PERSPECTIVE = 3.0F; // eye distance, in icon sizes

constexpr float HOVER_TILT = 10.F * DEG; // the site's ±10deg at the very edge
constexpr float HOVER_LIFT = 1.03F;      // the site's scale(1.02), a touch more for a small target
constexpr float HOVER_GLARE = 0.14F;     // the site's glare is rgba(255,255,255,0.1) at its centre

// A press adds to whatever the hover already leans, toward the same point, so
// pushing an icon you are pointing at leans it further in.
constexpr float PRESS_TILT  = 9.F * DEG;
constexpr float PRESS_SCALE = 0.84F;  // with the sink below: ~0.80 of rest, the depth the squish had
constexpr float PRESS_SINK  = -0.14F; // the site's translateZ(-20px), as a share of the icon
constexpr float PRESS_GLARE = 0.30F;

constexpr float GLARE_SIGMA = 0.32F; // uv: soft enough to read as light, not a spot

// Two springs, because the site has two motions. Following the pointer is quick
// and nearly critically damped -- the card tracks the hand without swimming.
// Letting go is its 0.5s cubic-bezier(0.34, 1.56, 0.64, 1): an overshoot of a
// quarter of the lean, back through flat, settling in about half a second.
constexpr float FOLLOW_OMEGA = 26.F, FOLLOW_ZETA = 0.90F;
constexpr float LEAVE_OMEGA  = 13.F, LEAVE_ZETA = 0.40F;
constexpr float STEP_MS      = 4.F;  // fixed integration step: the same curve at any refresh rate
constexpr float MAX_GAP_MS   = 64.F; // a stalled frame is not a reason to fling the icon

struct STilt {
    Vector2D        pos, vel;   // the lean, -1..1 per axis
    float           lift = 0.F; // 0 flat, 1 fully hovered
    float           liftVel = 0.F;
    bool            hovered = false;
    Time::steady_tp last    = Time::steadyNow();
};

// KEYED BY MONITOR FIRST. See the note on pose() in the header for what the
// single flat map keyed on the icon name alone actually did: two docks sharing
// one spring, flipping its target against each other every frame and never
// settling. The outer map is tiny (one row per output) and the inner one holds
// only the icons that are, or were just, hovered on that output.
std::unordered_map<MONITORID, std::unordered_map<std::string, STilt>> g_tilts;

float msSince(const Time::steady_tp& t) {
    return std::chrono::duration<float, std::milli>(Time::steadyNow() - t).count();
}

void spring(float& x, float& v, float target, float omega, float zeta, float h) {
    const float ACC = omega * omega * (target - x) - 2.F * zeta * omega * v;
    v += ACC * h;
    x += v * h;
}

void advance(STilt& s, std::optional<Vector2D> pointer) {
    float gap = std::min(msSince(s.last), MAX_GAP_MS);
    s.last    = Time::steadyNow();
    s.hovered = pointer.has_value();

    const Vector2D TARGET = pointer.value_or(Vector2D{});
    const float    OMEGA  = s.hovered ? FOLLOW_OMEGA : LEAVE_OMEGA;
    const float    ZETA   = s.hovered ? FOLLOW_ZETA : LEAVE_ZETA;

    while (gap > 0.F) {
        const float H = std::min(gap, STEP_MS) / 1000.F;
        gap -= STEP_MS;

        float px = sc<float>(s.pos.x), py = sc<float>(s.pos.y), vx = sc<float>(s.vel.x), vy = sc<float>(s.vel.y);
        spring(px, vx, sc<float>(TARGET.x), OMEGA, ZETA, H);
        spring(py, vy, sc<float>(TARGET.y), OMEGA, ZETA, H);
        spring(s.lift, s.liftVel, s.hovered ? 1.F : 0.F, OMEGA, ZETA, H);
        s.pos = {px, py};
        s.vel = {vx, vy};
    }
}

bool settled(const STilt& s) {
    return !s.hovered && std::abs(s.pos.x) < 0.002 && std::abs(s.pos.y) < 0.002 && std::abs(s.vel.x) < 0.02 && std::abs(s.vel.y) < 0.02 && std::abs(s.lift) < 0.002 &&
        std::abs(s.liftVel) < 0.02;
}

// ------------------------------------------------------------------ shader --

// Hyprland's own tex300.vert, with one change: the projected point's third
// component is handed to the GPU as w instead of being thrown away. For every
// affine matrix Hyprland builds that component is 1 and nothing changes; for
// the homography below it is the depth, and the rasteriser divides by it --
// which is both the perspective and the perspective-correct texture lookup.
constexpr const char* VERT = R"#(#version 300 es
uniform mat3 proj;
in vec2 pos;
in vec2 texcoord;
out vec2 v_texcoord;
void main() {
    vec3 p = proj * vec3(pos, 1.0);
    gl_Position = vec4(p.xy, 0.0, p.z);
    v_texcoord = texcoord;
}
)#";

// Premultiplied in, premultiplied out, like every Hyprland texture draw. The
// glare is added in proportion to coverage, so it takes the icon's own shape
// and can never light the transparent corners around it.
constexpr const char* FRAG = R"#(#version 300 es
precision highp float;
in vec2 v_texcoord;
uniform sampler2D tex;
uniform float alpha;
uniform vec3 glare; // uv.x, uv.y, strength
uniform float glareSigma;
layout(location = 0) out vec4 fragColor;
void main() {
    vec4 c = texture(tex, v_texcoord);
    vec2 d = v_texcoord - glare.xy;
    float g = glare.z * exp(-dot(d, d) / (2.0 * glareSigma * glareSigma));
    c.rgb = min(c.rgb + vec3(g * c.a), vec3(c.a));
    fragColor = c * alpha;
}
)#";

SP<CShader> g_shader;
bool        g_shaderFailed = false;
GLint       g_locGlare = -1, g_locSigma = -1;

SP<CShader> shader() {
    if (g_shader || g_shaderFailed)
        return g_shader;

    auto s = makeShared<CShader>();
    if (!s->createProgram(VERT, FRAG, true, true)) {
        g_shaderFailed = true;
        Log::logger->log(Log::ERR, "[hyprdrive] dock tilt shader failed to build; icons stay flat");
        return nullptr;
    }

    g_locGlare = glGetUniformLocation(s->program(), "glare");
    g_locSigma = glGetUniformLocation(s->program(), "glareSigma");
    g_shader   = s;
    return g_shader;
}

// The perspective can only be drawn in the sRGB the icon already is. If this
// frame would have colour-managed the texture (HDR, an ICC profile), the
// shader cannot follow Hyprland's CM path, so the icon is drawn flat and right
// rather than tilted and in the wrong colours. Same test renderToFBInternal makes.
bool needsColorManagement(const SP<Render::ITexture>& tex) {
    const auto MONITOR = g_pHyprRenderer->m_renderData.pMonitor;
    if (!MONITOR || !Render::GL::g_pHyprOpenGL->m_cmSupported || !ScrollOverview::Config::getValue<bool>("render:cm_enabled") || MONITOR->doesNoShaderCM())
        return false;

    const auto SOURCE = tex->m_imageDescription ? tex->m_imageDescription : NColorManagement::getDefaultImageDescription();
    auto       target = g_pHyprRenderer->m_renderData.currentFB ? g_pHyprRenderer->m_renderData.currentFB->imageDescription() : nullptr;
    if (!target)
        target = MONITOR->workBufferImageDescription();

    return SOURCE->needsCM(target);
}

// The tilted plane's projection, in the icon box's own unit square.
//
// A corner (u, v) of the icon sits, centred and scaled, at
//     P = R * (s*W*(u - 1/2), s*H*(v - 1/2), 0) + (0, 0, sink)
// and the eye at distance d sees it at  centre + P.xy * d / (d - P.z).
// Written homogeneously that is linear in (u, v) -- so it is one 3x3 matrix,
// which composes in front of the box matrix Hyprland would have drawn the flat
// icon with. Every in-plane thing that matrix already carries -- the monitor's
// transform, CBox::rot's urgent wiggle -- applies on top of the tilt untouched.
Mat3x3 homography(const CBox& box, const SPose& pose) {
    const float W    = std::max(1.F, sc<float>(box.width));
    const float H    = std::max(1.F, sc<float>(box.height));
    const float SIZE = (W + H) / 2.F;
    const float D    = PERSPECTIVE * SIZE;
    const float TZ   = pose.sink * SIZE;

    const float CA = std::cos(pose.rotX), SA = std::sin(pose.rotX);
    const float CB = std::cos(pose.rotY), SB = std::sin(pose.rotY);

    // R = rotateX * rotateY (CSS order), its first two columns: where the
    // icon's x and y axes end up.
    const float AX = CB * pose.scale * W, AY = SA * SB * pose.scale * W, AZ = -CA * SB * pose.scale * W;
    const float BX = 0.F, BY = CA * pose.scale * H, BZ = SA * pose.scale * H;

    const float WU = -AZ / D, WV = -BZ / D, W0 = 1.F - (TZ - 0.5F * AZ - 0.5F * BZ) / D;

    return Mat3x3{std::array<float, 9>{
        0.5F * WU + AX / W, 0.5F * WV + BX / W, 0.5F * W0 - 0.5F * (AX + BX) / W, //
        0.5F * WU + AY / H, 0.5F * WV + BY / H, 0.5F * W0 - 0.5F * (AY + BY) / H, //
        WU,                 WV,                 W0,
    }};
}

// Where the four corners land, in render px: the damage the draw must cover.
CBox projectedBounds(const CBox& box, const SPose& pose) {
    const auto M = homography(box, pose).getMatrix();

    double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
    for (const auto& [U, V] : std::array<std::pair<float, float>, 4>{{{0.F, 0.F}, {1.F, 0.F}, {0.F, 1.F}, {1.F, 1.F}}}) {
        const float WH = std::max(1e-3F, M[6] * U + M[7] * V + M[8]);
        const float X  = (M[0] * U + M[1] * V + M[2]) / WH;
        const float Y  = (M[3] * U + M[4] * V + M[5]) / WH;
        x0             = std::min(x0, box.x + X * box.width);
        x1             = std::max(x1, box.x + X * box.width);
        y0             = std::min(y0, box.y + Y * box.height);
        y1             = std::max(y1, box.y + Y * box.height);
    }

    CBox out{x0, y0, x1 - x0, y1 - y0};

    // CBox::rot spins the result about the box's centre; the circle through the
    // farthest corner holds it at any angle.
    if (std::abs(box.rot) > 1e-4) {
        const auto   C = box.middle();
        const double R = std::max({std::hypot(x0 - C.x, y0 - C.y), std::hypot(x1 - C.x, y0 - C.y), std::hypot(x0 - C.x, y1 - C.y), std::hypot(x1 - C.x, y1 - C.y)});
        out            = CBox{C.x - R, C.y - R, R * 2.0, R * 2.0};
    }

    return out.expand(2.0).round();
}

// The flat equivalent of a pose: its scale and its sink, about the centre.
CBox flatBox(CBox box, const SPose& pose) {
    const double FACTOR = pose.scale * (PERSPECTIVE / (PERSPECTIVE - pose.sink));
    if (std::abs(FACTOR - 1.0) < 1e-4)
        return box;

    return box.scaleFromCenter(FACTOR);
}

// THE TWO SWITCHES, RE-READ ON A SHORT TIMER.
//
// pose() asks both of them, and pose() is called once per icon per monitor per
// frame -- thirty icons, two outputs, a hundred frames a second is six thousand
// config lookups a second, each of which hashes a thirty-odd character key
// string (user, 2026-09-14). Nothing here can see a config reload, so the
// answer is cached with a freshness stamp instead, exactly the way the
// favourites, the locations and the stacks stores are. A quarter of a second is
// far under any hand's reaction time to ticking a box in the panel, and neither
// switch is animated.
struct SSwitches {
    bool            hover  = false;
    bool            press  = false;
    bool            loaded = false;
    Time::steady_tp readAt{};
};

SSwitches& switches() {
    static SSwitches s;

    constexpr float POLL_MS = 250.F;
    const auto      NOW     = Time::steadyNow();
    if (s.loaded && std::chrono::duration<float, std::milli>(NOW - s.readAt).count() < POLL_MS)
        return s;

    s.loaded = true;
    s.readAt = NOW;
    s.hover  = ScrollOverview::Config::getValue<bool>(KEY_HOVER);
    s.press  = ScrollOverview::Config::getValue<bool>(KEY_PRESS);
    return s;
}

} // namespace

bool SPose::flat() const {
    return std::abs(rotX) < 1e-4F && std::abs(rotY) < 1e-4F && glare < 1e-3F;
}

bool hoverEnabled() {
    return switches().hover;
}

bool pressEnabled() {
    return switches().press;
}

SPose pose(MONITORID monitor, const std::string& subject, std::optional<Vector2D> pointer, float pressDepth, const Vector2D& pressAt) {
    SPose out;

    if (!hoverEnabled())
        pointer.reset();

    // Only an icon that is, or was just, being hovered owns spring state; the
    // other thirty in the row answer from here without allocating anything --
    // and an output that has never had a hover owns no row at all.
    auto MON = g_tilts.find(monitor);
    if (pointer && MON == g_tilts.end())
        MON = g_tilts.emplace(monitor, std::unordered_map<std::string, STilt>{}).first;

    Vector2D lean;
    if (MON != g_tilts.end()) {
        auto& ICONS = MON->second;

        auto  IT    = ICONS.find(subject);
        if (pointer && IT == ICONS.end())
            IT = ICONS.emplace(subject, STilt{}).first;

        if (IT != ICONS.end()) {
            auto& S = IT->second;
            advance(S, pointer);
            lean      = S.pos;
            out.scale = 1.F + (HOVER_LIFT - 1.F) * S.lift;
            out.glare = HOVER_GLARE * std::clamp(S.lift, 0.F, 1.F);
        }
    }

    out.rotX = -sc<float>(lean.y) * HOVER_TILT;
    out.rotY = sc<float>(lean.x) * HOVER_TILT;

    // The squish itself is not optional -- it predates both tilts, and with
    // them off it is still the answer to a click. The negative depth of the
    // rebound grows the icon a touch past rest, which is the bounce.
    out.scale *= 1.F - pressDepth * (1.F - PRESS_SCALE);
    out.sink = PRESS_SINK * pressDepth;

    Vector2D glareFrom = lean;
    if (pressEnabled() && std::abs(pressDepth) > 1e-4F) {
        out.rotX -= sc<float>(pressAt.y) * PRESS_TILT * pressDepth;
        out.rotY += sc<float>(pressAt.x) * PRESS_TILT * pressDepth;
        out.glare += PRESS_GLARE * std::clamp(std::abs(pressDepth), 0.F, 1.F);

        // The light belongs where the finger is. Blend toward the press point
        // by how much of the glare the press is responsible for.
        const float SHARE = out.glare > 1e-4F ? std::clamp(PRESS_GLARE * std::abs(pressDepth) / out.glare, 0.F, 1.F) : 0.F;
        glareFrom         = lean + (pressAt - lean) * SHARE;
    }

    out.glareAt = Vector2D{0.5 + glareFrom.x * 0.5, 0.5 + glareFrom.y * 0.5};
    return out;
}

bool animating(MONITORID monitor) {
    const auto MON = g_tilts.find(monitor);
    if (MON == g_tilts.end())
        return false;

    auto& ICONS  = MON->second;
    bool  moving = false;
    for (auto it = ICONS.begin(); it != ICONS.end();) {
        // An icon that left the dock mid-spring is never advanced again; its
        // entry would otherwise ask for frames forever.
        if (msSince(it->second.last) > 1000.F || settled(it->second)) {
            it = ICONS.erase(it);
            continue;
        }

        // A pointer resting on an icon has a spring at rest at its target:
        // nothing moves, nothing needs a frame. Motion re-arms it through
        // the dock's visual state.
        const auto& S = it->second;
        if (!S.hovered || std::abs(S.vel.x) > 0.02 || std::abs(S.vel.y) > 0.02 || std::abs(S.liftVel) > 0.02)
            moving = true;
        ++it;
    }

    if (ICONS.empty())
        g_tilts.erase(MON);

    return moving;
}

void forget(MONITORID monitor) {
    g_tilts.erase(monitor);
}

void queue(SP<Render::ITexture> tex, const CBox& box, float alpha, const SPose& pose) {
    if (!tex)
        return;

    if (pose.flat()) {
        CTexPassElement::SRenderData data;
        data.tex = tex;
        data.box = flatBox(box, pose);
        data.a   = alpha;
        g_pHyprRenderer->m_renderPass.add(makeUnique<CTexPassElement>(data));
        return;
    }

    g_pHyprRenderer->m_renderPass.add(makeUnique<CDockTiltPassElement>(CDockTiltPassElement::SData{.tex = tex, .box = box, .alpha = alpha, .pose = pose}));
}

bool drawAdditive(const SP<Render::ITexture>& tex, const CBox& boxIn, float alpha, bool sum, const CBox* scissorBox) {
    if (!tex || !tex->ok() || boxIn.width < 1 || boxIn.height < 1 || alpha <= 0.F || !g_pHyprRenderer->m_renderData.pMonitor)
        return false;
    const auto PROGRAM = tex->m_type == Render::TEXTURE_RGBA && tex->m_transform == HYPRUTILS_TRANSFORM_NORMAL && !needsColorManagement(tex) ? shader() : nullptr;
    if (!PROGRAM)
        return false;

    CBox box = boxIn;
    g_pHyprRenderer->m_renderData.renderModif.applyToBox(box);
    const auto MATRIX = g_pHyprRenderer->projectBoxToTarget(box, tex->m_transform);

    Render::GL::g_pHyprOpenGL->blend(true);
    if (sum)
        glBlendFunc(GL_ONE, GL_ONE); // summed, not layered
    else
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); // premultiplied over

    glActiveTexture(GL_TEXTURE0);
    tex->bind();
    tex->setTexParameter(GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    tex->setTexParameter(GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    tex->setTexParameter(GL_TEXTURE_MAG_FILTER, tex->magFilter);
    tex->setTexParameter(GL_TEXTURE_MIN_FILTER, tex->minFilter);

    const auto S = Render::GL::g_pHyprOpenGL->useShader(PROGRAM);
    S->setUniformMatrix3fv(SHADER_PROJ, 1, GL_TRUE, MATRIX.getMatrix());
    S->setUniformInt(SHADER_TEX, 0);
    S->setUniformFloat(SHADER_ALPHA, std::clamp(alpha, 0.F, 1.F));
    glUniform3f(g_locGlare, 0.5F, 0.5F, 0.F);
    glUniform1f(g_locSigma, GLARE_SIGMA);

    glBindVertexArray(S->getUniformLocation(SHADER_SHADER_VAO));
    if (scissorBox)
        Render::GL::g_pHyprOpenGL->scissor(*scissorBox);
    else
        Render::GL::g_pHyprOpenGL->scissor(nullptr);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    tex->unbind();

    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); // Hyprland's premultiplied default
    return true;
}

void shutdown() {
    g_tilts.clear();
    if (g_shader && Render::GL::g_pHyprOpenGL) {
        Render::GL::g_pHyprOpenGL->makeEGLCurrent();
        g_shader.reset();
    }
    g_shaderFailed = false;
}

CDockTiltPassElement::CDockTiltPassElement(const SData& data_) : data(data_) {
    ;
}

std::vector<UP<IPassElement>> CDockTiltPassElement::draw() {
    const auto& TEX = data.tex;
    if (!TEX || !TEX->ok() || data.box.width < 1 || data.box.height < 1 || data.alpha <= 0.F || !g_pHyprRenderer->m_renderData.pMonitor)
        return {};

    CBox box = data.box;
    g_pHyprRenderer->m_renderData.renderModif.applyToBox(box);

    auto transform = TEX->m_transform;
    if (g_pHyprRenderer->monitorTransformEnabled())
        transform = Math::composeTransform(Math::wlTransformToHyprutils(Math::invertTransform(g_pHyprRenderer->m_renderData.pMonitor->m_transform)), transform);

    const auto PROGRAM = TEX->m_type == Render::TEXTURE_RGBA && transform == HYPRUTILS_TRANSFORM_NORMAL && !needsColorManagement(TEX) ? shader() : nullptr;

    CRegion damage{g_pHyprRenderer->m_renderData.damage};
    damage.intersect(projectedBounds(box, data.pose));
    if (!g_pHyprRenderer->m_renderData.clipBox.empty())
        damage.intersect(g_pHyprRenderer->m_renderData.clipBox);
    if (damage.empty())
        return {};

    // Anything the shader cannot serve is still an icon: flat, at the pose's size.
    if (!PROGRAM) {
        Render::GL::g_pHyprOpenGL->renderTexture(TEX, flatBox(data.box, data.pose), {.damage = &damage, .a = data.alpha});
        return {};
    }

    const auto MATRIX = g_pHyprRenderer->projectBoxToTarget(box, transform).copy().multiply(homography(box, data.pose));

    Render::GL::g_pHyprOpenGL->blend(true);

    glActiveTexture(GL_TEXTURE0);
    TEX->bind();
    TEX->setTexParameter(GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    TEX->setTexParameter(GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    TEX->setTexParameter(GL_TEXTURE_MAG_FILTER, TEX->magFilter);
    TEX->setTexParameter(GL_TEXTURE_MIN_FILTER, TEX->minFilter);

    const auto S = Render::GL::g_pHyprOpenGL->useShader(PROGRAM);
    S->setUniformMatrix3fv(SHADER_PROJ, 1, GL_TRUE, MATRIX.getMatrix());
    S->setUniformInt(SHADER_TEX, 0);
    S->setUniformFloat(SHADER_ALPHA, std::clamp(data.alpha, 0.F, 1.F));
    glUniform3f(g_locGlare, sc<float>(data.pose.glareAt.x), sc<float>(data.pose.glareAt.y), std::clamp(data.pose.glare, 0.F, 1.F));
    glUniform1f(g_locSigma, GLARE_SIGMA);

    glBindVertexArray(S->getUniformLocation(SHADER_SHADER_VAO));
    damage.forEachRect([](const auto& RECT) {
        Render::GL::g_pHyprOpenGL->scissor(&RECT, g_pHyprRenderer->m_renderData.transformDamage);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    });

    Render::GL::g_pHyprOpenGL->scissor(nullptr);
    glBindVertexArray(0);
    TEX->unbind();
    return {};
}

bool CDockTiltPassElement::needsLiveBlur() {
    return false;
}

bool CDockTiltPassElement::needsPrecomputeBlur() {
    return false;
}

std::optional<CBox> CDockTiltPassElement::boundingBox() {
    const auto MONITOR = g_pHyprRenderer->m_renderData.pMonitor;
    if (!MONITOR)
        return std::nullopt;

    return projectedBounds(data.box, data.pose).scale(1.F / MONITOR->m_scale).round();
}

CRegion CDockTiltPassElement::opaqueRegion() {
    return {};
}

} // namespace ScrollOverview::DockTilt
