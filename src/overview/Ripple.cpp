#include "Ripple.hpp"

#include "Config.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/helpers/time/Time.hpp>
#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/render/Framebuffer.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/Shader.hpp>
#include <hyprland/src/render/pass/PassElement.hpp>
#include <hyprland/src/state/MonitorState.hpp>

namespace ScrollOverview::Ripple {

    namespace {
        // The film's jump ring (edge.ts, WAVE): speed, wavenumber, envelope and
        // fade, in logical px. Its amplitude knob was 2.6 ("~8 px of bend at
        // 160 px out" on the 1080p canvas).
        constexpr float CJ      = 640.F;
        constexpr float SJ      = 16.F;
        // The film's 0.55 s, shortened (user, 2026-10-13: "the wave should fade
        // a little sooner", then "make it last 1 second").
        constexpr float FADE_S  = 0.28F;
        constexpr float AMP     = 2.6F;
        constexpr float LIFE_S  = 1.0F; // exp(-1.0 / 0.28) ~ 0.028: gone
        constexpr float BAND    = 3.F * SJ + 8.F; // the wavelet's reach either side of its front, + wobble
        constexpr float MAXBEND = 32.F;            // logical px a sample can be pulled from: the copy covers it

        struct SRipple {
            MONITORID       monitor = MONITOR_INVALID;
            Vector2D        originLocal; // monitor-local render px
            Time::steady_tp born;
        };
        std::vector<SRipple> g_ripples;

        CHyprSignalListener      g_render;
        SP<CShader>              g_shader;
        bool                     g_shaderFailed = false;
        GLint                    g_locMode = -1, g_locRes = -1, g_locOrigin = -1, g_locAge = -1, g_locScale = -1, g_locAmp = -1, g_locFade = -1;
        SP<Render::IFramebuffer> g_copy;

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

