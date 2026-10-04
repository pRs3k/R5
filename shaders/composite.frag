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

// Look controls, driven from Settings > Look (main.cpp's LookSettings holds
// the defaults). Every soft/hazy post effect is scaled by one of these so
// the overall crispness can be tuned live instead of by editing constants.
uniform float uDofAmount;    // 0 = no depth of field, 1 = full background blur
uniform float uAberration;   // lens colour fringe strength (edges only)
uniform float uBloomStrength;
uniform float uGrain;
uniform float uSharpen;      // post-downsample sharpening, 0 = off
uniform float uSaturation;   // 1 = neutral

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

// Lens colour fringe: one tap per channel (red pushed outward, blue pulled
// inward, green untouched). This used to be an 8-tap spectral march, which
// looked smooth in isolation but was really a radial blur -- every pixel
// was the average of 8 samples spread along the radius, up to ~25px apart
// near the edges on beats. Three distinct taps give a crisp fringe on
// contrasty edges and leave flat areas and fine detail untouched.
vec3 lensFringe(sampler2D tex, vec2 uv, vec2 dir, float amount, vec3 center) {
    if (amount < 1e-5) return center;
    return vec3(texture(tex, uv + dir * amount).r,
                center.g,
                texture(tex, uv - dir * amount).b);
}

// Contrast-adaptive sharpen (in the spirit of AMD's CAS) on the
// downsampled scene: pushes each pixel away from its 4 neighbours, then
// clamps to their min/max so bright HDR edges can't ring or halo. The
// supersampled scene is minified with trilinear filtering on the way to
// native resolution, which softens a little; this wins that back.
vec3 sharpenedScene(vec2 uv, vec2 px) {
    vec3 center = texture(uScene, uv).rgb;
    if (uSharpen <= 0.0) return center;
    vec3 n = texture(uScene, uv + vec2(0.0, px.y)).rgb;
    vec3 s = texture(uScene, uv - vec2(0.0, px.y)).rgb;
    vec3 e = texture(uScene, uv + vec2(px.x, 0.0)).rgb;
    vec3 w = texture(uScene, uv - vec2(px.x, 0.0)).rgb;
    vec3 lo = min(center, min(min(n, s), min(e, w)));
    vec3 hi = max(center, max(max(n, s), max(e, w)));
    vec3 sharp = center + (center - (n + s + e + w) * 0.25) * uSharpen * 2.0;
    return clamp(sharp, lo, hi);
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
    // Only the genuinely huge accents (the top of bigAccent's 0..2 range)
    // zoom-blur now; from 0.6 up it was smearing the whole frame on most
    // ordinary big hits.
    float zoomStrength = smoothstep(1.2, 1.9, bigAccent);

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
    // Fringe only past the inner ~35% radius (none at all over the
    // subject), and an order of magnitude smaller than before: 0.006 UV max
    // (~6px at 1080p, at the far corners, on a hit) vs. 0.02.
    vec2 caDir = distFromCenter > 1e-4 ? centered / distFromCenter : vec2(0.0);
    float caEdge = smoothstep(0.35, 0.8, distFromCenter);
    float caPulse = 0.0016 + iAudio.w * 0.0016 + iAudio.x * 0.0010 + bigAccent * 0.0008;
    float caAmount = min(caPulse * caEdge * uAberration, 0.006);

    vec3 sharpScene = sharpenedScene(uv, 1.0 / iResolution);
    sharpScene = lensFringe(uScene, uv, caDir, caAmount, sharpScene);

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
    float coc = pixelDepth < 0.0 ? 0.0 : smoothstep(0.0, 6.0, distPastFocus) * uDofAmount;
    scene = mix(scene, texture(uDofBlur, uv).rgb, coc);

    vec3 bloom = texture(uBloom, uv).rgb;

    vec3 col = scene + bloom * uBloomStrength;

    // Saturation in linear light, before the tonemap compresses it.
    float luma = dot(col, vec3(0.2126, 0.7152, 0.0722));
    col = max(mix(vec3(luma), col, uSaturation), 0.0);

    float vignette = smoothstep(0.95, 0.3, distFromCenter);
    col *= mix(0.65, 1.0, vignette);

    // Pre-tonemap so the fade genuinely bottoms out at black rather than
    // ACES rolling a dimmed-but-still-colorful signal back up.
    col *= (1.0 - blackout);

    col = acesFilmic(col * 0.85 * uExposure);
    col = pow(col, vec3(1.0 / 2.2));

    float grain = (hash21(gl_FragCoord.xy + fract(iTime) * 100.0) - 0.5) * uGrain;
    col += grain;

    // radialSpectrum() is kept for a future spectrum-ring preset, not used here.

    fragColor = vec4(clamp(col, 0.0, 1.0), 1.0);
}
