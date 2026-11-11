#include "ConfigParts.hpp"
using namespace ConfigDetail;

namespace Hyprdrive::Config {

using namespace ConfigPart;

static CHyprColor colorFromArgb(uint32_t argb, float alphaOverride = -1.F) {
    const float a = alphaOverride >= 0.F ? alphaOverride : sc<float>((argb >> 24) & 0xFF) / 255.F;
    return CHyprColor{sc<float>((argb >> 16) & 0xFF) / 255.F, sc<float>((argb >> 8) & 0xFF) / 255.F, sc<float>(argb & 0xFF) / 255.F, a};
}

struct SOmarchyThemeColors {
    bool     valid           = false;
    bool     isLight         = false;
    uint32_t background      = 0xFF0c0b0c;
    uint32_t foreground      = 0xFFFAFCFB;
    uint32_t muted           = 0xFF584e51;
    uint32_t accent          = 0xFFb59790;
    uint32_t selection       = 0xFF584e51;
    uint32_t darkForeground  = 0;
    uint32_t lightForeground = 0;
    uint32_t urgent          = 0xFFa55555;
    bool     hasDarkFg       = false;
    bool     hasLightFg      = false;
    bool     hasUrgent       = false;
};

static float relLuma(const CHyprColor& c) {
    return (0.2126F * c.r) + (0.7152F * c.g) + (0.0722F * c.b);
}

static float lumaDelta(const CHyprColor& a, const CHyprColor& b) {
    return std::abs(relLuma(a) - relLuma(b));
}

static CHyprColor readableOn(const CHyprColor& bg, std::initializer_list<CHyprColor> prefs) {
    CHyprColor best{};
    float      bestD = -1.F;
    for (const auto& p : prefs) {
        const float d = lumaDelta(p, bg);
        if (d > bestD) {
            bestD = d;
            best  = p;
        }
    }
    if (bestD >= 0.28F)
        return best;
    return relLuma(bg) <= 0.55F ? CHyprColor{1.F, 1.F, 1.F, 1.F} : CHyprColor{0.12F, 0.12F, 0.12F, 1.F};
}

// How often the theme file is looked at, at most. omarchyTheme() sits behind
// six colour accessors and every one of those is read per card per frame, so
// the mtime check ran -- getenv, a path concatenation and a stat() -- thousands
// of times a second to notice a file the user changes by hand. Same number and
// the same reasoning as the dock's FAVORITES_POLL_MS.
static constexpr float THEME_POLL_MS = 500.F;

void invalidateThemeCache() {
    ++g_themeGeneration;
}

uint64_t themeGeneration() {
    return g_themeGeneration;
}

// Omarchy writes the active palette into colors.toml on `omarchy theme set`
// and reloads Hyprland. Cached on mtime so a frame does not reopen the file,
// and the mtime itself behind THEME_POLL_MS so a frame does not stat it either.
//
// `last` is what the previous FULL check resolved to, and the throttled path
// returns exactly that -- handing back `cachedTheme` would resurrect a palette
// whose file has since gone away.
static const SOmarchyThemeColors& omarchyTheme() {
    static const SOmarchyThemeColors  NONE{};
    static SOmarchyThemeColors        cachedTheme;
    static const SOmarchyThemeColors* last = &NONE;
    static std::string                cachedPath;
    static time_t                     cachedMtime = 0;
    static Time::steady_tp            checkedAt{};
    static uint64_t                   seenGeneration = 0;

    const auto NOW = Time::steadyNow();
    if (seenGeneration == g_themeGeneration && std::chrono::duration<float, std::milli>(NOW - checkedAt).count() < THEME_POLL_MS)
        return *last;

    checkedAt      = NOW;
    seenGeneration = g_themeGeneration;
    last           = &NONE;

    const char* const STATE = std::getenv("XDG_STATE_HOME");
    const char* const HOME  = std::getenv("HOME");
    const std::string PATH  = (STATE && *STATE) ? std::string{STATE} + "/omarchy/current/theme/colors.toml" :
                              (HOME && *HOME)   ? std::string{HOME} + "/.local/state/omarchy/current/theme/colors.toml" :
                                                  std::string{};
    if (PATH.empty())
        return NONE;

    struct stat st {};
    if (stat(PATH.c_str(), &st) != 0)
        return NONE;

    last = &cachedTheme;

    if (cachedTheme.valid && cachedPath == PATH && cachedMtime == st.st_mtime)
        return cachedTheme;

    cachedTheme = {};
    cachedPath  = PATH;
    cachedMtime = st.st_mtime;

    std::ifstream in{PATH};
    if (!in)
        return cachedTheme;

    static const std::regex RE_KEYVAL{R"(^\s*([a-zA-Z0-9_]+)\s*=\s*[\"']?([^\"'\r\n]+)[\"']?)"};
    static const std::regex RE_HEX{R"(^#?([0-9A-Fa-f]{6}))"};

    std::string line;
    bool hasBg = false, hasFg = false, hasMuted = false, hasAccent = false, hasSel = false;
    bool explicitMode = false;

    while (std::getline(in, line)) {
        std::smatch match;
        if (!std::regex_search(line, match, RE_KEYVAL))
            continue;

        const auto KEY = match[1].str();
        const auto VAL = match[2].str();

        if (KEY == "mode") {
            cachedTheme.isLight = (VAL.find("light") != std::string::npos);
            explicitMode = true;
            continue;
        }

        std::smatch hexMatch;
        if (std::regex_search(VAL, hexMatch, RE_HEX)) {
            const uint32_t RGB = 0xFF000000u | sc<uint32_t>(std::strtoul(hexMatch[1].str().c_str(), nullptr, 16));
            if (KEY == "background") {
                cachedTheme.background = RGB;
                hasBg = true;
            } else if (KEY == "foreground") {
                cachedTheme.foreground = RGB;
                hasFg = true;
            } else if (KEY == "muted") {
                cachedTheme.muted = RGB;
                hasMuted = true;
            } else if (KEY == "accent") {
                cachedTheme.accent = RGB;
                hasAccent = true;
            } else if (KEY == "selection") {
                cachedTheme.selection = RGB;
                hasSel = true;
            } else if (KEY == "dark_foreground") {
                cachedTheme.darkForeground = RGB;
                cachedTheme.hasDarkFg      = true;
            } else if (KEY == "light_foreground") {
                cachedTheme.lightForeground = RGB;
                cachedTheme.hasLightFg      = true;
            } else if (KEY == "red") {
                cachedTheme.urgent    = RGB;
                cachedTheme.hasUrgent = true;
            }
        }
    }

    if (!explicitMode && hasBg) {
        const float r = sc<float>((cachedTheme.background >> 16) & 0xFF) / 255.F;
        const float g = sc<float>((cachedTheme.background >> 8) & 0xFF) / 255.F;
        const float b = sc<float>(cachedTheme.background & 0xFF) / 255.F;
        const float LUMA = (0.2126F * r) + (0.7152F * g) + (0.0722F * b);
        cachedTheme.isLight = (LUMA > 0.55F);
    }

    if (!hasFg)
        cachedTheme.foreground = cachedTheme.isLight ? 0xFF141414u : 0xFFFAFCFBu;
    if (!hasMuted)
        cachedTheme.muted = cachedTheme.isLight ? 0xFF606060u : 0xFF8A8588u;

    cachedTheme.valid = true;
    return cachedTheme;
}

static std::optional<uint32_t> omarchyThemeBackgroundRgb() {
    const auto& T = omarchyTheme();
    return T.valid ? std::optional<uint32_t>(T.background) : std::nullopt;
}

bool isLightTheme() {
    const auto& T = omarchyTheme();
    if (T.valid)
        return T.isLight;
    const auto BG = dockBackgroundColor();
    const float LUMA = (0.2126F * BG.r) + (0.7152F * BG.g) + (0.0722F * BG.b);
    return LUMA > 0.55F;
}

CHyprColor themeForeground() {
    const auto& T = omarchyTheme();
    if (T.valid)
        return colorFromArgb(T.foreground, 1.F);
    return isLightTheme() ? CHyprColor{0.08F, 0.08F, 0.08F, 1.F} : CHyprColor{1.F, 1.F, 1.F, 1.F};
}

CHyprColor themeMutedText() {
    const auto& T = omarchyTheme();
    const auto BG = T.valid ? colorFromArgb(T.background, 1.F) : dockBackgroundColor();
    const auto FG = themeForeground();

    // `muted` in colors.toml is often a surface/overlay, not body text. Prefer
    // light_foreground (Omarchy subtext) then dark_foreground, then muted —
    // whichever still contrasts with the plate.
    //
    // A fixed array, not a vector: three candidates at most, and this is read
    // once per label per frame -- the vector was a heap allocation to hold
    // 48 bytes of colour.
    std::array<CHyprColor, 3> cands{};
    size_t                    nCands = 0;
    if (T.valid && T.hasLightFg)
        cands[nCands++] = colorFromArgb(T.lightForeground, 1.F);
    if (T.valid && T.hasDarkFg)
        cands[nCands++] = colorFromArgb(T.darkForeground, 1.F);
    if (T.valid)
        cands[nCands++] = colorFromArgb(T.muted, 1.F);

    CHyprColor best = FG;
    float      bestD = -1.F;
    for (const auto& c : std::span{cands.data(), nCands}) {
        const float vsBg = lumaDelta(c, BG);
        if (vsBg < 0.22F)
            continue;
        if (vsBg > bestD) {
            bestD = vsBg;
            best  = c;
        }
    }
    if (bestD >= 0.22F)
        return best;

    const CHyprColor MIX{FG.r * 0.72F + BG.r * 0.28F, FG.g * 0.72F + BG.g * 0.28F, FG.b * 0.72F + BG.b * 0.28F, 1.F};
    if (lumaDelta(MIX, BG) >= 0.22F)
        return MIX;
    return FG;
}

CHyprColor themeAccent() {
    const auto& T = omarchyTheme();
    if (T.valid)
        return colorFromArgb(T.accent, 1.F);
    return highlightColor();
}

CHyprColor themeHeading() {
    const auto& T = omarchyTheme();
    const auto BG = T.valid ? colorFromArgb(T.background, 1.F) : dockBackgroundColor();
    return readableOn(BG, {themeAccent(), themeForeground()});
}

CHyprColor themeSelection() {
    const auto& T = omarchyTheme();
    if (T.valid)
        return colorFromArgb(T.selection, 1.F);
    return highlightColor().modifyA(0.35F);
}

CHyprColor themeSelectedFill() {
    const auto& T = omarchyTheme();
    const auto BG = T.valid ? colorFromArgb(T.background, 1.F) : dockBackgroundColor();
    const auto SEL = themeSelection();
    if (lumaDelta(SEL, BG) >= 0.10F)
        return SEL.modifyA(0.92F);
    const auto FG = themeForeground();
    return CHyprColor{FG.r, FG.g, FG.b, 0.18F};
}

CHyprColor themeSelectedText() {
    const auto FILL = themeSelectedFill();
    // Approximate the blended plate: opaque-ish fill vs translucent fg wash.
    const CHyprColor BG = FILL.a >= 0.6F ? FILL : CHyprColor{
        FILL.r * FILL.a, FILL.g * FILL.a, FILL.b * FILL.a, 1.F
    };
    return readableOn(BG, {themeForeground(), themeAccent()});
}

CHyprColor themeUrgent() {
    const auto& T = omarchyTheme();
    if (T.valid && T.hasUrgent)
        return colorFromArgb(T.urgent, 1.F);
    return CHyprColor{0.95F, 0.35F, 0.35F, 1.F};
}

CHyprColor highlightColor() {
    // The accent is read a dozen times a frame per monitor -- every tile ring,
    // every strip cell, every selected row. Through getValue<CGradientValueData>
    // each of those was two full copies of a vector-owning gradient; the handles
    // are resolved once and only the ONE colour that is wanted is copied out.
    static auto& HIGHLIGHT = valueRef<::Config::IComplexConfigValue>("plugin:hyprdrive:col_highlight");
    if (const auto* G = gradientPtr(HIGHLIGHT); G && !G->m_colors.empty() && G->m_colors[0].a > 0.001F)
        return G->m_colors[0];

    static auto& ACCENT = valueRef<::Config::IComplexConfigValue>("general:col.active_border");
    const auto*  G      = gradientPtr(ACCENT);
    return (!G || G->m_colors.empty()) ? CHyprColor{1.F, 1.F, 1.F, 1.F} : G->m_colors[0];
}

// The ring a grid tile wears when nothing is on it (the window's own inactive
// border), for the chrome that should sit as quietly as the tiles until the
// pointer or the keyboard is on it (user, 2026-09-27).
CHyprColor restingBorderColor() {
    static auto& INACTIVE = valueRef<::Config::IComplexConfigValue>("general:col.inactive_border");
    const auto*  G        = gradientPtr(INACTIVE);
    return (!G || G->m_colors.empty()) ? highlightColor() : G->m_colors[0];
}

CHyprColor highlightForeground() {
    const auto BG = highlightColor();

    // Rec. 709 relative luminance. Deciding this from the colour rather than
    // hardcoding either answer is what makes it survive a theme switch in
    // EITHER direction -- a light accent and a dark one both stay readable.
    if (const float LUMA = (0.2126F * BG.r) + (0.7152F * BG.g) + (0.0722F * BG.b); LUMA <= 0.55F)
        return CHyprColor{1.F, 1.F, 1.F, 1.F};

    // Not flat black: the accent's own dark end reads as the same material,
    // where black on a warm pink reads as a hole punched through it.
    return CHyprColor{BG.r * 0.18F, BG.g * 0.18F, BG.b * 0.18F, 1.F};
}

int themeRounding() {
    static auto& VAL = valueRef<Hyprlang::INT>("decoration:rounding");
    return std::max<int>(0, sc<int>(*VAL));
}

float themeRoundingPower() {
    static auto& VAL   = valueRef<Hyprlang::FLOAT>("decoration:rounding_power");
    const auto   POWER = sc<float>(*VAL);
    return std::clamp(POWER > 0.F ? POWER : 2.F, 1.F, 10.F);
}

// Omarchy's `omarchy font set` writes ~/.config/fontconfig/fonts.conf so the
// "monospace" alias resolves to the chosen family. Hyprland's unset default is
// Sans; that is not Omarchy's font. A user-set misc:font_family still wins.
//
// Behind THEME_POLL_MS, and by reference. This runs inside OverviewRender::text()
// for every caller that does not pin a family -- which is most of them -- so it
// was a getenv, a stat() and two std::string copies per glyph-cache PROBE. The
// misc:font_family read is inside the gate with everything else: it only moves
// on a reload, and a reload invalidates this.
const std::string& themeFontFamily() {
    static std::string     resolved{"monospace"};   // what the last full check answered
    static std::string     parsed;                  // last successful fonts.conf parse
    static std::string     parsedPath;
    static time_t          parsedMtime = 0;
    static Time::steady_tp checkedAt{};
    static uint64_t        seenGeneration = 0;

    const auto NOW = Time::steadyNow();
    if (seenGeneration == g_themeGeneration && std::chrono::duration<float, std::milli>(NOW - checkedAt).count() < THEME_POLL_MS)
        return resolved;

    checkedAt      = NOW;
    seenGeneration = g_themeGeneration;

    const auto PIN = getValue<std::string>("misc:font_family");
    if (!PIN.empty() && PIN != STRVAL_EMPTY && PIN != "Sans") {
        resolved = PIN;
        return resolved;
    }

    resolved = "monospace";

    const char* const CFG  = std::getenv("XDG_CONFIG_HOME");
    const char* const HOME = std::getenv("HOME");
    const std::string PATH = (CFG && *CFG)  ? std::string{CFG} + "/fontconfig/fonts.conf" :
                             (HOME && *HOME) ? std::string{HOME} + "/.config/fontconfig/fonts.conf" :
                                               std::string{};
    if (PATH.empty())
        return resolved;

    struct stat st {};
    if (stat(PATH.c_str(), &st) != 0)
        return resolved;

    if (parsedPath == PATH && parsedMtime == st.st_mtime && !parsed.empty()) {
        resolved = parsed;
        return resolved;
    }

    parsedPath  = PATH;
    parsedMtime = st.st_mtime;
    parsed      = "monospace";

    std::ifstream in{PATH};
    if (!in) {
        resolved = parsed;
        return resolved;
    }

    static const std::regex RE{R"(<string>([^<]+)</string>)"};
    std::string             line;
    while (std::getline(in, line)) {
        std::smatch match;
        if (!std::regex_search(line, match, RE))
            continue;
        const auto NAME = match[1].str();
        if (NAME.empty() || NAME == "monospace")
            continue;
        parsed = NAME;
        break;
    }

    resolved = parsed;
    return resolved;
}

CHyprColor dockIndicatorColor() {
    static auto&   VAL  = valueRef<Hyprlang::INT>("plugin:hyprdrive:dock:indicator_color");
    const uint32_t ARGB = sc<uint32_t>(*VAL);
    if ((ARGB >> 24) == 0)
        return highlightColor();
    return colorFromArgb(ARGB);
}

// Overview chrome (dock, search, strip, results) follows unfocused window
// opacity from ~/.config/hypr/windows.lua (`0.985 override 0.85 override`).
static float omarchyWindowOpacity() {
    static constexpr float STOCK = 0.85F;
    static auto& VAL      = valueRef<Hyprlang::FLOAT>("decoration:inactive_opacity");
    const float  INACTIVE = std::clamp(sc<float>(*VAL), 0.F, 1.F);
    return std::clamp(STOCK * INACTIVE, 0.F, 1.F);
}

// THE TERMINAL'S GLASS (user, 2026-10-16: the dock, search, results and strip
// should be the same glass as Ghostty -- the blur was already Hyprland's, the
// tint over it was not: 0.85 over the theme's darkest shade, where the terminal
// is its background-opacity over its own background). Read from Ghostty's
// config: `config` / `config.ghostty` in its config dir, every `config-file`
// it includes (relative to the including file; a leading ? is optional), and a
// `theme` for the background when none is set outright. Later lines win. Only
// what Ghostty SETS is taken: no background-opacity line means no opinion.
//
// Re-read on the theme generation (a config reload), and otherwise at most
// every THEME_POLL_MS when a file it read changed -- a theme switch that only
// rewrites Ghostty's theme file (fedora's Serpantinum sync) is seen too.
struct SGhosttyGlass {
    std::optional<uint32_t> background; // 0xRRGGBB
    std::optional<float>    opacity;
};

static std::optional<uint32_t> ghosttyHex(std::string v) {
    if (!v.empty() && v.front() == '#')
        v.erase(0, 1);
    if (v.size() != 6 || !std::ranges::all_of(v, [](char c) { return std::isxdigit(sc<unsigned char>(c)) != 0; }))
        return std::nullopt;
    return sc<uint32_t>(std::stoul(v, nullptr, 16));
}

static std::string ghosttyUnquote(std::string v) {
    while (!v.empty() && std::isspace(sc<unsigned char>(v.back())))
        v.pop_back();
    if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'') && v.back() == v.front())
        v = v.substr(1, v.size() - 2);
    return v;
}