        // mode 0: a straight copy (into the ripple's own buffer). mode 1: the
        // jump ring, refracting that copy back onto the frame. Everything in
        // logical px; uScale turns it into the frame's render px.
        constexpr const char* FRAG = R"#(#version 300 es
precision highp float;
in vec2 v_texcoord;
uniform sampler2D tex;
uniform int   uMode;
uniform vec2  uRes;
uniform vec2  uOrigin;
uniform float uAge;
uniform float uScale;
uniform float uAmp;
uniform float uFade;
layout(location = 0) out vec4 fragColor;
float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
float vnoise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash(i), hash(i + vec2(1.0, 0.0)), u.x), mix(hash(i + vec2(0.0, 1.0)), hash(i + vec2(1.0, 1.0)), u.x), u.y) * 2.0 - 1.0;
}
void main() {
    if (uMode == 0) { fragColor = texture(tex, v_texcoord); return; }
    vec2  rel = (v_texcoord * uRes - uOrigin) / uScale;
    float dd  = length(rel);
    vec2  n   = dd > 0.001 ? rel / dd : vec2(0.0);
    const float CJ = 640.0, KJ = 6.2831853 / 30.0, SJ = 16.0, WARPGAIN = 20.0;
    // the front is never a perfect circle
    float jit = 2.5 * vnoise(vec2(atan(rel.y, rel.x) * 18.0, uAge * 0.8));
    float x   = dd + jit * 2.0 - 4.0 - CJ * uAge;
    if (abs(x) > 3.0 * SJ) discard;
    float env = exp(-x * x / (SJ * SJ));
    float a   = uAmp * exp(-uAge / uFade) * smoothstep(0.0, 0.05, uAge) / sqrt(1.0 + dd / 260.0) * smoothstep(0.0, 4.0, dd);
    float cs  = cos(KJ * x), sn = sin(KJ * x);
    float hd  = a * (-KJ * sn - 2.0 * x / (SJ * SJ) * cs) * env;
    float hdd = a * (-KJ * KJ * cs) * env;
    vec2  disp = n * hd * WARPGAIN * uScale / uRes;   // uv
    vec2  dc   = disp * 0.35;                         // the wet-glass RGB split
    vec4  g    = texture(tex, v_texcoord - disp);
    float r    = texture(tex, v_texcoord - disp - dc).r;
    float b    = texture(tex, v_texcoord - disp + dc).b;
    float focus = clamp(-hdd * 5.0, -1.0, 1.0);       // converging fronts brighten, diverging dim
    fragColor = vec4(vec3(r, g.g, b) * (1.0 + 0.06 * focus), g.a);
}
)#";

        SP<CShader> shader() {
            if (g_shader || g_shaderFailed)
                return g_shader;
            auto s = makeShared<CShader>();
            if (!s->createProgram(VERT, FRAG, true, true)) {
                g_shaderFailed = true;
                Log::logger->log(Log::ERR, "[hyprdrive] ripple shader failed to build; the hot corner has no ripple");
                return nullptr;
            }
            g_locMode   = glGetUniformLocation(s->program(), "uMode");
            g_locRes    = glGetUniformLocation(s->program(), "uRes");
            g_locOrigin = glGetUniformLocation(s->program(), "uOrigin");
            g_locAge    = glGetUniformLocation(s->program(), "uAge");
            g_locScale  = glGetUniformLocation(s->program(), "uScale");
            g_locAmp    = glGetUniformLocation(s->program(), "uAmp");
            g_locFade   = glGetUniformLocation(s->program(), "uFade");
            g_shader    = s;
            return g_shader;
        }

        float ageOf(const SRipple& r) {
            return std::chrono::duration<float>(Time::steadyNow() - r.born).count();
        }

        bool finiteBox(const CBox& b) {
            return std::isfinite(b.x) && std::isfinite(b.y) && std::isfinite(b.w) && std::isfinite(b.h) && b.w >= 1 && b.h >= 1;
        }

        // The ring's band this frame, plus how far a sample can be pulled from,
        // clipped to the output: the only pixels copied and redrawn.
        CBox bandBox(const SRipple& r, const Vector2D& res, float scale) {
            const double RADIUS = (CJ * ageOf(r) + BAND + MAXBEND) * scale;
            CBox box{r.originLocal.x - RADIUS, r.originLocal.y - RADIUS, RADIUS * 2.0, RADIUS * 2.0};
            return box.intersection(CBox{0, 0, res.x, res.y});
        }

        // One full-output quad through the shader, scissored to `clip`.
        void drawQuad(const SP<CShader>& program, const SP<Render::ITexture>& tex, const CBox& full, const CRegion& clip, int mode, const SRipple& r, float scale,
                      const Vector2D& res) {
            auto transform = tex->m_transform;
            if (g_pHyprRenderer->monitorTransformEnabled())
                transform = Math::composeTransform(Math::wlTransformToHyprutils(Math::invertTransform(g_pHyprRenderer->m_renderData.pMonitor->m_transform)), transform);
            const auto MATRIX = g_pHyprRenderer->projectBoxToTarget(full, transform);

            glActiveTexture(GL_TEXTURE0);
            tex->bind();
            tex->setTexParameter(GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            tex->setTexParameter(GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            tex->setTexParameter(GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            tex->setTexParameter(GL_TEXTURE_MIN_FILTER, GL_LINEAR);

            const auto S = Render::GL::g_pHyprOpenGL->useShader(program);
            S->setUniformMatrix3fv(SHADER_PROJ, 1, GL_TRUE, MATRIX.getMatrix());
            S->setUniformInt(SHADER_TEX, 0);
            glUniform1i(g_locMode, mode);
            glUniform2f(g_locRes, sc<float>(res.x), sc<float>(res.y));
            glUniform2f(g_locOrigin, sc<float>(r.originLocal.x), sc<float>(r.originLocal.y));
            glUniform1f(g_locAge, ageOf(r));
            glUniform1f(g_locScale, scale);
            glUniform1f(g_locAmp, AMP);
            glUniform1f(g_locFade, FADE_S);

            glBindVertexArray(S->getUniformLocation(SHADER_SHADER_VAO));
            clip.forEachRect([](const auto& RECT) {
                Render::GL::g_pHyprOpenGL->scissor(&RECT, g_pHyprRenderer->m_renderData.transformDamage);
                glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            });
            Render::GL::g_pHyprOpenGL->scissor(nullptr);
            glBindVertexArray(0);
            tex->unbind();
        }

        class CRipplePassElement : public IPassElement {
          public:
            explicit CRipplePassElement(const SRipple& r) : m_ripple(r) {}

            std::vector<UP<IPassElement>> draw() override {
                const auto MONITOR = g_pHyprRenderer->m_renderData.pMonitor.lock();
                const auto CUR     = g_pHyprRenderer->m_renderData.currentFB;
                const auto PROGRAM = shader();
                if (!MONITOR || !CUR || !PROGRAM)
                    return {};
                const auto CURTEX = CUR->getTexture();
                if (!CURTEX || !CURTEX->ok())
                    return {};

                const Vector2D RES   = MONITOR->m_pixelSize;
                const float    SCALE = std::max(0.25F, sc<float>(MONITOR->m_scale));
                if (RES.x < 2 || RES.y < 2)
                    return {};
                const CBox FULL{0, 0, RES.x, RES.y};
                const CBox BAND = bandBox(m_ripple, RES, SCALE);
                if (!finiteBox(BAND))
                    return {};

                CRegion clip{g_pHyprRenderer->m_renderData.damage};
                clip.intersect(BAND);
                if (clip.empty())
                    return {};

                // The ripple's own copy of the frame, the output's size.
                if (!g_copy)
                    g_copy = g_pHyprRenderer->createFB("hyprdrive-ripple");
                if (!g_copy)
                    return {};
                if (!g_copy->isAllocated() || g_copy->m_size != RES) {
                    g_copy->release();
                    if (!g_copy->alloc(sc<int>(RES.x), sc<int>(RES.y)))
                        return {};
                }

                Render::GL::g_pHyprOpenGL->blend(false);
                {
                    // Copy the band (sampling the frame being drawn would be a
                    // feedback loop). The guard rebinds the frame's own target.
                    const auto GUARD = g_pHyprRenderer->bindTempFB(g_copy);
                    drawQuad(PROGRAM, CURTEX, FULL, CRegion{BAND}, 0, m_ripple, SCALE, RES);
                }
                const auto COPYTEX = g_copy->getTexture();
                if (COPYTEX && COPYTEX->ok())
                    drawQuad(PROGRAM, COPYTEX, FULL, clip, 1, m_ripple, SCALE, RES);
                Render::GL::g_pHyprOpenGL->blend(true);
                glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); // Hyprland's premultiplied default
                return {};
            }

            ePassElementType type() override {
                return EK_CUSTOM;
            }
            bool needsLiveBlur() override {
                return false;
            }
            bool needsPrecomputeBlur() override {
                return false;
            }
            std::optional<CBox> boundingBox() override {
                const auto MONITOR = g_pHyprRenderer->m_renderData.pMonitor.lock();
                if (!MONITOR)
                    return std::nullopt;
                const float SCALE = std::max(0.25F, sc<float>(MONITOR->m_scale));
                const CBox  BAND  = bandBox(m_ripple, MONITOR->m_pixelSize, SCALE);
                if (!finiteBox(BAND))
                    return std::nullopt;
                return BAND.copy().scale(1.F / SCALE).round();
            }
            CRegion opaqueRegion() override {
                return {};
            }
            const char* passName() override {
                return PASS_NAME;
            }

          private:
            SRipple m_ripple;
        };

        void onLastMoment() {
            const auto MONITOR = g_pHyprRenderer->m_renderData.pMonitor.lock();
            if (!MONITOR || g_ripples.empty())
                return;
            const auto IT = std::ranges::find_if(g_ripples, [&](const SRipple& r) { return r.monitor == MONITOR->m_id; });
            if (IT == g_ripples.end())
                return;
            if (ageOf(*IT) >= LIFE_S) {
                // One more full frame, drawn clean, then it is gone.
                g_ripples.erase(IT);
                g_pHyprRenderer->damageMonitor(MONITOR);
                return;
            }
            g_pHyprRenderer->m_renderPass.add(makeUnique<CRipplePassElement>(*IT));
            // Every pixel again next frame: a partial redraw would bend what this
            // frame already bent.
            g_pHyprRenderer->damageMonitor(MONITOR);
        }
    }

    void init() {
        g_render = Event::bus()->m_events.render.stage.listen([](eRenderStage stage) {
            if (stage == RENDER_LAST_MOMENT)
                onLastMoment();
        });
    }

    void trigger(const PHLMONITOR& monitor, const Vector2D& originGlobal) {
        if (!monitor)
            return;
        std::erase_if(g_ripples, [&](const SRipple& r) { return r.monitor == monitor->m_id; });
        g_ripples.push_back(SRipple{
            .monitor     = monitor->m_id,
            .originLocal = (originGlobal - monitor->m_position) * monitor->m_scale,
            .born        = Time::steadyNow(),
        });
        g_pHyprRenderer->damageMonitor(monitor);
    }

    void shutdown() {
        g_render.reset();
        g_ripples.clear();
        if (Render::GL::g_pHyprOpenGL && (g_shader || g_copy)) {
            Render::GL::g_pHyprOpenGL->makeEGLCurrent();
            g_shader.reset();
            if (g_copy)
                g_copy->release();
            g_copy.reset();
        }
        g_shaderFailed = false;
    }
}
