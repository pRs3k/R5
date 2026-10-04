#include <glad/gl.h>
#include <GLFW/glfw3.h>

#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_opengl3.h>

#include "audio.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

struct ColorScheme {
    const char* name;
    float colors[4][3]; // uploaded as uPalette[4]; scenes cycle through all 4
};

// The original 6 were a single complementary pair each; expressed here as
// [A,B,A,B] so they still read as simple two-tone blends under the 4-stop
// palette system. The 4 new ones use real distinct 4-color palettes.
static const std::array<ColorScheme, 11> kColorSchemes = {{
    {"Synthwave",     {{1.00f, 0.18f, 0.62f}, {0.18f, 0.90f, 1.00f}, {1.00f, 0.18f, 0.62f}, {0.18f, 0.90f, 1.00f}}},
    {"Sunset",        {{1.00f, 0.48f, 0.22f}, {0.18f, 0.44f, 0.85f}, {1.00f, 0.48f, 0.22f}, {0.18f, 0.44f, 0.85f}}},
    {"Neon",          {{1.00f, 0.18f, 0.85f}, {0.18f, 1.00f, 0.56f}, {1.00f, 0.18f, 0.85f}, {0.18f, 1.00f, 0.56f}}},
    {"Fire & Ice",    {{1.00f, 0.29f, 0.18f}, {0.18f, 0.88f, 1.00f}, {1.00f, 0.29f, 0.18f}, {0.18f, 0.88f, 1.00f}}},
    {"Royal",         {{0.54f, 0.18f, 1.00f}, {1.00f, 0.83f, 0.18f}, {0.54f, 0.18f, 1.00f}, {1.00f, 0.83f, 0.18f}}},
    {"Ocean",         {{0.18f, 0.83f, 0.75f}, {1.00f, 0.43f, 0.37f}, {0.18f, 0.83f, 0.75f}, {1.00f, 0.43f, 0.37f}}},
    {"Half-Life",     {{1.00f, 0.38f, 0.02f}, {0.02f, 0.015f, 0.01f}, {1.00f, 0.38f, 0.02f}, {0.02f, 0.015f, 0.01f}}},
    {"Doom",          {{1.00f, 0.35f, 0.00f}, {0.75f, 0.03f, 0.03f}, {0.22f, 0.03f, 0.02f}, {0.55f, 0.85f, 0.15f}}},
    {"Fruitiger Aero",{{0.00f, 0.95f, 0.65f}, {0.35f, 1.00f, 0.15f}, {0.05f, 0.75f, 0.95f}, {0.65f, 1.00f, 0.45f}}},
    {"Nebula",        {{1.00f, 0.25f, 0.45f}, {0.15f, 0.85f, 0.85f}, {0.45f, 0.15f, 0.85f}, {1.00f, 0.70f, 0.25f}}},
    // 4 stops at even 90°-hue steps (red -> yellow -> green -> violet, then
    // wrapping back to red) rather than a literal ROYGBIV 6/7-stop strip --
    // paletteColor() only ever has 4 slots to cycle through, and spacing
    // them evenly around the full hue wheel is what actually reads as "a
    // rainbow" sweeping past as the parameter increases, instead of a
    // lopsided blend crowded into part of the wheel.
    {"Rainbow",       {{1.00f, 0.20f, 0.20f}, {1.00f, 0.90f, 0.20f}, {0.20f, 1.00f, 0.55f}, {0.45f, 0.25f, 1.00f}}},
}};

// Borderless windowed fullscreen rather than glfwSetWindowMonitor's real
// display-mode attach: avoids an actual monitor mode switch, which is a
// known trigger for freezes/crashes when overlay software (Discord,
// GeForce Experience, RTSS, Game Bar, etc.) hooks the transition.
//
// Except on Wayland, where that trick can't work at all: clients aren't
// allowed to position their own windows (glfwSetWindowPos is an error
// there), so a borderless window just stays wherever the compositor put
// it. Wayland fullscreen is a request to the compositor and never changes
// the display mode, so glfwSetWindowMonitor carries none of the
// mode-switch risk above and is the right call there.
static void toggleFullscreen(GLFWwindow* window, bool& isFullscreen,
                              int& windowedX, int& windowedY, int& windowedW, int& windowedH) {
    if (glfwGetPlatform() == GLFW_PLATFORM_WAYLAND) {
        if (!isFullscreen) {
            glfwGetWindowSize(window, &windowedW, &windowedH);
            GLFWmonitor* monitor = glfwGetPrimaryMonitor();
            const GLFWvidmode* mode = glfwGetVideoMode(monitor);
            glfwSetWindowMonitor(window, monitor, 0, 0, mode->width, mode->height, mode->refreshRate);
        } else {
            glfwSetWindowMonitor(window, nullptr, 0, 0, windowedW, windowedH, GLFW_DONT_CARE);
        }
        isFullscreen = !isFullscreen;
        return;
    }

    if (!isFullscreen) {
        glfwGetWindowPos(window, &windowedX, &windowedY);
        glfwGetWindowSize(window, &windowedW, &windowedH);
        GLFWmonitor* monitor = glfwGetPrimaryMonitor();
        const GLFWvidmode* mode = glfwGetVideoMode(monitor);
        int monitorX, monitorY;
        glfwGetMonitorPos(monitor, &monitorX, &monitorY);
        glfwSetWindowAttrib(window, GLFW_DECORATED, GLFW_FALSE);
        glfwSetWindowPos(window, monitorX, monitorY);
        glfwSetWindowSize(window, mode->width, mode->height);
        isFullscreen = true;
    } else {
        glfwSetWindowAttrib(window, GLFW_DECORATED, GLFW_TRUE);
        glfwSetWindowPos(window, windowedX, windowedY);
        glfwSetWindowSize(window, windowedW, windowedH);
        isFullscreen = false;
    }
}