static std::string ghosttyPath(std::string v, const std::filesystem::path& base) {
    if (!v.empty() && v.front() == '~')
        if (const char* home = std::getenv("HOME"); home && *home)
            v = std::string{home} + v.substr(1);
    std::filesystem::path p{v};
    return (p.is_absolute() ? p : base / p).string();
}

static void ghosttyRead(const std::string& path, int depth, SGhosttyGlass& out, std::optional<std::string>& theme, std::vector<std::pair<std::string, time_t>>& seen) {
    struct stat st {};
    if (depth > 6 || stat(path.c_str(), &st) != 0)
        return;
    seen.emplace_back(path, st.st_mtime);
    std::ifstream in{path};
    if (!in)
        return;
    std::vector<std::string> includes;
    std::string              line;
    while (std::getline(in, line)) {
        const auto EQ = line.find('=');
        if (EQ == std::string::npos)
            continue;
        std::string key = line.substr(0, EQ);
        std::erase_if(key, [](char c) { return std::isspace(sc<unsigned char>(c)) != 0; });
        if (key.empty() || key.front() == '#')
            continue;
        std::string val = line.substr(EQ + 1);
        val.erase(0, std::min(val.find_first_not_of(" \t"), val.size()));
        val = ghosttyUnquote(val);
        if (key == "background") {
            if (const auto HEX = ghosttyHex(val))
                out.background = HEX;
        } else if (key == "background-opacity") {
            try {
                out.opacity = std::clamp(std::stof(val), 0.F, 1.F);
            } catch (...) {}
        } else if (key == "theme")
            theme = val;
        else if (key == "config-file") {
            if (!val.empty() && val.front() == '?')
                val = ghosttyUnquote(val.substr(1));
            includes.push_back(ghosttyPath(val, std::filesystem::path{path}.parent_path()));
        }
    }
    // Ghostty loads a file's config-file includes after the file itself.
    for (const auto& INC : includes)
        ghosttyRead(INC, depth + 1, out, theme, seen);
}

