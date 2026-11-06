// The overview's easter egg: see KeyboardCat.hpp.
#include "KeyboardCat.hpp"

#include "Config.hpp"
#include "Motion.hpp"
#include "OverviewRender.hpp"
#include "SearchSession.hpp"

#include <hyprland/src/output/Monitor.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/gl/GLTexture.hpp>
#include <hyprland/src/render/pass/RectPassElement.hpp>
#include <hyprland/src/render/pass/TexPassElement.hpp>
#include <drm_fourcc.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fcntl.h>
#include <filesystem>
#include <mutex>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace {
    using clock = std::chrono::steady_clock;

    // THE TRIGGER: more than four type-then-backspace turns inside four seconds.
    constexpr int   CYCLES     = 5;
    constexpr float WINDOW_MS  = 4000.F;
    // Keys swallowed while the hands are still mid-spam; any key after stops it.
    constexpr float GRACE_MS   = 1500.F;
    constexpr float CLOSE_MS   = 260.F; // the fold follows the band; this only bounds it
    // The frame the video plays in: its height in logical px, and the plate's
    // margin around it.
    constexpr float VIDEO_HL   = 240.F;
    constexpr float MARGIN_L   = 8.F;
    // Bigger sources are scaled down to this width before they reach us.
    constexpr int   MAX_W      = 480;
    // No frame drawn for this long while playing: the overview went away.
    constexpr float WATCHDOG_MS = 600.F;

    float msSince(clock::time_point t) {
        return std::chrono::duration<float, std::milli>(clock::now() - t).count();
    }

    // ------------------------------------------------------------- trigger --
    bool                          g_lastTyped = false;
    std::deque<clock::time_point> g_cycles;

    // --------------------------------------------------------------- state --
    enum class EPhase : uint8_t { Idle, Open, Closing };
    EPhase            g_phase = EPhase::Idle;
    clock::time_point g_startedAt{};  // the trigger
    clock::time_point g_closingAt{};
    float             g_closeFrom = 1.F; // how open it was when the fold began
    clock::time_point g_lastRender{};

    // ------------------------------------------------------------- decoder --
    struct SDecoder {
        std::atomic<pid_t>   pid{-1};
        std::atomic<int>     fd{-1};
        std::thread          reader;
        std::atomic<bool>    quit{false};
        std::atomic<bool>    ended{false};
        std::atomic<bool>    failed{false};
        std::mutex           mtx;
        std::vector<uint8_t> latest; // the newest whole frame
        uint64_t             seq = 0;
        int                  w = 0, h = 0; // set by the reader before the first frame
    };
    SDecoder* g_dec = nullptr;

    SP<Render::ITexture> g_tex;
    uint64_t             g_texSeq = 0;
    int                  g_texW = 0, g_texH = 0;

    std::string expandHome(std::string p) {
        if (p.starts_with("~/"))
            if (const char* HOME = std::getenv("HOME"))
                p = std::string{HOME} + p.substr(1);
        return p;
    }

    // One line of a command's stdout, for ffprobe. Runs on the reader thread.
    std::string firstLine(const std::vector<std::string>& argv) {
        std::string cmd;
        for (const auto& A : argv) {
            std::string q = "'";
            for (const char C : A)
                q += C == '\'' ? std::string{"'\\''"} : std::string(1, C);
            cmd += q + "' ";
        }
        cmd += "2>/dev/null";
        std::string out;
        if (FILE* p = popen(cmd.c_str(), "r")) {
            char buf[256];
            if (fgets(buf, sizeof(buf), p))
                out = buf;
            pclose(p);
        }
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
            out.pop_back();
        return out;
    }

    pid_t spawnPiped(const std::vector<std::string>& argv, int& readFd) {
        int pipefd[2];
        if (pipe2(pipefd, O_CLOEXEC) != 0)
            return -1;
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_adddup2(&fa, pipefd[1], STDOUT_FILENO);
        posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
        posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
        posix_spawnattr_t at;
        posix_spawnattr_init(&at);
        posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSID); // its own group: one kill takes it all

        std::vector<char*> args;
        for (const auto& A : argv)
            args.push_back(const_cast<char*>(A.c_str()));
        args.push_back(nullptr);

        pid_t pid = -1;
        if (posix_spawnp(&pid, args[0], &fa, &at, args.data(), environ) != 0)
            pid = -1;
        posix_spawn_file_actions_destroy(&fa);
        posix_spawnattr_destroy(&at);
        close(pipefd[1]);
        if (pid < 0) {
            close(pipefd[0]);
            return -1;
        }
        readFd = pipefd[0];
        return pid;
    }

    // The worker: probe, start ffmpeg, then read whole frames off its pipe.
    void readerMain(SDecoder* d, std::string path) {
        const auto DIMS = firstLine({"ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries", "stream=width,height", "-of", "csv=p=0:s=x", path});
        int        sw = 0, sh = 0;
        if (std::sscanf(DIMS.c_str(), "%dx%d", &sw, &sh) != 2 || sw <= 0 || sh <= 0) {
            d->failed = true;
            return;
        }
        const bool AUDIO = !firstLine({"ffprobe", "-v", "error", "-select_streams", "a:0", "-show_entries", "stream=index", "-of", "csv=p=0", path}).empty();

        int w = sw, h = sh;
        if (w > MAX_W) {
            h = std::max(2, (h * MAX_W / w) & ~1);
            w = MAX_W;
        }
        w &= ~1;
        h &= ~1;
        {
            std::lock_guard lk(d->mtx);
            d->w = w;
            d->h = h;
        }
        if (d->quit)
            return;

        // -re paces the INPUT, so both outputs run on the video's own clock.
        std::vector<std::string> argv = {"ffmpeg",    "-hide_banner", "-loglevel", "error", "-nostdin", "-re",     "-i",  path,
                                         "-map",      "0:v:0",        "-vf",       "scale=" + std::to_string(w) + ":" + std::to_string(h), "-f",
                                         "rawvideo",  "-pix_fmt",     "rgba",      "pipe:1"};
        if (AUDIO)
            argv.insert(argv.end(), {"-map", "0:a:0", "-f", "pulse", "hyprdrive"});

        int         fd  = -1;
        const pid_t PID = spawnPiped(argv, fd);
        if (PID < 0) {
            d->failed = true;
            return;
        }
        d->fd  = fd;
        d->pid = PID;
        // stop() may have run between the quit check above and the spawn, when
        // it had no pid to kill: then this thread kills its own child, or the
        // read below would wait out the whole video with the compositor joined
        // on it.
        if (d->quit)
            kill(-PID, SIGTERM);

        const size_t         BYTES = sc<size_t>(w) * sc<size_t>(h) * 4;
        std::vector<uint8_t> buf(BYTES);
        size_t               have = 0;
        while (!d->quit) {
            const ssize_t N = read(fd, buf.data() + have, BYTES - have);
            if (N < 0 && errno == EINTR)
                continue;
            if (N <= 0)
                break;
            have += sc<size_t>(N);
            if (have == BYTES) {
                std::lock_guard lk(d->mtx);
                d->latest.swap(buf);
                buf.resize(BYTES);
                ++d->seq;
                have = 0;
            }
        }
        d->ended = true;
    }

    void stopDecoder() {
        if (!g_dec)
            return;
        g_dec->quit = true;
        if (g_dec->pid > 0)
            kill(-g_dec->pid, SIGTERM); // the group: ffmpeg and anything it started
        if (g_dec->reader.joinable())
            g_dec->reader.join(); // the read returns once the pipe's writer is gone
        if (g_dec->fd >= 0)
            close(g_dec->fd);
        if (g_dec->pid > 0)
            waitpid(g_dec->pid, nullptr, 0);
        delete g_dec;
        g_dec = nullptr;
    }

    void startDecoder(const std::string& path) {
        stopDecoder();
        g_dec        = new SDecoder;
        g_dec->reader = std::thread(readerMain, g_dec, path);
    }

    void beginClose() {
        if (g_phase != EPhase::Open)
            return;
        g_closeFrom = 1.F;
        g_closingAt = clock::now();
        g_phase     = EPhase::Closing;
        stopDecoder(); // the sound stops with the key, not after the fold
    }
}