static const char* kVertexShaderSrc = R"(#version 410 core
const vec2 kPositions[3] = vec2[3](
    vec2(-1.0, -1.0),
    vec2( 3.0, -1.0),
    vec2(-1.0,  3.0)
);
void main() {
    gl_Position = vec4(kPositions[gl_VertexID], 0.0, 1.0);
}
)";

static GLuint compileStage(GLenum stage, const char* src) {
    GLuint shader = glCreateShader(stage);
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        std::fprintf(stderr, "[shader] compile error:\n%s\n", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint compileFullscreenProgram(const std::string& fragSrc) {
    GLuint vs = compileStage(GL_VERTEX_SHADER, kVertexShaderSrc);
    if (vs == 0) return 0;
    GLuint fs = compileStage(GL_FRAGMENT_SHADER, fragSrc.c_str());
    if (fs == 0) {
        glDeleteShader(vs);
        return 0;
    }

    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        std::fprintf(stderr, "[shader] link error:\n%s\n", log);
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

// GLSL-style smoothstep, for scripting easing curves host-side to match
// the ones used throughout the shaders.
static float smoothstepf(float lo, float hi, float x) {
    float t = std::clamp((x - lo) / (hi - lo), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

static std::string readFile(const std::string& path) {
    // MSVC's ifstream construction has an internal throw path (independent
    // of the stream's exceptions() mask, which defaults to non-throwing)
    // in rare cases. Both callers already treat "couldn't read the file"
    // as a normal failure — an empty string, leading to a logged compile
    // error — so that's what an exception here becomes too, rather than
    // risking std::terminate over a missing/locked shader file.
    try {
        std::ifstream file(path);
        std::stringstream ss;
        ss << file.rdbuf();
        return ss.str();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[file] could not read %s: %s\n", path.c_str(), e.what());
        return {};
    }
}

static GLuint loadFixedProgram(const std::string& path) {
    GLuint prog = compileFullscreenProgram(readFile(path));
    if (prog == 0) {
        std::fprintf(stderr, "[shader] fatal: could not compile %s\n", path.c_str());
    }
    return prog;
}

struct ShaderReloader {
    std::string path;
    std::filesystem::file_time_type lastWrite{};
    GLuint program = 0;

    bool checkAndReload() {
        // The whole body is wrapped rather than just the filesystem call:
        // this is polled every 0.25s from main()'s loop, and both
        // last_write_time (narrow-to-wide path conversion) and ifstream
        // construction in readFile() have internal MSVC throw paths that
        // survive the error_code overload / default non-throwing stream
        // mode in rare cases (bad encoding, races with external deletion,
        // etc). Letting any of those escape would take the whole app down
        // via std::terminate over a routine hot-reload check.
        try {
            std::error_code ec;
            auto writeTime = std::filesystem::last_write_time(path, ec);
            if (ec || writeTime == lastWrite) return false;
            lastWrite = writeTime;

            GLuint newProgram = compileFullscreenProgram(readFile(path));
            if (newProgram != 0) {
                if (program != 0) glDeleteProgram(program);
                program = newProgram;
                std::printf("[shader] reloaded %s\n", path.c_str());
                std::fflush(stdout);
                return true;
            }
            std::fprintf(stderr, "[shader] keeping previous program (compile failed)\n");
            return false;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[shader] checkAndReload threw: %s\n", e.what());
            return false;
        }
    }
};

struct Framebuffer {
    GLuint fbo = 0;
    GLuint colorTex = 0;
    int width = 0;
    int height = 0;

    bool mipmapped = false;

    void create(int w, int h, bool useMipmaps = false) {
        destroy();
        width = w;
        height = h;
        mipmapped = useMipmaps;

        glGenTextures(1, &colorTex);
        glBindTexture(GL_TEXTURE_2D, colorTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, useMipmaps ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        if (useMipmaps) glGenerateMipmap(GL_TEXTURE_2D);

        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorTex, 0);
        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            std::fprintf(stderr, "[fbo] incomplete: 0x%x\n", status);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    void destroy() {
        if (colorTex) glDeleteTextures(1, &colorTex);
        if (fbo) glDeleteFramebuffers(1, &fbo);
        colorTex = 0;
        fbo = 0;
        width = height = 0;
    }

    void bindAndClear() const {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glViewport(0, 0, width, height);
    }
};

static void glfwErrorCallback(int code, const char* description) {
    std::fprintf(stderr, "[glfw] error %d: %s\n", code, description);
}

// The real entry point -- kept separate from main() below so any exception
// escaping it (std::bad_alloc, a std::filesystem call outside the
// hot-reload try/catch, etc.) has somewhere to land instead of reaching an
// implicit-noexcept main() and calling std::terminate() with no diagnostic
// and no GLFW cleanup. Genuinely possible here: this loop runs for hours
// doing file I/O and allocation every frame.
static int runApp() {
    glfwSetErrorCallback(glfwErrorCallback);
    if (!glfwInit()) {
        std::fprintf(stderr, "glfwInit failed\n");
        return 1;
    }

    // 4.1 rather than a higher version: it's macOS's hard ceiling (Apple
    // never shipped OpenGL past it), and nothing this project actually
    // does -- checked directly against every gl* call in this file --
    // needs anything newer, so there's no reason to request more on
    // Windows and maintain two versions. GLFW_OPENGL_FORWARD_COMPAT is
    // required for a core-profile context on macOS; harmless elsewhere.
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);

    GLFWwindow* window = glfwCreateWindow(1280, 720, "R5", nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "glfwCreateWindow failed\n");
        glfwTerminate();
        return 1;
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    if (!gladLoadGL(glfwGetProcAddress)) {
        std::fprintf(stderr, "gladLoadGL failed\n");
        return 1;
    }

    std::printf("GL_VENDOR:   %s\n", glGetString(GL_VENDOR));
    std::printf("GL_RENDERER: %s\n", glGetString(GL_RENDERER));
    std::printf("GL_VERSION:  %s\n", glGetString(GL_VERSION));

    std::fflush(stdout);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    // Must match the 4.1 context requested above (as every .frag in
    // shaders/ already does). Asking for GLSL 4.50 under a 4.1 request
    // only works where the driver happens to hand back a newer context
    // than asked for -- usual on Windows and Mesa, but not guaranteed on
    // older Linux GPUs/drivers, and never on macOS.
    ImGui_ImplOpenGL3_Init("#version 410 core");

    bool isFullscreen = false;
    int windowedX = 100, windowedY = 100, windowedW = 1280, windowedH = 720;
    bool showSettings = false;
    int colorSchemeIndex = 0;

    // Render-resolution quality presets, exposed in Settings so people on
    // less powerful GPUs can turn it down instead of eating a fixed cost.
    struct QualityPreset { const char* name; float supersample; };
    constexpr QualityPreset kQualityPresets[] = {
        {"Low", 1.0f}, {"Medium", 1.5f}, {"High", 2.0f},
    };
    int qualityIndex = 2; // High, matching the previous fixed 2.0x default
    float appliedSupersampleFactor = -1.0f; // forces framebuffer (re)creation on first frame

    GLuint vao;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);

    const std::string shaderDir = SHADER_DIR;

    ShaderReloader sceneShader;
    sceneShader.path = shaderDir + "/scene.frag";
    sceneShader.checkAndReload();
    if (sceneShader.program == 0) {
        std::fprintf(stderr, "initial scene shader compile failed, exiting\n");
        return 1;
    }

    GLuint brightProgram = loadFixedProgram(shaderDir + "/bright_extract.frag");
    GLuint blurProgram = loadFixedProgram(shaderDir + "/blur.frag");
    GLuint compositeProgram = loadFixedProgram(shaderDir + "/composite.frag");
    GLuint bloomCombineProgram = loadFixedProgram(shaderDir + "/bloom_combine.frag");
    if (brightProgram == 0 || blurProgram == 0 || compositeProgram == 0 || bloomCombineProgram == 0) {
        std::fprintf(stderr, "fixed pipeline shader compile failed, exiting\n");
        return 1;
    }

    GLint sceneLocTime = glGetUniformLocation(sceneShader.program, "iTime");
    GLint sceneLocRes = glGetUniformLocation(sceneShader.program, "iResolution");
    GLint sceneLocAudio = glGetUniformLocation(sceneShader.program, "iAudio");
    GLint sceneLocMotion = glGetUniformLocation(sceneShader.program, "iMotion");
    GLint sceneLocAudioSlow = glGetUniformLocation(sceneShader.program, "iAudioSlow");
    GLint sceneLocPalette = glGetUniformLocation(sceneShader.program, "uPalette");
    GLint sceneLocShockwave = glGetUniformLocation(sceneShader.program, "iShockwave");
    GLint sceneLocSpeed = glGetUniformLocation(sceneShader.program, "iSpeed");
    GLint sceneLocPan = glGetUniformLocation(sceneShader.program, "iPan");
    GLint sceneLocMorph = glGetUniformLocation(sceneShader.program, "iMorph");
    GLint sceneLocPortalProgress = glGetUniformLocation(sceneShader.program, "iPortalProgress");
    GLint sceneLocDimension = glGetUniformLocation(sceneShader.program, "iDimension");
    GLint sceneLocWorldBlend = glGetUniformLocation(sceneShader.program, "iWorldBlend");
    GLint sceneLocTunnelTravel = glGetUniformLocation(sceneShader.program, "iTunnelTravel");
    GLint sceneLocOrbitTime = glGetUniformLocation(sceneShader.program, "iOrbitTime");

    GLint brightLocThreshold = glGetUniformLocation(brightProgram, "uThreshold");

    GLint blurLocTexelStep = glGetUniformLocation(blurProgram, "uTexelStep");
    GLint blurLocOutputSize = glGetUniformLocation(blurProgram, "uOutputSize");
    GLint blurLocLod = glGetUniformLocation(blurProgram, "uLod");

    GLint combineLocTex = glGetUniformLocation(bloomCombineProgram, "uTex");
    GLint combineLocOutputSize = glGetUniformLocation(bloomCombineProgram, "uOutputSize");
    GLint combineLocWeight = glGetUniformLocation(bloomCombineProgram, "uWeight");

    GLint compLocRes = glGetUniformLocation(compositeProgram, "iResolution");
    GLint compLocTime = glGetUniformLocation(compositeProgram, "iTime");
    GLint compLocAudio = glGetUniformLocation(compositeProgram, "iAudio");
    GLint compLocAudioSlow = glGetUniformLocation(compositeProgram, "iAudioSlow");
    GLint compLocColorA = glGetUniformLocation(compositeProgram, "uColorA");
    GLint compLocColorB = glGetUniformLocation(compositeProgram, "uColorB");
    GLint compLocSpectrum = glGetUniformLocation(compositeProgram, "uSpectrum");
    GLint compLocSceneTex = glGetUniformLocation(compositeProgram, "uScene");
    GLint compLocBloomTex = glGetUniformLocation(compositeProgram, "uBloom");
    GLint compLocDofTex = glGetUniformLocation(compositeProgram, "uDofBlur");
    GLint compLocExposure = glGetUniformLocation(compositeProgram, "uExposure");
    GLint compLocPortalProgress = glGetUniformLocation(compositeProgram, "uPortalProgress");

    Framebuffer sceneFB, brightFB, blurFB[2], dofBlurFB[2], bloomAccumFB;
    int fbWidth = 0, fbHeight = 0;

    // Multi-scale bloom: brightFB's mip chain (regenerated each frame,
    // same technique as sceneFB's DOF mips) gives each scale a properly
    // pre-filtered source instead of a single blur radius re-widened —
    // scale 0 is a tight hot core, higher scales get progressively wider
    // AND sample a coarser mip level so the wider taps don't alias on the
    // bright-pass's own hard edges. Weights fall off so the core still
    // reads as the dominant, brightest layer.
    struct BloomScale { float texelStep; float lod; float weight; };
    constexpr BloomScale kBloomScales[] = {
        {0.75f, 0.0f, 1.0f},
        {1.5f,  1.0f, 0.6f},
        {3.0f,  2.0f, 0.32f},
        {6.0f,  3.0f, 0.16f},
    };

    AudioAnalyzer audio;
    if (!audio.init()) {
        std::fprintf(stderr, "[audio] init failed, continuing with silent audio uniforms\n");
    }

    // Continuous motion state, integrated smoothly each frame. Big accents
    // inject a decaying impulse into joltVelocity/shakeAmount rather than
    // snapping position/rotation directly off noisy per-frame audio bands.
    float travelDistance = 0.0f;
    float ringRotation = 0.0f;
    float orbitTime = 0.0f;
    float huePhase = 0.0f;
    float joltVelocity = 0.0f;
    float shakeAmount = 0.0f;
    float exposure = 1.0f;

    // Donut <-> plasma-orb morph. Only a genuinely huge musical moment
    // flips morphTarget (0 or 1); morphAmount eases toward it over a
    // couple seconds rather than snapping, so the transformation itself
    // reads as an event rather than a glitch. A long cooldown keeps it
    // rare — this is meant to feel like a rare "the being changes form"
    // moment, not something that retriggers every big chorus hit.
    float morphTarget = 0.0f;
    float morphAmount = 0.0f;
    float lastHugeMomentTime = -1000.0f;
    constexpr float kMorphTriggerThreshold = 1.75f; // bigAccent caps at 2.0 — this is the rare top sliver
    constexpr float kMorphCooldown = 25.0f;         // seconds; keeps it a rare event even in a wild set

    // Portal fly-through: shares the same huge-moment detector and
    // cooldown as the orb morph, alternating which one fires so the two
    // "the scene transforms" events don't compete for the same instant.
    // dimensionState here means "which world are we in" (0 = donut/orb,
    // 1 = inside the tunnel) — it flips exactly once per trigger, at
    // kPortalCutPoint through the envelope, hidden behind the flash in
    // composite.frag. Persists until the next portal trigger flips it back
    // (entering and exiting are deliberately NOT the same choreography
    // played in reverse — see worldBlend's isExiting branch below).
    bool nextHugeMomentIsPortal = false;
    float portalTriggerTime = -1000.0f;
    float dimensionState = 0.0f;
    // This initial value only governs the window before the very first real
    // trigger ever fires: portalTriggerTime still reads as the far-past
    // sentinel below, which makes portalProgress permanently saturated at
    // 1.0, which collapses worldBlend's formula to (1 - this value). For
    // that to rest at dimensionState's initial 0 (normal orbit cam,
    // matching the donut), this needs to be 1.0, not 0.0 — despite looking
    // backwards next to dimensionState's own initial value just above.
    // Once a real trigger fires, this stale value stops mattering: the
    // trigger handler below reassigns it to dimensionState (read before the
    // flip) at that exact moment, which is what actually keeps every real
    // transition correct, in both directions, from then on.
    float tunnelStateAtTriggerStart = 1.0f;
    bool pendingDimensionFlip = false;
    float tunnelTravel = 0.0f; // distance flown down the tunnel since the most recent entry
    constexpr float kPortalDuration = 7.0f;   // seconds for the full align -> cross -> settle envelope
    constexpr float kPortalCutPoint = 0.35f;  // fraction of the envelope where alignment finishes and the hard cut happens
    constexpr float kTunnelFlightSpeed = 4.0f;

    // Ages (seconds since spawn) of up to 3 concurrent shockwave rings.
    // A large sentinel age means "not active" / fully faded.
    std::array<float, 3> shockwaveAges = {99.0f, 99.0f, 99.0f};
    float prevOnsetPulse = 0.0f;

    constexpr float kBaseSpeed = 1.7f;
    constexpr float kBaseRotSpeed = 0.35f;

    auto start = std::chrono::steady_clock::now();
    auto lastReloadCheck = start;
    auto lastAudioLog = start;
    auto lastFrameTime = start;

    bool fKeyWasDown = false;

    auto doToggleFullscreen = [&]() {
        toggleFullscreen(window, isFullscreen, windowedX, windowedY, windowedW, windowedH);
        int nw, nh, nx, ny;
        glfwGetWindowSize(window, &nw, &nh);
        glfwGetWindowPos(window, &nx, &ny);
        std::printf("[fullscreen] now %d, window pos=(%d,%d) size=(%d,%d)\n", isFullscreen, nx, ny, nw, nh);
        std::fflush(stdout);
    };

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        // Must happen before ImGui::NewFrame() below, not after — a
        // minimized window (0x0 framebuffer) used to hit a `continue` further
        // down that skipped the matching ImGui::Render()/EndFrame() for a
        // frame NewFrame() had already opened, and the very next NewFrame()
        // call would hit ImGui's internal assertion for exactly that
        // ("Forgot to call Render() or EndFrame()...") and abort() the
        // whole process. Bailing out here instead means we never open an
        // ImGui frame we're not going to close.
        int w, h;
        glfwGetFramebufferSize(window, &w, &h);
        if (w == 0 || h == 0) continue;

        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        }

        const bool fKeyDown = glfwGetKey(window, GLFW_KEY_F) == GLFW_PRESS;
        if (fKeyDown && !fKeyWasDown) {
            doToggleFullscreen();
        }
        fKeyWasDown = fKeyDown;

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        {
            const ImGuiIO& io = ImGui::GetIO();
            ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 108, 12), ImGuiCond_Always);
            ImGui::SetNextWindowBgAlpha(0.35f);
            ImGui::Begin("##settingsButton", nullptr,
                          ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoMove |
                          ImGuiWindowFlags_NoSavedSettings);
            if (ImGui::Button(showSettings ? "Close" : "Settings", ImVec2(88, 28))) {
                showSettings = !showSettings;
            }
            ImGui::End();

            if (showSettings) {
                ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 268, 48), ImGuiCond_Once);
                ImGui::SetNextWindowSize(ImVec2(256, 0), ImGuiCond_Once);
                ImGui::Begin("Settings", &showSettings, ImGuiWindowFlags_NoSavedSettings);

                bool fs = isFullscreen;
                if (ImGui::Checkbox("Fullscreen", &fs)) {
                    doToggleFullscreen();
                }
                ImGui::TextDisabled("(F key also toggles)");

                ImGui::Separator();
                ImGui::Text("Color scheme");
                for (int i = 0; i < static_cast<int>(kColorSchemes.size()); ++i) {
                    if (ImGui::RadioButton(kColorSchemes[i].name, colorSchemeIndex == i)) {
                        colorSchemeIndex = i;
                    }
                }

                ImGui::Separator();
                ImGui::Text("Render quality");
                for (int i = 0; i < static_cast<int>(sizeof(kQualityPresets) / sizeof(kQualityPresets[0])); ++i) {
                    if (ImGui::RadioButton(kQualityPresets[i].name, qualityIndex == i)) {
                        qualityIndex = i;
                    }
                }
                ImGui::TextDisabled("Lower this if the app runs slow");
                ImGui::End();
            }
        }

        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<float>(now - lastReloadCheck).count() > 0.25f) {
            lastReloadCheck = now;
            if (sceneShader.checkAndReload()) {
                sceneLocTime = glGetUniformLocation(sceneShader.program, "iTime");
                sceneLocRes = glGetUniformLocation(sceneShader.program, "iResolution");
                sceneLocAudio = glGetUniformLocation(sceneShader.program, "iAudio");
                sceneLocMotion = glGetUniformLocation(sceneShader.program, "iMotion");
                sceneLocAudioSlow = glGetUniformLocation(sceneShader.program, "iAudioSlow");
                sceneLocPalette = glGetUniformLocation(sceneShader.program, "uPalette");
                sceneLocShockwave = glGetUniformLocation(sceneShader.program, "iShockwave");
                sceneLocSpeed = glGetUniformLocation(sceneShader.program, "iSpeed");
                sceneLocPan = glGetUniformLocation(sceneShader.program, "iPan");
                sceneLocMorph = glGetUniformLocation(sceneShader.program, "iMorph");
                sceneLocPortalProgress = glGetUniformLocation(sceneShader.program, "iPortalProgress");
                sceneLocDimension = glGetUniformLocation(sceneShader.program, "iDimension");
                sceneLocWorldBlend = glGetUniformLocation(sceneShader.program, "iWorldBlend");
                sceneLocTunnelTravel = glGetUniformLocation(sceneShader.program, "iTunnelTravel");
                sceneLocOrbitTime = glGetUniformLocation(sceneShader.program, "iOrbitTime");
            }
        }

        audio.update();

        float rawDt = std::chrono::duration<float>(now - lastFrameTime).count();
        lastFrameTime = now;
        float dt = std::clamp(rawDt, 0.0f, 0.05f);

        const float bassSlow = audio.bassSlow(), midSlow = audio.midSlow(), trebleSlow = audio.trebleSlow();
        const float bigAccent = audio.bigAccent();

        float t = std::chrono::duration<float>(now - start).count();

        // A genuinely huge hit (near bigAccent's own cap, not just any
        // routine "big" accent) fires one of the two big transformation
        // events, provided the shared cooldown has cleared — alternating
        // between them so consecutive huge moments don't just retrigger
        // the same one, and so they never both start at once.
        if (bigAccent > kMorphTriggerThreshold && (t - lastHugeMomentTime) > kMorphCooldown) {
            lastHugeMomentTime = t;
            if (nextHugeMomentIsPortal) {
                if (morphTarget > 0.5f) {
                    // The tunnel only ever connects to the donut, never
                    // the orb — no hole to fly through on entry, and on
                    // exit the ride should reverse the same donut<->tunnel
                    // choreography it used going in, not surface into a
                    // shape it never entered from. Gated both directions:
                    // if a portal turn comes up while morphed to the orb
                    // (entering OR exiting), spend this huge moment
                    // morphing back to donut instead; the portal keeps its
                    // turn and fires on the next one, by which point
                    // morphAmount has long since eased to 0 (kMorphCooldown
                    // is 25s, morphAmount's ease constant is under 2s).
                    morphTarget = 0.0f;
                } else {
                    portalTriggerTime = t;
                    pendingDimensionFlip = true;
                    tunnelStateAtTriggerStart = dimensionState;
                    tunnelTravel = 0.0f;
                    nextHugeMomentIsPortal = false;
                }
            } else {
                morphTarget = 1.0f - morphTarget;
                nextHugeMomentIsPortal = true;
            }
        }
        morphAmount += (morphTarget - morphAmount) * std::min(1.0f, dt * 0.6f);

        // Portal progress is a fixed-duration scripted envelope (not an
        // eased approach toward a target like morphAmount) — it's a
        // one-shot event, not a persistent state to hold. dimensionState
        // flips exactly once per trigger, at kPortalCutPoint through the
        // envelope, which is also where the flash in composite.frag peaks
        // — hiding the hard cut behind it, the same trick real editors use
        // to hide a hard cut behind a whip-pan or a flash cut.
        const float portalProgress = std::clamp((t - portalTriggerTime) / kPortalDuration, 0.0f, 1.0f);
        if (pendingDimensionFlip && portalProgress >= kPortalCutPoint) {
            dimensionState = 1.0f - dimensionState;
            pendingDimensionFlip = false;
        }
        // Eases from the world we were in toward the one we're headed to —
        // but *when* it eases is direction-dependent, not symmetric, so
        // the two directions read as a real reverse of each other rather
        // than the same swing played backwards:
        //  - Entering the tunnel: ease over the approach portion [0, cut],
        //    reaching fully tunnel-locked right as the hard cut happens.
        //    The donut is still what's on screen while the camera lines
        //    up with its hole, so seeing it swing into alignment first
        //    reads correctly — "lining up, then diving in."
        //  - Exiting the tunnel: stay fully tunnel-locked THROUGH the cut
        //    (ease over [cut, 1] instead), so the donut appears dead
        //    ahead — exactly where the tunnel was receding to — before
        //    the camera starts swinging back toward its normal orbit.
        //    Easing early here (the entry's window) swung the camera away
        //    from the tunnel-lock view *while the tunnel geometry was
        //    still on screen*, then popped the donut in once the swing
        //    finished — reads as "camera turns, donut just appears"
        //    instead of "we flew out the end and there it was."
        const bool isExiting = tunnelStateAtTriggerStart > 0.5f;
        const float easeT = isExiting
            ? smoothstepf(kPortalCutPoint, 1.0f, portalProgress)
            : smoothstepf(0.0f, kPortalCutPoint, portalProgress);
        const float worldBlend = std::clamp(
            tunnelStateAtTriggerStart + (1.0f - 2.0f * tunnelStateAtTriggerStart) * easeT,
            0.0f, 1.0f);

        joltVelocity += bigAccent * 6.0f * dt;
        joltVelocity *= std::exp(-dt * 3.0f);

        const float speed = kBaseSpeed + bassSlow * 0.7f + joltVelocity;
        travelDistance += speed * dt;

        // The donut's spin visibly eases to a stop as the camera lines up
        // with the hole (worldBlend -> 1), rather than being frozen at a
        // captured angle — scaling the increment down to ~0 has the same
        // effect but composes for free: the axis just asymptotically
        // settles wherever the spin happened to be heading, and resumes
        // smoothly (no snap) once worldBlend eases back down on the way out.
        const float rotSpeed = kBaseRotSpeed + midSlow * 0.5f;
        ringRotation += rotSpeed * dt * (1.0f - worldBlend);

        // The normal-mode orbit camera position must freeze in lockstep with
        // ringRotation once worldBlend -> 1: the shader picks which side of
        // the frozen axis to fly down (side = sign(dot(normalCamPos-target,
        // axisDir))) by re-evaluating that dot product every frame. If the
        // orbit camera kept sweeping on raw iTime while the axis itself sat
        // still, it would eventually cross the plane perpendicular to the
        // axis and flip that sign mid-flight, snapping the whole tunnel-cam
        // formula to the opposite side — the "rotating out of view" bug.
        // Driving the orbit off this same eased accumulator instead of iTime
        // keeps normalCamPos (and therefore side) stable once frozen, and
        // resumes it from exactly where it left off on the way back out.
        orbitTime += dt * (1.0f - worldBlend);

        // Distance flown down the tunnel so far this trip — only advances
        // once actually inside (past the hard cut), continuing for as
        // long as we stay there, and reset to 0 on every new trigger (see
        // above). The camera's world position is derived directly from
        // this value (see axisCamPos in scene.frag), so it's still kept
        // bounded rather than left to grow forever across an
        // unrealistically long stay — but the wrap period needs to be far
        // outside any real visit's reach: this used to be 80 units, which
        // at kTunnelFlightSpeed sounds generous but is only ~20-40
        // seconds of flight, comfortably within a normal visit — the
        // instant subtraction when it wrapped was a real, visible camera
        // teleport (reported as "the tunnel glitches and restarts every
        // so often"), not the seamless loop the old comment here assumed.
        // A real visit tops out somewhere in the hundreds of units even
        // accounting for the portal gate's occasional extra redirect
        // delay (see main-loop trigger handling); this period is ~25x
        // that, and float32 precision is still fine at this magnitude
        // (error stays well under a tenth of the tube's own radius), so
        // in practice this now never fires during an actual visit.
        tunnelTravel += dt * kTunnelFlightSpeed * dimensionState;
        constexpr float kTunnelWrapPeriod = 10000.0f;
        if (tunnelTravel > kTunnelWrapPeriod) tunnelTravel -= kTunnelWrapPeriod;

        huePhase += dt * (0.05f + midSlow * 0.08f);

        // Reserve camera shake for genuinely exceptional hits (bass drops,
        // etc.) rather than every big accent — those still drive bolts/
        // flash/crackle on their own, just without shaking the camera.
        const float shakeTrigger = std::max(0.0f, bigAccent - 1.1f);
        shakeAmount = shakeAmount * std::exp(-dt * 6.0f) + shakeTrigger * 0.6f;

        // Auto-exposure: eases down as the scene's audio-driven brightness
        // (veins, bloom, bolts — all scale with this same energy) climbs,
        // to keep loud/energetic stretches from just blowing out to white,
        // and eases up above baseline during quiet stretches so calm
        // moments don't read as flat/dim. Darkens quickly (like a pupil
        // reacting to a flash) but recovers slowly, rather than tracking
        // instantaneously — that asymmetry is what reads as "adaptation"
        // instead of a distracting brightness wobble.
        // Range narrowed from (0.65, 1.15) — the wider swing was crushing
        // low-brightness background layers (the nebula) toward black
        // during energetic passages, to the point they only read clearly
        // once the music went quiet again. This still compensates for
        // blowout on loud hits without suppressing the background as hard.
        const float sceneEnergy = bassSlow * 0.25f + midSlow * 0.25f + trebleSlow * 0.15f + bigAccent * 0.5f;
        const float targetExposure = std::clamp(1.15f - sceneEnergy * 0.22f, 0.85f, 1.15f);
        const float exposureRate = (targetExposure < exposure) ? 3.0f : 0.8f;
        exposure += (targetExposure - exposure) * std::min(1.0f, dt * exposureRate);

        for (float& age : shockwaveAges) age += dt;
        const float onsetNow = audio.onsetPulse();
        if (onsetNow >= 0.999f && prevOnsetPulse < 0.999f) {
            const int oldest = static_cast<int>(std::max_element(shockwaveAges.begin(), shockwaveAges.end()) - shockwaveAges.begin());
            shockwaveAges[oldest] = 0.0f;
        }
        prevOnsetPulse = onsetNow;

        if (std::chrono::duration<float>(now - lastAudioLog).count() > 0.5f) {
            lastAudioLog = now;
            std::printf("[audio] bass=%.3f mid=%.3f treble=%.3f vol=%.4f onset=%.2f bigAccent=%.2f speed=%.2f frameMs=%.2f\n",
                        audio.bass(), audio.mid(), audio.treble(), audio.volume(), audio.onsetPulse(), bigAccent, speed, rawDt * 1000.0f);
            std::fflush(stdout);
        }

        // The scene (raymarch) pass renders supersampled relative to the
        // window/display, then the composite pass downsamples it back to
        // native size — this is what actually delivers "4K-grade" detail
        // even on a display below 4K, and reduces aliasing everywhere.
        // Adjustable at runtime via Settings > Render quality.
        const float supersampleFactor = kQualityPresets[qualityIndex].supersample;
        const int renderW = std::max(1, static_cast<int>(w * supersampleFactor));
        const int renderH = std::max(1, static_cast<int>(h * supersampleFactor));

        if (w != fbWidth || h != fbHeight || supersampleFactor != appliedSupersampleFactor) {
            fbWidth = w;
            fbHeight = h;
            appliedSupersampleFactor = supersampleFactor;
            sceneFB.create(renderW, renderH, /*useMipmaps=*/true);
            int halfW = std::max(1, renderW / 2);
            int halfH = std::max(1, renderH / 2);
            brightFB.create(halfW, halfH, /*useMipmaps=*/true);
            blurFB[0].create(halfW, halfH);
            blurFB[1].create(halfW, halfH);
            dofBlurFB[0].create(halfW, halfH);
            dofBlurFB[1].create(halfW, halfH);
            bloomAccumFB.create(halfW, halfH);
        }

        float bass = audio.bass(), mid = audio.mid(), treble = audio.treble(), onset = audio.onsetPulse();

        // Pass 1: scene -> HDR texture (supersampled resolution)
        sceneFB.bindAndClear();
        glUseProgram(sceneShader.program);
        if (sceneLocTime >= 0) glUniform1f(sceneLocTime, t);
        if (sceneLocRes >= 0) glUniform2f(sceneLocRes, static_cast<float>(renderW), static_cast<float>(renderH));
        if (sceneLocAudio >= 0) glUniform4f(sceneLocAudio, bass, mid, treble, onset);
        if (sceneLocMotion >= 0) glUniform4f(sceneLocMotion, travelDistance, ringRotation, huePhase, shakeAmount);
        if (sceneLocAudioSlow >= 0) glUniform4f(sceneLocAudioSlow, bassSlow, midSlow, trebleSlow, bigAccent);
        const ColorScheme& scheme = kColorSchemes[colorSchemeIndex];
        if (sceneLocPalette >= 0) glUniform3fv(sceneLocPalette, 4, &scheme.colors[0][0]);
        if (sceneLocShockwave >= 0) glUniform3f(sceneLocShockwave, shockwaveAges[0], shockwaveAges[1], shockwaveAges[2]);
        if (sceneLocSpeed >= 0) glUniform1f(sceneLocSpeed, speed);
        if (sceneLocPan >= 0) glUniform1f(sceneLocPan, audio.stereoBalance());
        if (sceneLocMorph >= 0) glUniform1f(sceneLocMorph, morphAmount);
        if (sceneLocPortalProgress >= 0) glUniform1f(sceneLocPortalProgress, portalProgress);
        if (sceneLocDimension >= 0) glUniform1f(sceneLocDimension, dimensionState);
        if (sceneLocWorldBlend >= 0) glUniform1f(sceneLocWorldBlend, worldBlend);
        if (sceneLocTunnelTravel >= 0) glUniform1f(sceneLocTunnelTravel, tunnelTravel);
        if (sceneLocOrbitTime >= 0) glUniform1f(sceneLocOrbitTime, orbitTime);
        glDrawArrays(GL_TRIANGLES, 0, 3);

        // Mip chain for sceneFB, regenerated every frame since its content
        // changes every frame. GPU mip generation box-filters each level,
        // giving the DOF downsample pass a properly band-limited source
        // instead of sparse blur taps skipping over the donut's crisp edge
        // (which was aliasing into a visible woven/moire pattern).
        glBindTexture(GL_TEXTURE_2D, sceneFB.colorTex);
        glGenerateMipmap(GL_TEXTURE_2D);

        // Pass 2: bright-pass extract at half res
        brightFB.bindAndClear();
        glUseProgram(brightProgram);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, sceneFB.colorTex);
        if (brightLocThreshold >= 0) glUniform1f(brightLocThreshold, 1.4f);
        glDrawArrays(GL_TRIANGLES, 0, 3);

        // Mip chain for brightFB, regenerated every frame — each bloom
        // scale below samples a different level, so the wider scales read
        // pre-filtered (alias-safe) data instead of sparse taps skipping
        // over the bright-pass's own hard edges (same technique as
        // sceneFB's DOF mips above).
        glBindTexture(GL_TEXTURE_2D, brightFB.colorTex);
        glGenerateMipmap(GL_TEXTURE_2D);

        // Pass 3: multi-scale bloom. Blurring brightFB once at a single
        // radius gives a uniform "everything has a light blur filter"
        // look; real HDR bloom layers several widths — a tight hot core
        // plus progressively wider, dimmer bleed — which is what actually
        // reads as filmic. Each scale is blurred (ping-ponged through
        // blurFB as before) then additively accumulated into bloomAccumFB.
        glUseProgram(blurProgram);
        bloomAccumFB.bindAndClear();
        glClear(GL_COLOR_BUFFER_BIT);
        for (const BloomScale& scale : kBloomScales) {
            // Blur ping-pong: plain overwrite, blending must be OFF here —
            // it's not part of the accumulation, just scratch space reused
            // every scale.
            glUseProgram(blurProgram);
            if (blurLocLod >= 0) glUniform1f(blurLocLod, scale.lod);
            blurFB[0].bindAndClear();
            glBindTexture(GL_TEXTURE_2D, brightFB.colorTex);
            if (blurLocTexelStep >= 0) {
                glUniform2f(blurLocTexelStep, scale.texelStep / brightFB.width, 0.0f);
            }
            if (blurLocOutputSize >= 0) {
                glUniform2f(blurLocOutputSize, static_cast<float>(blurFB[0].width), static_cast<float>(blurFB[0].height));
            }
            glDrawArrays(GL_TRIANGLES, 0, 3);

            blurFB[1].bindAndClear();
            glBindTexture(GL_TEXTURE_2D, blurFB[0].colorTex);
            if (blurLocTexelStep >= 0) {
                glUniform2f(blurLocTexelStep, 0.0f, scale.texelStep / brightFB.height);
            }
            if (blurLocOutputSize >= 0) {
                glUniform2f(blurLocOutputSize, static_cast<float>(blurFB[1].width), static_cast<float>(blurFB[1].height));
            }
            glDrawArrays(GL_TRIANGLES, 0, 3);

            // Combine: this draw specifically needs additive blending, so
            // it's scoped tightly around just this one call rather than
            // the whole loop — the blur passes above must NOT blend.
            glUseProgram(bloomCombineProgram);
            bloomAccumFB.bindAndClear();
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE);
            glBindTexture(GL_TEXTURE_2D, blurFB[1].colorTex);
            if (combineLocTex >= 0) glUniform1i(combineLocTex, 0);
            if (combineLocOutputSize >= 0) {
                glUniform2f(combineLocOutputSize, static_cast<float>(bloomAccumFB.width), static_cast<float>(bloomAccumFB.height));
            }
            if (combineLocWeight >= 0) glUniform1f(combineLocWeight, scale.weight);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glDisable(GL_BLEND);
        }

        // Pass 3.5: wide blur of the *whole* scene (not just bright-pass),
        // downsampled from full res, for the depth-of-field background.
        // Sampled from sceneFB's mip 1 (hardware box-filtered, regenerated
        // this frame) rather than mip 0, so this pass's wide/sparse taps
        // blur an already alias-safe source instead of aliasing on the
        // donut's crisp edge themselves.
        glUseProgram(blurProgram); // multi-scale bloom above leaves bloomCombineProgram bound
        if (blurLocLod >= 0) glUniform1f(blurLocLod, 1.0f);
        dofBlurFB[0].bindAndClear();
        glBindTexture(GL_TEXTURE_2D, sceneFB.colorTex);
        if (blurLocTexelStep >= 0) {
            glUniform2f(blurLocTexelStep, 3.5f / dofBlurFB[0].width, 0.0f);
        }
        if (blurLocOutputSize >= 0) {
            glUniform2f(blurLocOutputSize, static_cast<float>(dofBlurFB[0].width), static_cast<float>(dofBlurFB[0].height));
        }
        glDrawArrays(GL_TRIANGLES, 0, 3);

        if (blurLocLod >= 0) glUniform1f(blurLocLod, 0.0f);
        dofBlurFB[1].bindAndClear();
        glBindTexture(GL_TEXTURE_2D, dofBlurFB[0].colorTex);
        if (blurLocOutputSize >= 0) {
            glUniform2f(blurLocOutputSize, static_cast<float>(dofBlurFB[1].width), static_cast<float>(dofBlurFB[1].height));
        }
        if (blurLocTexelStep >= 0) {
            glUniform2f(blurLocTexelStep, 0.0f, 3.5f / dofBlurFB[0].height);
        }
        glDrawArrays(GL_TRIANGLES, 0, 3);

        // Pass 5: composite to backbuffer
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, w, h);
        glUseProgram(compositeProgram);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, sceneFB.colorTex);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, bloomAccumFB.colorTex);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, dofBlurFB[1].colorTex);
        if (compLocSceneTex >= 0) glUniform1i(compLocSceneTex, 0);
        if (compLocBloomTex >= 0) glUniform1i(compLocBloomTex, 1);
        if (compLocDofTex >= 0) glUniform1i(compLocDofTex, 2);
        if (compLocRes >= 0) glUniform2f(compLocRes, static_cast<float>(w), static_cast<float>(h));
        if (compLocTime >= 0) glUniform1f(compLocTime, t);
        if (compLocAudio >= 0) glUniform4f(compLocAudio, bass, mid, treble, onset);
        if (compLocAudioSlow >= 0) glUniform4f(compLocAudioSlow, bassSlow, midSlow, trebleSlow, bigAccent);
        if (compLocExposure >= 0) glUniform1f(compLocExposure, exposure);
        if (compLocPortalProgress >= 0) glUniform1f(compLocPortalProgress, portalProgress);
        if (compLocColorA >= 0) glUniform3f(compLocColorA, scheme.colors[0][0], scheme.colors[0][1], scheme.colors[0][2]);
        if (compLocColorB >= 0) glUniform3f(compLocColorB, scheme.colors[1][0], scheme.colors[1][1], scheme.colors[1][2]);
        if (compLocSpectrum >= 0) glUniform1fv(compLocSpectrum, AudioAnalyzer::kSpectrumBands, audio.spectrum());
        glDrawArrays(GL_TRIANGLES, 0, 3);

        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}

int main() {
    try {
        return runApp();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[fatal] unhandled exception: %s\n", e.what());
        return 1;
    } catch (...) {
        std::fprintf(stderr, "[fatal] unhandled exception of unknown type\n");
        return 1;
    }
}