static const SGhosttyGlass& ghosttyGlass() {
    static SGhosttyGlass                               cached;
    static std::vector<std::pair<std::string, time_t>> files; // what the last parse read, with mtimes
    static Time::steady_tp                             checkedAt{};
    static uint64_t                                    seenGeneration = 0;

    const auto NOW = Time::steadyNow();
    if (seenGeneration == g_themeGeneration && std::chrono::duration<float, std::milli>(NOW - checkedAt).count() < THEME_POLL_MS)
        return cached;
    const bool RELOADED = seenGeneration != g_themeGeneration;
    checkedAt           = NOW;
    seenGeneration      = g_themeGeneration;

    if (!RELOADED && !files.empty() && std::ranges::all_of(files, [](const auto& f) {
            struct stat st {};
            return stat(f.first.c_str(), &st) == 0 && st.st_mtime == f.second;
        }))
        return cached;

    const char* const CFG  = std::getenv("XDG_CONFIG_HOME");
    const char* const HOME = std::getenv("HOME");
    const std::filesystem::path DIR = (CFG && *CFG) ? std::filesystem::path{CFG} / "ghostty" :
                                      (HOME && *HOME) ? std::filesystem::path{HOME} / ".config/ghostty" :
                                                        std::filesystem::path{};
    cached = {};
    files.clear();
    if (DIR.empty())
        return cached;

    std::optional<std::string> theme;
    for (const char* NAME : {"config", "config.ghostty"})
        ghosttyRead((DIR / NAME).string(), 0, cached, theme, files);

    // A theme gives the background only where the config does not.
    if (!cached.background && theme && !theme->empty()) {
        // "light:A,dark:B": the dark one (the chrome is a dark-glass surface).
        std::string name = *theme;
        if (const auto DARK = name.find("dark:"); DARK != std::string::npos) {
            name = name.substr(DARK + 5);
            name = name.substr(0, name.find(','));
        }
        SGhosttyGlass                                      themed;
        std::optional<std::string>                         ignored;
        const std::filesystem::path                        P{name};
        for (const auto& CAND : P.is_absolute() ? std::vector<std::string>{name} :
                                                  std::vector<std::string>{(DIR / "themes" / name).string(), "/usr/share/ghostty/themes/" + name}) {
            if (std::filesystem::exists(CAND)) {
                ghosttyRead(CAND, 0, themed, ignored, files);
                break;
            }
        }
        cached.background = themed.background;
    }
    return cached;
}