namespace ScrollOverview::KeyboardCat {

    void noteTyped() {
        g_lastTyped = true;
    }

    bool noteBackspace() {
        const bool CYCLE = std::exchange(g_lastTyped, false);
        if (!CYCLE || g_phase != EPhase::Idle || !Config::getValue<bool>("plugin:hyprdrive:search:easter_egg"))
            return false;
        const auto NOW = clock::now();
        g_cycles.push_back(NOW);
        while (!g_cycles.empty() && std::chrono::duration<float, std::milli>(NOW - g_cycles.front()).count() > WINDOW_MS)
            g_cycles.pop_front();
        if (sc<int>(g_cycles.size()) < CYCLES)
            return false;
        g_cycles.clear();

        const auto      PATH = expandHome(Config::getValue<std::string>("plugin:hyprdrive:search:easter_egg_video"));
        std::error_code ec;
        if (PATH.empty() || !std::filesystem::is_regular_file(PATH, ec))
            return false; // no video, no egg -- and no sign there was one
        g_phase      = EPhase::Open;
        g_startedAt  = NOW;
        g_lastRender = NOW;
        startDecoder(PATH);
        return true;
    }

    bool active() {
        return g_phase != EPhase::Idle;
    }

    bool handleKey(uint32_t keycode) {
        if (g_phase == EPhase::Idle)
            return false;
        // Modifiers alone neither type nor stop it.
        switch (keycode) {
            case 29: case 42: case 54: case 56: case 97: case 100: case 125: case 126: return true;
            default: break;
        }
        if (g_phase == EPhase::Open && msSince(g_startedAt) >= GRACE_MS)
            beginClose();
        return true;
    }

