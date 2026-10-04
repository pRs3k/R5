#version 410 core

uniform sampler2D uScene;
uniform sampler2D uBloom;
uniform sampler2D uDofBlur; // wide blur of the full scene, for depth of field
uniform vec2 iResolution;
uniform float iTime;
uniform vec4 iAudio;      // x=bass y=mid z=treble w=onsetPulse
uniform vec4 iAudioSlow;  // x=bassSlow y=midSlow z=trebleSlow w=bigAccent
uniform vec3 uColorA;
uniform vec3 uColorB;
uniform float uSpectrum[16];
uniform float uExposure; // host-computed auto-exposure, eases with audio energy over time
uniform float uPortalProgress; // 0..1 through a portal fly-through; drives the blackout/blur at closest approach

out vec4 fragColor;

float hash21(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

vec3 acesFilmic(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

// Spectral lens dispersion: instead of a single R/B channel split, march a
// handful of taps along the radial direction, each carrying a slice of the
// visible spectrum (blue pulled toward center, red flung outward), and
// recombine weighted by a rough eye response. Gives a soft rainbow fringe
// that behaves like real glass rather than a hard 3-colour ghost.
vec3 spectralAberration(sampler2D tex, vec2 uv, vec2 dir, float amount) {
    const int kCASamples = 8;
    vec3 sum = vec3(0.0);
    vec3 wsum = vec3(0.0);
    for (int i = 0; i < kCASamples; ++i) {
        float t = float(i) / float(kCASamples - 1);
        vec3 w = vec3(
            smoothstep(0.35, 1.0, t),          // red weight rises toward the outward taps
            1.0 - abs(t - 0.5) * 2.0,           // green peaks in the middle
            smoothstep(0.65, 0.0, t)            // blue weight rises toward the inward taps
        );
        float shift = mix(-amount, amount, t);
        sum += texture(tex, uv + dir * shift).rgb * w;
        wsum += w;
    }
    return sum / max(wsum, vec3(1e-4));
}

vec3 zoomBlurSample(sampler2D tex, vec2 uv, vec2 center, float strength) {
    vec3 col = vec3(0.0);
    const int kSamples = 8;
    for (int i = 0; i < kSamples; ++i) {
        float scale = 1.0 - strength * 0.05 * (float(i) / float(kSamples - 1));
        vec2 suv = (uv - center) * scale + center;
        col += texture(tex, suv).rgb;
    }
    return col / float(kSamples);
}

// Radial spectrum ring: 16 segments read straight from the audio analyzer's
// log-spaced band array, arranged around the donut instead of debug bars.
vec3 radialSpectrum(vec2 fragPx, vec2 res) {
    vec2 center = res * 0.5;
    vec2 d = fragPx - center;
    float r = length(d);
    float ang = atan(d.y, d.x);
    float scaled = ((ang + 3.14159265) / 6.28318531) * 16.0;
    int idx = clamp(int(scaled), 0, 15);
    float segFrac = fract(scaled);

    float val = clamp(uSpectrum[idx], 0.0, 1.5);
    float minDim = min(res.x, res.y);
    float innerR = minDim * 0.34;
    float barH = val * minDim * 0.10 + minDim * 0.004;
    float outerR = innerR + barH;

    float gap = smoothstep(0.0, 0.08, segFrac) * smoothstep(1.0, 0.92, segFrac);
    float mask = step(innerR, r) * step(r, outerR) * gap;

    vec3 barColor = mix(uColorA, uColorB, float(idx) / 15.0);
    return barColor * mask * 1.5;
}

void main() {
    float bigAccent = iAudioSlow.w;
    float zoomStrength = smoothstep(0.60, 1.7, bigAccent);

    // Portal fly-through: the geometry hard-switches from donut/orb to
    // tunnel at kPortalCutPoint (main.cpp, mirrored here as a literal —
    // composite.frag has no access to main.cpp's constant, so keep this
    // in sync if that one ever changes). This used to hide the switch
    // behind a brief white flash; that covered the cut but didn't feel
    // like anything actually happened — no relation to flying into a
    // hole. A symmetric fade through black instead: the screen genuinely
    // goes dark right as the swap happens (scene.frag's camera dolly
    // pushes the hole to fill the frame over the same window on the way
    // in), so the cut reads as "we flew into the dark and came out the
    // other side" rather than a hidden trick.
    const float kPortalCutPoint = 0.35;
    float blackout = 1.0 - smoothstep(0.0, 0.09, abs(uPortalProgress - kPortalCutPoint));
    zoomStrength = max(zoomStrength, blackout * 0.5);

    vec2 uv = gl_FragCoord.xy / iResolution;

    vec2 centered = uv - 0.5;
    float distFromCenter = length(centered);

    // Band-jitter glitch removed from this preset (a good fit for a future
    // "glitch" preset instead). Spectral lens dispersion stays as a subtle,
    // always-on effect: near-zero at the frame centre so the hero subject
    // reads clean, ramping past the edges, and pumped by the fast onset
    // pulse plus bass and big accents so hits fringe without adding brightness.
    vec2 caDir = distFromCenter > 1e-4 ? centered / distFromCenter : vec2(0.0);
    float caEdge = pow(clamp(distFromCenter * 1.9, 0.0, 1.6), 1.5);
    float caPulse = 0.0014 + iAudio.w * 0.0026 + iAudio.x * 0.0018 + bigAccent * 0.0012;
    float caAmount = min(caPulse * caEdge, 0.02);

    vec3 sharpScene = spectralAberration(uScene, uv, caDir, caAmount);

    vec3 blurredScene = zoomBlurSample(uScene, uv, vec2(0.5), zoomStrength);
    vec3 scene = mix(sharpScene, blurredScene, zoomStrength * 0.85);

    // Depth of field, background/particles only: the donut and its bolts
    // write a negative sentinel depth and are never blurred here at all
    // (trying to auto-track focus onto a complex, always-moving subject
    // kept breaking in new ways, so it's simpler and far more robust to
    // just always keep the hero elements sharp). Everything else compares
    // against a fixed reference distance roughly matching the camera-to-
    // donut range, rather than a dynamically detected one.
    // Anything at or nearer than the focus plane stays fully sharp (no
    // re-blur as it gets closer) — only things farther out gradually blur.
    // The previous symmetric falloff (abs(depth - focus)) blurred very-close
    // foreground the same as very-far background, fighting the intended
    // "nearer to camera/screen edge = clearer" gradient.
    const float kFocusDepth = 4.0;
    float pixelDepth = texture(uScene, uv).a;
    float distPastFocus = max(0.0, pixelDepth - kFocusDepth);
    float coc = pixelDepth < 0.0 ? 0.0 : smoothstep(0.0, 6.0, distPastFocus);
    scene = mix(scene, texture(uDofBlur, uv).rgb, coc);

    vec3 bloom = texture(uBloom, uv).rgb;

    vec3 col = scene + bloom * 0.35;

    float vignette = smoothstep(0.9, 0.25, distFromCenter);
    col *= mix(0.55, 1.0, vignette);

    // Pre-tonemap so the fade genuinely bottoms out at black rather than
    // ACES rolling a dimmed-but-still-colorful signal back up.
    col *= (1.0 - blackout);

    col = acesFilmic(col * 0.85 * uExposure);
    col = pow(col, vec3(1.0 / 2.2));

    float grain = (hash21(gl_FragCoord.xy + fract(iTime) * 100.0) - 0.5) * 0.035;
    col += grain;

    // radialSpectrum() is kept for a future spectrum-ring preset, not used here.

    fragColor = vec4(clamp(col, 0.0, 1.0), 1.0);
}