CHyprColor dockBackgroundColor() {
    static auto& OPACITYVAL = valueRef<Hyprlang::FLOAT>("plugin:hyprdrive:dock:background_opacity");
    static auto& COLORVAL   = valueRef<Hyprlang::INT>("plugin:hyprdrive:dock:background_color");
    static auto& FALLBACK   = valueRef<Hyprlang::INT>("misc:background_color");

    const auto& GLASS = ghosttyGlass();

    float OPACITY = sc<float>(*OPACITYVAL);
    if (OPACITY >= 0.F)
        OPACITY = std::clamp(OPACITY, 0.F, 1.F);
    else if (GLASS.opacity)
        OPACITY = *GLASS.opacity; // the terminal's glass
    else
        OPACITY = omarchyWindowOpacity();

    const uint32_t ARGB = sc<uint32_t>(*COLORVAL);
    uint32_t       RGB  = ARGB;
    if ((ARGB >> 24) == 0) {
        if (GLASS.background)
            RGB = *GLASS.background; // the terminal's background
        else if (const auto THEME = omarchyThemeBackgroundRgb())
            RGB = *THEME;
        else
            RGB = sc<uint32_t>(*FALLBACK);
    }
    return colorFromArgb(RGB).modifyA(OPACITY);
}

int dockCornerRadius() {
    static auto& VAL = valueRef<Hyprlang::INT>("plugin:hyprdrive:dock:corner_radius");
    return std::max<int>(0, sc<int>(*VAL));
}

}