    bool busy() {
        return g_phase != EPhase::Idle;
    }

    void watchdog() {
        if (g_phase != EPhase::Idle && msSince(g_lastRender) > WATCHDOG_MS) {
            // The overview closed under it: no frame, no point, and no sound.
            stopDecoder();
            g_phase = EPhase::Idle;
            g_tex.reset();
        }
    }

    float bandL() {
        return g_phase == EPhase::Open ? std::max(0.F, VIDEO_HL + 2.F * MARGIN_L - OVERVIEW_PILL_HL) : 0.F;
    }

    void render(PHLMONITOR monitor, const CBox& pill, float scale, float fade, int rounding, float roundingPower, float bandNow) {
        if (g_phase == EPhase::Idle || !monitor)
            return;
        g_lastRender = clock::now();

        // Over: the video finished, or never started.
        if (g_phase == EPhase::Open && g_dec && (g_dec->failed || (g_dec->ended && msSince(g_startedAt) > 500.F)))
            beginClose();
        // How open: the band's own progress, so the frame and the strip share a
        // curve. (A results panel under it keeps the band up; the clock ends
        // the fold anyway.)
        const float FULLBAND = std::max(1.F, VIDEO_HL + 2.F * MARGIN_L - OVERVIEW_PILL_HL);
        const float BANDE    = std::clamp(bandNow / FULLBAND, 0.F, 1.F);
        if (g_phase == EPhase::Closing && (BANDE < 0.01F || msSince(g_closingAt) >= 4.F * CLOSE_MS)) {
            g_phase = EPhase::Idle;
            g_tex.reset();
            return;
        }

        // The newest frame, uploaded once.
        if (g_dec) {
            std::lock_guard lk(g_dec->mtx);
            if (g_dec->seq != g_texSeq && !g_dec->latest.empty()) {
                g_texSeq = g_dec->seq;
                g_texW   = g_dec->w;
                g_texH   = g_dec->h;
                g_tex    = makeShared<Render::GL::CGLTexture>(DRM_FORMAT_ABGR8888, g_dec->latest.data(), sc<uint32_t>(g_texW * 4), Vector2D{sc<double>(g_texW), sc<double>(g_texH)});
            }
        }

        const float  S      = std::max(scale, 0.01F);
        const double ASPECT = g_texW > 0 && g_texH > 0 ? sc<double>(g_texW) / g_texH : 4.0 / 3.0;
        const double VH     = VIDEO_HL * S, VW = VH * ASPECT, M = MARGIN_L * S;
        // Grown out of the pill: centred on it, its top edge where the pill's is.
        const CBox FULL{pill.x + pill.width / 2.0 - (VW + 2 * M) / 2.0, pill.y, VW + 2 * M, VH + 2 * M};
        const float E   = g_phase == EPhase::Closing ? std::min(BANDE, g_closeFrom) : BANDE;
        CBox        box{pill.x + (FULL.x - pill.x) * E, pill.y + (FULL.y - pill.y) * E, pill.width + (FULL.width - pill.width) * E, pill.height + (FULL.height - pill.height) * E};
        box.round();
        if (box.width < 2 || box.height < 2)
            return;

        const int RND = std::min({rounding, sc<int>(box.height / 2.0), sc<int>(box.width / 2.0)});
        OverviewRender::themePlate(box, RND, roundingPower, fade, false, true);

        // The picture fades in over the last part of the opening, out with the fold.
        const float PICA = fade * std::clamp((E - 0.6F) / 0.4F, 0.F, 1.F);
        if (PICA <= 0.01F)
            return;
        CBox inner{box.x + M * E, box.y + M * E, box.width - 2 * M * E, box.height - 2 * M * E};
        inner.round();
        if (inner.width < 2 || inner.height < 2)
            return;
        if (!g_tex || !g_tex->ok()) {
            CRectPassElement::SRectData black;
            black.box   = inner;
            black.color = CHyprColor{0.F, 0.F, 0.F, PICA};
            black.round = std::max(0, RND - sc<int>(M));
            g_pHyprRenderer->addPassElement(makeUnique<CRectPassElement>(black));
            return;
        }
        CTexPassElement::SRenderData d;
        d.tex   = g_tex;
        d.box   = inner;
        d.a     = PICA;
        d.round = std::max(0, RND - sc<int>(M));
        g_pHyprRenderer->addPassElement(makeUnique<CTexPassElement>(d));
    }

    void stop() {
        stopDecoder();
        g_phase = EPhase::Idle;
        g_tex.reset();
    }

    void shutdown() {
        stop();
        g_cycles.clear();
        g_lastTyped = false;
    }
}
