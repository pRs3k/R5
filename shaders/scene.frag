#version 410 core

uniform float iTime;
uniform vec2 iResolution;
uniform vec4 iAudio;      // x=bass y=mid z=treble w=onsetPulse (fast, raw)
uniform vec4 iMotion;     // x=travelDistance y=spinAngle z=huePhase w=shakeAmount
uniform vec4 iAudioSlow;  // x=bassSlow y=midSlow z=trebleSlow w=bigAccent
uniform vec3 iShockwave;  // age (seconds) of up to 3 concurrent shockwave rings
uniform float iSpeed;     // current dolly speed, for radial star streaking
uniform float iPan;       // smoothed stereo balance, -1 (left) .. +1 (right)
uniform vec3 uPalette[4]; // active color scheme's 4 stops
uniform float iMorph;     // 0 = donut, 1 = plasma orb; eased host-side, triggered by huge musical moments
uniform float iPortalProgress; // 0..1 through the current/most recent portal fly-through, frozen at 1 once it's done
uniform float iDimension;      // 0 or 1: which world is active — donut/orb, or the tunnel. Hard cut, hidden by the flash.
uniform float iWorldBlend;     // 0 = normal orbiting camera, 1 = locked to the tunnel axis; eases toward iDimension's target
uniform float iTunnelTravel;   // distance flown down the tunnel since the most recent entry (resets each trigger)
uniform float uBackground;     // Settings > Look: brightness of the nebula/gas backdrop (1 = default)
uniform float iOrbitTime;      // eased clock for the normal-mode orbit camera; freezes in lockstep with the axis (see ringRotation) so the frozen-axis "side" pick never flips mid-flight

out vec4 fragColor; // HDR linear, may exceed 1.0 (bloom pass reads this)

mat2 rot(float a) {
    float s = sin(a), c = cos(a);
    return mat2(c, -s, s, c);
}

// Smoothly cycles through all 4 palette stops as t increases, wrapping
// seamlessly (t is expected to already be a naturally-periodic value, e.g.
// an angle/2pi or an fbm/hash output in roughly 0..1 — any real value
// works since this fracts internally).
vec3 paletteColor(float t) {
    float scaled = fract(t) * 4.0;
    int i0 = int(floor(scaled)) % 4;
    int i1 = (i0 + 1) % 4;
    float f = smoothstep(0.0, 1.0, fract(scaled));
    return mix(uPalette[i0], uPalette[i1], f);
}

float hash21(vec2 p) {
    p = fract(p * vec2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return fract(p.x * p.y);
}

float hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float valueNoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    float a = hash12(i);
    float b = hash12(i + vec2(1.0, 0.0));
    float c = hash12(i + vec2(0.0, 1.0));
    float d = hash12(i + vec2(1.0, 1.0));
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(a, b, u.x) + (c - a) * u.y * (1.0 - u.x) + (d - b) * u.x * u.y;
}

float fbm(vec2 p) {
    float sum = 0.0;
    float amp = 0.5;
    for (int i = 0; i < 4; ++i) {
        sum += amp * valueNoise(p);
        p *= 2.02;
        amp *= 0.5;
    }
    return sum;
}

float hash13(vec3 p) {
    p = fract(p * vec3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yzx + 33.33);
    return fract((p.x + p.y) * p.z);
}

float valueNoise3(vec3 p) {
    vec3 i = floor(p);
    vec3 f = fract(p);
    vec3 u = f * f * (3.0 - 2.0 * f);
    float n000 = hash13(i + vec3(0.0, 0.0, 0.0));
    float n100 = hash13(i + vec3(1.0, 0.0, 0.0));
    float n010 = hash13(i + vec3(0.0, 1.0, 0.0));
    float n110 = hash13(i + vec3(1.0, 1.0, 0.0));
    float n001 = hash13(i + vec3(0.0, 0.0, 1.0));
    float n101 = hash13(i + vec3(1.0, 0.0, 1.0));
    float n011 = hash13(i + vec3(0.0, 1.0, 1.0));
    float n111 = hash13(i + vec3(1.0, 1.0, 1.0));
    float nx00 = mix(n000, n100, u.x);
    float nx10 = mix(n010, n110, u.x);
    float nx01 = mix(n001, n101, u.x);
    float nx11 = mix(n011, n111, u.x);
    float nxy0 = mix(nx00, nx10, u.y);
    float nxy1 = mix(nx01, nx11, u.y);
    return mix(nxy0, nxy1, u.z);
}

float fbm3(vec3 p) {
    float sum = 0.0;
    float amp = 0.5;
    for (int i = 0; i < 4; ++i) {
        sum += amp * valueNoise3(p);
        p *= 2.02;
        amp *= 0.5;
    }
    return sum;
}

// Thin branching veins evaluated directly on the 3D surface position (not
// an angle/height parameterization), so there's no seam or pole to collect
// at. Domain-warped for organic branching, stepped in time so veins arc
// rather than sweep. Returns a sharp crack layer plus a softer glow layer.
float electricVeins(vec3 p, float flickerRate, out float glow) {
    float tStep = floor(iTime * flickerRate) / flickerRate;
    vec3 wp = p * 2.4;
    vec3 warp = vec3(
        fbm3(wp + tStep * 0.6),
        fbm3(wp + tStep * 0.6 + 11.0),
        fbm3(wp + tStep * 0.6 + 23.0)
    ) - 0.5;
    wp += warp * 1.3;

    float n1 = fbm3(wp + tStep * 1.3);
    float ridgeRaw = clamp(1.0 - abs(n1 - 0.5) * 2.0, 0.0, 1.0);
    float veinSharp = pow(ridgeRaw, 14.0);
    glow = pow(ridgeRaw, 4.0);

    float n2 = fbm3(wp * 2.1 - tStep * 0.8 + 5.0);
    float ridge2 = pow(clamp(1.0 - abs(n2 - 0.5) * 2.0, 0.0, 1.0), 16.0);

    return clamp(veinSharp + ridge2 * 0.8, 0.0, 1.0);
}

float sdSegment(vec2 p, vec2 a, vec2 b) {
    vec2 pa = p - a, ba = b - a;
    float h = clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0);
    return length(pa - ba * h);
}

// Undoes mapDist's p.xz/p.xy rotations (inverse order) so a point defined
// in the torus's own canonical frame lands at its true current world
// position — needed to anchor bolts exactly on the live (spinning,
// pulsing) surface instead of a fixed screen-space approximation.
vec3 canonicalToWorld(vec3 pc) {
    pc.xy *= rot(-iMotion.y * 0.6);
    pc.xz *= rot(-iMotion.y);
    return pc;
}

// Seamless (atan2-based) angle of p around an arbitrary axis through
// axisPoint — the tunnel's equivalent of the torus's majorAngle, needed
// because the tunnel isn't necessarily aligned to a world axis (it's
// whatever direction the donut's hole was facing when frozen). Builds an
// arbitrary-but-consistent pair of basis vectors perpendicular to axisDir,
// guarding the case where axisDir is itself near-vertical (a naive
// cross(axisDir, vec3(0,1,0)) degenerates there).
float angleAroundAxis(vec3 p, vec3 axisPoint, vec3 axisDir) {
    vec3 refUp = abs(axisDir.y) > 0.99 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0);
    vec3 basisA = normalize(cross(axisDir, refUp));
    vec3 basisB = cross(axisDir, basisA);
    vec3 rel = p - axisPoint;
    vec3 radial = rel - axisDir * dot(rel, axisDir);
    return atan(dot(radial, basisB), dot(radial, basisA));
}


// Projects into the given camera's local space first (so it stays correct
// under the orbiting camera, not just the old fixed straight-on one).
vec2 projectToScreen(vec3 worldPoint, vec3 camPos, vec3 fwd, vec3 right, vec3 up) {
    vec3 dir = worldPoint - camPos;
    vec3 local = vec3(dot(dir, right), dot(dir, up), dot(dir, fwd));
    return local.xy / max(local.z, 0.05);
}

// Draws one thin, long path via a genuine random walk (each segment turns
// by an irregular angle, biased back toward the target so it still gets
// there) rather than a symmetric zigzag off a straight line — closer to
// how a real lightning channel wanders. Occasional thinner forward-leaning
// forks branch off, in the same visual language as the surface veins.
float boltPath(vec2 uv, vec2 origin, vec2 end, float seed, float energy, out float core) {
    vec2 mainDir = normalize(end - origin);
    float totalLen = length(end - origin);
    const int kSegs = 14;
    float segLen = totalLen / float(kSegs);

    vec2 pos = origin;
    vec2 dir = mainDir;
    float dist = 1e9;

    for (int s = 0; s < kSegs; ++s) {
        float fs = float(s) / float(kSegs);
        float h = hash21(vec2(seed * 13.0 + fs * 91.0, 2.0));
        float turnAngle = (h - 0.5) * 1.3;
        dir = normalize(rot(turnAngle) * dir + mainDir * 0.35);
        vec2 nextPoint = pos + dir * segLen;
        dist = min(dist, sdSegment(uv, pos, nextPoint));

        // Occasional short forward-leaning fork, thinner and dimmer.
        if (h > 0.78) {
            float forkAngle = (hash21(vec2(seed, fs * 29.0)) - 0.5) * 1.6;
            vec2 forkDir = normalize(rot(forkAngle) * dir);
            float forkLen = segLen * mix(1.5, 3.0, hash21(vec2(seed, fs * 41.0)));
            vec2 forkEnd = nextPoint + forkDir * forkLen;
            dist = min(dist, sdSegment(uv, nextPoint, forkEnd) * 2.2);
        }

        pos = nextPoint;
    }

    float thickness = 0.0011 + 0.0014 * energy;
    core = smoothstep(thickness, 0.0, dist);
    return smoothstep(thickness * 2.0, 0.0, dist);
}

// Shared by mapDist and boltField so the bolts' attachment radius always
// matches the body they're actually anchored to mid-morph.
float orbRadius(float bassSlow) {
    return 0.62 + bassSlow * 0.15;
}

// Small lightning bolts periodically discharging outward from the donut's
// actual (rotating, pulsing) surface, anchored via true 3D projection so
// they visibly touch it. beatEnergy scales how many are active and how
// bright/long they are. Also reports the anchor depth of whichever bolt
// dominates this pixel, so depth-of-field treats the whole bolt as part of
// the (in-focus) donut instead of blurring it by screen position — and
// draws a genuine directional streak (widening from origin to tip) for an
// intentional "shooting outward" look, rather than that being a side
// effect of the depth blur like before.
vec3 boltField(vec2 uv, vec3 ro, vec3 fwd, vec3 right, vec3 up, float beatEnergy, out float dominantDepth, out float geomMask) {
    vec3 col = vec3(0.0);
    dominantDepth = 1e9;
    geomMask = 0.0;
    float maxContribution = 0.0;
    float majorR = 1.0 + iAudioSlow.x * 0.25;
    float minorR = 0.35 + iAudioSlow.x * 0.12;

    const int kBolts = 6;
    // Quiet down entirely at rest (no floor count), and cap well below the
    // full slot count even at peak energy, for a calmer, more intentional
    // strike rate instead of near-constant sparkle.
    int activeCount = int(clamp(beatEnergy * 2.5 - 0.2, 0.0, 4.0));

    for (int b = 0; b < kBolts; ++b) {
        if (b >= activeCount) break;
        float fb = float(b);
        float cycle = mix(1.8, 0.8, clamp(beatEnergy, 0.0, 1.0)) + hash21(vec2(fb, 1.0)) * 0.5;
        float phase = mod(iTime + fb * 1.7, cycle) / cycle;
        float seed = floor((iTime + fb * 1.7) / cycle);

        float attack = smoothstep(0.0, 0.08, phase);
        float decay = 1.0 - smoothstep(0.08, 0.32, phase);
        float energy = attack * decay;
        if (energy <= 0.001) continue;

        float majorAngle = hash21(vec2(seed, fb * 3.1)) * 6.28318;
        vec3 canonOut = vec3(cos(majorAngle), 0.0, sin(majorAngle));
        // Blend the attachment radius toward the orb's, so bolts stay
        // anchored to the actual (morphing) surface instead of hovering
        // off a torus that's no longer there.
        float attachR = mix(majorR + minorR, orbRadius(iAudioSlow.x), iMorph);
        vec3 surfWorld = canonicalToWorld(canonOut * attachR);
        vec3 outWorld = canonicalToWorld(canonOut);

        float boltLen3D = mix(1.2, 3.0, hash21(vec2(seed, fb * 7.7))) * (0.7 + beatEnergy * 0.6);
        vec2 origin = projectToScreen(surfWorld, ro, fwd, right, up);
        vec2 end = projectToScreen(surfWorld + outWorld * boltLen3D, ro, fwd, right, up);

        float core;
        float line = boltPath(uv, origin, end, seed + fb * 19.0, energy, core);

        // Hue matches the surface hue right at the attachment point, so
        // the bolt reads as a continuation of the vein pattern, not a
        // separately-colored effect.
        float hueParam = majorAngle / 6.28318 + iMotion.z * 0.15;
        vec3 boltColor = paletteColor(hueParam);

        // No separate attachment glow blob here (tried a circular one, but
        // it read as a distinct small sphere sitting on the surface). The
        // line/core falloff from boltPath already rounds off softly at the
        // origin end of the segment, which fuses cleanly on its own.
        float contribution = (line * 1.1 + core * 2.6) * energy;
        col += contribution * boltColor;

        // Geometric footprint (line/core shape only, not the color
        // brightness weighting above) — used to mark every pixel touched by
        // a bolt as "always sharp", including its dim outer fringe. Relying
        // on color brightness for that left a faint ring that dipped under
        // the threshold and fell back to the blurry background depth.
        geomMask = max(geomMask, max(line, core) * energy);

        if (contribution > maxContribution) {
            maxContribution = contribution;
            dominantDepth = length(surfWorld - ro);
        }
    }
    return col;
}

float sdTorus(vec3 p, vec2 t) {
    vec2 q = vec2(length(p.xz) - t.x, p.y);
    return length(q) - t.y;
}

// The tunnel's world-space axis: whatever direction the donut's hole was
// facing at the moment its spin froze (see iMotion.y's host-side easing).
// Shared by mapDist and main() so the camera's flight path and the actual
// geometry it's flying through are always talking about the same axis.
vec3 tunnelAxis() {
    return normalize(canonicalToWorld(vec3(0.0, 1.0, 0.0)));
}

vec3 tunnelAxisPoint() {
    return vec3(-iPan * 1.1, 0.0, 0.0);
}

// axisDir/axisPoint are passed in rather than recomputed from tunnelAxis()/
// tunnelAxisPoint() here — this is called up to 100 times per pixel from
// the raymarch loop alone, plus 6 more from calcNormal and 5 more from
// calcAO per hit, and tunnelAxis() isn't free (it's two rot() calls, each
// a sin+cos, chained through canonicalToWorld). main() computes the axis
// exactly once per frame and threads it through instead.
float mapDist(vec3 p, vec3 axisDir, vec3 axisPoint, out vec3 glowColor) {
    float bassSlow = iAudioSlow.x;

    // Hard switch, not a blend: a torus/sphere and an open-ended tube are
    // different enough topologically that interpolating between their
    // distance fields (the trick that makes the donut<->orb morph work)
    // would just produce garbage. The cut itself happens exactly when
    // iDimension flips, which is timed to sit under the flash in
    // composite.frag, so it's never actually seen happening.
    if (iDimension > 0.5) {
        vec3 rel = p - axisPoint;
        float alongAxis = dot(rel, axisDir);
        vec3 radial = rel - axisDir * alongAxis;
        float radius = 1.0 + bassSlow * 0.15;
        float d = radius - length(radial);

        // Cheap color here on purpose: the raymarch loop calls mapDist up
        // to 100 times per pixel just for this soft glow accumulation, so
        // the full kaleidoscope fold (a cross + normalize + atan on top of
        // this) would pay its cost that many times over for a
        // contribution that's deliberately blurry anyway. The real fold
        // is applied once, in the hit-shading block below, where it's
        // only ever paid for once per pixel.
        glowColor = paletteColor(alongAxis * 0.1 + iMotion.z * 0.4);
        return d;
    }

    p.xz *= rot(iMotion.y);
    p.xy *= rot(iMotion.y * 0.6);

    float majorR = 1.0 + bassSlow * 0.25;
    float minorR = 0.35 + bassSlow * 0.12;
    float dTorus = sdTorus(p, vec2(majorR, minorR));
    // Linear SDF blend: not a geometrically "true" morph, but torus and
    // sphere are both convex-ish blobs, so interpolating the distance
    // fields directly reads as the hole smoothly closing into a sphere
    // rather than producing self-intersecting garbage — the classic cheap
    // raymarched-morph trick.
    float dOrb = length(p) - orbRadius(bassSlow);
    float d = mix(dTorus, dOrb, iMorph);

    // Hue driven by a proper wrapping angle (not a linear coordinate like
    // p.x, which cuts the donut into two flat-colored halves with a hard
    // seam) so the palette blends continuously all the way around. Only
    // an integer multiple of an atan2 angle stays seamless across its
    // branch cut, so no stray fractional coefficients here. Works
    // unchanged as a plain azimuthal angle on the orb too.
    float majorAngle = atan(p.z, p.x);
    float hue = majorAngle / 6.28318 + iMotion.z * 0.15;
    glowColor = paletteColor(hue);
    return d;
}

// Tetrahedron-offset gradient estimate: 4 mapDist taps instead of the
// naive 6-tap central difference, same approximation quality. Used to
// cost little when calcNormal was only ever reached for the donut's
// silhouette; the room's box+floating-object union has no cheap analytic
// normal the way the cylinder does, so calcNormal now runs on most of
// the visible screen there too, and the extra 2 taps stopped being free.
vec3 calcNormal(vec3 p, vec3 axisDir, vec3 axisPoint) {
    vec3 dummy;
    const float h = 1e-3;
    const vec2 k = vec2(1.0, -1.0);
    return normalize(
        k.xyy * mapDist(p + k.xyy * h, axisDir, axisPoint, dummy) +
        k.yyx * mapDist(p + k.yyx * h, axisDir, axisPoint, dummy) +
        k.yxy * mapDist(p + k.yxy * h, axisDir, axisPoint, dummy) +
        k.xxx * mapDist(p + k.xxx * h, axisDir, axisPoint, dummy)
    );
}

float calcAO(vec3 p, vec3 n, vec3 axisDir, vec3 axisPoint) {
    float occ = 0.0;
    float weight = 1.0;
    vec3 dummy;
    for (int i = 0; i < 5; ++i) {
        float dist = 0.02 + 0.10 * float(i);
        float d = mapDist(p + n * dist, axisDir, axisPoint, dummy);
        occ += (dist - d) * weight;
        weight *= 0.6;
    }
    return clamp(1.0 - occ * 1.5, 0.0, 1.0);
}

// Soft shadow via the standard "penumbra from the tightest cone the ray
// could have snuck through" trick: at each step, how much room d leaves
// on either side of the ray relative to distance traveled bounds how wide
// a light source could still graze past the nearest obstruction, so the
// running minimum of k*d/t (not just whether d ever hits 0) is what gives
// a soft-edged shadow instead of a binary one. This is what actually
// grounds the geometry -- everything up to now glowed evenly regardless
// of what was around it, which is a big part of why it read as flat/toy-
// like rather than physically there. ro is expected to already be offset
// off the surface along its normal (the caller's job, since the right
// bias depends on the caller's own hitEps).
// maxStep matters a lot more here than it first looks like it should: the
// SDF's own distance already guarantees a safe step, so capping it small
// is purely about not skipping past a thin occluder in one leap, not
// about correctness. Left at a donut-appropriate ~0.5 unit cap, the room
// (a ~60-unit box with occupied cells spaced 16-34 units apart, so mostly
// open space) burned through every iteration just crossing empty space
// before ever reaching maxT -- this is what made the room's shadow cost
// roughly 4x the donut's despite being the exact same iteration count.
float softShadow(vec3 ro, vec3 rd, float maxT, float k, float maxStep, vec3 axisDir, vec3 axisPoint) {
    float res = 1.0;
    float t = 0.02;
    vec3 dummy;
    for (int i = 0; i < 12; ++i) {
        float d = mapDist(ro + rd * t, axisDir, axisPoint, dummy);
        res = min(res, k * d / t);
        if (res < 0.005 || t > maxT) break;
        t += clamp(d, 0.02, maxStep);
    }
    return clamp(res, 0.0, 1.0);
}

// Soft drifting color clouds behind everything else, for depth. Reads
// iDimension directly (rather than taking it as a parameter) so every
// background layer can pick up the "other side of the portal" look
// uniformly without threading a new argument through every call site.
vec3 nebula(vec2 uv, float travel) {
    // Denser, larger-scale clouds on the other side — thicker atmosphere,
    // less empty space between them, reads as a different kind of place.
    float scale = mix(1.4, 2.4, iDimension);
    vec2 p = uv * scale + vec2(travel * 0.015, sin(travel * 0.05) * 0.2);
    float n = fbm(p + fbm(p * 1.7) * 0.6);
    // Higher threshold plus a squared falloff: clouds keep bright cores
    // but thin out into genuinely black space between them. The old ramp
    // (from 0.35, linear) tinted ~60% of the screen at a fairly even mid
    // level, which is what left no true blacks anywhere in the frame and
    // read as a milky veil over everything. Peak brightness is kept close
    // to before so the clouds still hold their own against the foreground.
    n = smoothstep(mix(0.45, 0.32, iDimension), 0.9, n);
    n *= n;
    vec3 col = paletteColor(fbm(p * 0.6 + 5.0));
    return col * n * mix(0.5, 0.65, iDimension) * uBackground;
}

// Concentric rings receding via a log-polar scroll: the classic stateless
// "flying down a tunnel" cue, layered behind the stars. spacing/speed/
// opacity are exposed so a second, independently-paced layer can be
// stacked on top for parallax depth (see the two call sites in main()).
vec3 tunnelRings(vec2 uv, float travel, float spacing, float speed, float opacity) {
    float r = length(uv) + 1e-4;
    float logR = log(r);
    float rawPhase = logR * spacing - travel * speed;
    float phase = fract(rawPhase);
    float d = min(phase, 1.0 - phase);
    // Antialiased to about one pixel via fwidth rather than a fixed 0.025
    // phase ramp, which smeared each ring wider (in pixels) the farther
    // out it sat, since log-radius compresses toward the edge. fwidth of
    // the unwrapped phase: fract() jumps by 1 exactly on the ring line.
    float aa = fwidth(rawPhase) * 1.5;
    float ring = 1.0 - smoothstep(0.004, 0.004 + aa, d);
    float fadeIn = smoothstep(0.0, 1.5, r);
    float fadeOut = 1.0 - smoothstep(3.0, 5.0, r);
    return paletteColor(logR * 0.3) * ring * fadeIn * fadeOut * opacity;
}

// Patchy galactic dust/gas drifting past — not a uniform haze. Uses actual
// noise for genuine dark lanes (dust occluding what's behind it, applied as
// a multiplicative darken) alongside bright wisps (glowing gas, applied
// additively), so it reads as passing through structured cloud rather than
// washing the whole frame toward white. Returns (glowColor, dustAmount);
// the caller composites both against the existing scene color.
vec4 ambientFog(vec2 uv, float travel, float bassSlow) {
    float r = length(uv);
    vec2 p = uv * 1.6 + vec2(travel * 0.018, travel * 0.007);
    float n = fbm(p + fbm(p * 2.3 + 11.0) * 0.6);
    float edgeFade = smoothstep(1.3, 0.15, r);

    // Same noise field drives both: low values are dust (darken), high
    // values are glowing wisps (brighten) — clumped, with clear gaps of
    // untouched space between clouds instead of coverage everywhere. Glow
    // threshold pushed higher still (rarer, smaller wisps) and its
    // intensity cut well down — this was the main source of the
    // "whitewash" since it's additive brightening stacked on top of
    // everything else already in frame.
    // More and denser wisps/dust on the other side of a portal — a
    // thicker, more active atmosphere rather than sparse empty space.
    float glow = smoothstep(mix(0.7, 0.5, iDimension), 0.94, n) * edgeFade;
    float dust = smoothstep(mix(0.42, 0.55, iDimension), 0.12, n) * edgeFade;

    vec3 glowColor = paletteColor(travel * 0.01 + n * 0.5) * glow * (mix(0.3, 0.48, iDimension) + bassSlow * 0.25) * uBackground;
    float dustAmount = dust * 0.55;
    return vec4(glowColor, dustAmount);
}

// Log-polar starfield: scrolling the log-radius coordinate with travel
// distance makes stars radiate outward and accelerate as they "pass" the
// camera. Stars stretch radially with speed for a warp-streak feel.
vec3 starField(vec2 uv, float travel, float speed) {
    float r = length(uv) + 1e-4;
    float a = atan(uv.y, uv.x);
    float logR = log(r);
    vec2 st = vec2(a / 6.28318, logR * 0.55 - travel * 0.35);

    float speedNorm = clamp((speed - 1.7) / 3.5, 0.0, 1.0);
    float streak = 1.0 + speedNorm * 4.0;

    vec3 col = vec3(0.0);
    for (int i = 0; i < 3; ++i) {
        float fi = float(i);
        vec2 guv = st * vec2(28.0, 9.0) + vec2(fi * 13.0, fi * 29.0);
        vec2 id = floor(guv);
        vec2 gv = fract(guv) - 0.5;
        float h = hash21(id + fi * 7.0);
        // Denser field on the other side — feels like moving through a
        // thicker star cluster rather than sparse open space.
        if (h > mix(0.88, 0.76, iDimension)) {
            float colorMix = hash21(id + fi * 7.0 + 3.7);
            float distMetric = length(vec2(gv.x, gv.y / streak));
            // Sharp core plus a faint short glow, rather than one soft blob
            // 22% of a cell wide: cells grow with radius (log-polar), so
            // near the screen edge that blob was a big out-of-focus oval.
            float star = smoothstep(0.09, 0.03, distMetric) * 1.3 + exp(-distMetric * 14.0) * 0.2;
            float twinkle = 0.6 + 0.4 * sin(h * 250.0 + iTime * 3.0);
            float fadeIn = smoothstep(0.0, 2.0, r);
            col += star * twinkle * fadeIn * paletteColor(colorMix) * 1.3;
        }
    }
    return col;
}

// Cheap point-sprite particles: analytic closest-approach glow, no marching.
// Orbits are tilted per-particle for variety and spin with the donut.
// Also reports the real depth of whichever particle dominates this pixel
// (brightest contributor), so depth-of-field can use its true distance
// from camera instead of a screen-space guess — particles genuinely have
// 3D positions, unlike the purely-decorative background layers.
vec3 particleField(vec3 ro, vec3 rd, float trebleSlow, out float closestDepth) {
    vec3 glow = vec3(0.0);
    closestDepth = 1e9;
    float maxBrightness = 0.0;
    const int kCount = 14;
    for (int i = 0; i < kCount; ++i) {
        float fi = float(i);
        float h = hash21(vec2(fi, 3.17));
        float h2 = hash21(vec2(fi, 7.71));
        float h3 = hash21(vec2(fi, 11.3));

        float orbitR = 1.55 + h * 1.15 + iAudioSlow.x * 0.3;
        float speed = 0.5 + h2 * 0.7;
        float ang = iMotion.y * speed + h * 6.28318;

        vec3 pos = vec3(cos(ang) * orbitR, sin(ang * 2.0 + h3 * 6.0) * 0.35, sin(ang) * orbitR);
        pos.xy *= rot(h2 * 6.28318);
        pos.yz *= rot(h3 * 6.28318);

        vec3 toParticle = pos - ro;
        float tClosest = max(dot(toParticle, rd), 0.0);
        vec3 closest = ro + rd * tClosest;
        float d = length(closest - pos);

        float twinkle = 0.5 + 0.5 * sin(iTime * (2.0 + h * 4.0) + h2 * 30.0);
        // Tighter core (same peak brightness, ~4x less spread) than before.
        // The previous falloff was so soft that particles already looked
        // like diffuse blobs when perfectly in focus, so depth-of-field
        // blurring them further was barely visible — unlike the thin ring
        // lines, which go from crisp to mushy very obviously. A harder core
        // gives DOF something visibly sharp to blur away from.
        float brightness = (0.00045 / (0.00015 + d * d)) * (0.4 + trebleSlow * 1.5) * twinkle;
        glow += brightness * paletteColor(h);

        if (brightness > maxBrightness) {
            maxBrightness = brightness;
            closestDepth = tClosest;
        }
    }
    return glow;
}

// Beat-synced expanding rings in screen space, centered on the donut.
vec3 shockwaves(vec2 uv, vec3 ages) {
    vec3 col = vec3(0.0);
    float agesArr[3] = float[3](ages.x, ages.y, ages.z);
    for (int i = 0; i < 3; ++i) {
        float age = agesArr[i];
        if (age > 1.4) continue;
        float radius = age * 1.9;
        float d = abs(length(uv) - radius);
        // Thin bright core (a few pixels, antialiased) plus a short, dim
        // exponential glow. This used to be one soft band up to 0.05 uv
        // wide -- ~35px at 720p, as wide as the donut's tube -- which was
        // the big blurry ring sweeping across the frame on every beat.
        float px = 1.0 / iResolution.y;
        float coreW = mix(3.0, 1.2, clamp(age / 1.4, 0.0, 1.0)) * px;
        float core = 1.0 - smoothstep(coreW, coreW + 1.5 * px, d);
        float halo = exp(-d / (coreW * 4.0)) * 0.25;
        float fade = 1.0 - age / 1.4;
        col += (core + halo) * fade * fade * paletteColor(clamp(age / 1.4, 0.0, 1.0)) * 1.6;
    }
    return col;
}

void main() {
    vec2 uv = (gl_FragCoord.xy - 0.5 * iResolution.xy) / iResolution.y;
    float treb = iAudio.z, onset = iAudio.w;
    float mid = iAudio.y, midSlow = iAudioSlow.y;
    float bigAccent = iAudioSlow.w;
    // Bolts (count, strike rate, length, vein flicker) track the mid band —
    // vocals/lead/snare, the part of a track a listener's ear locks onto —
    // rather than a generic broadband onset, so the electricity visibly
    // breathes with the music instead of just sparkling at a constant rate.
    // Onset/bigAccent stay in for a punchy kick right on hard transients.
    float beatEnergy = clamp(mid * 1.1 + midSlow * 0.5 + onset * 0.5 + bigAccent * 1.2, 0.0, 1.8);

    // Slow cinematic orbit + dolly-breathing + vertical drift around the
    // donut, framed a bit further back, plus a brief shake kick reserved
    // for genuinely exceptional accents. The donut's left/right position
    // follows the stereo mix (camera aim shifts opposite the desired
    // on-screen shift). Everything else (bolt/particle anchoring) projects
    // through this same camera basis so it stays correct as it moves.
    float orbitAngle = iOrbitTime * 0.035;
    float orbitTilt = 0.18 + sin(iOrbitTime * 0.017) * 0.22;
    // Eased in toward the orb (roughly half the torus's outer radius) so
    // the "condensing into a small, dense core" reads as deliberate
    // framing rather than the subject just shrinking in an unchanged shot.
    float orbitRadius = mix(4.3, 3.5, iMorph) + sin(iOrbitTime * 0.011) * 0.4;

    vec3 normalCamPos = vec3(sin(orbitAngle) * orbitRadius,
                              orbitTilt * orbitRadius * 0.35,
                              -cos(orbitAngle) * orbitRadius);
    vec3 camTarget = tunnelAxisPoint();
    vec3 normalFwd = normalize(camTarget - normalCamPos);

    // Portal fly-through: rather than trying to precisely thread the
    // camera through the hole while the torus is still tumbling (fragile
    // — same class of problem as the old DOF focus-tracking), the donut's
    // own spin is what eases to a stop instead (see iMotion.y's host-side
    // easing), captured via tunnelAxis() at whatever orientation it settles
    // on. Once that's frozen, "through the hole" is just "along a fixed
    // axis", which is easy and robust to fly a camera down for real. This
    // axis-locked path is blended in via iWorldBlend (eased host-side
    // toward iDimension's target) rather than snapping, so the camera
    // visibly swings around to line up with the hole before committing.
    vec3 axisDir = tunnelAxis();
    // Perpendicular pair the roller-coaster camera tilt (below) rides on.
    vec3 axisUpRef = abs(axisDir.y) > 0.99 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0);
    vec3 basisA = normalize(cross(axisDir, axisUpRef));
    vec3 basisB = cross(axisDir, basisA);
    // Whichever side of the hole the camera already happens to be on, so
    // lining up swings the shorter way instead of ever crossing through
    // the donut to approach from "behind" it.
    float side = sign(dot(normalCamPos - camTarget, axisDir));
    if (side == 0.0) side = 1.0;

    float travel = iTunnelTravel;
    vec3 axisCamPos = camTarget + axisDir * side * (2.6 - travel);
    vec3 axisFwd = -axisDir * side;

    // Roller-coaster camera dynamics: the tube itself stays the simple
    // straight cylinder — walls never move — but the camera pitches,
    // yaws, and rolls gently as it flies down it, so the ride reads as
    // a slow, swaying banking motion rather than the tunnel's actual
    // shape changing. Keyed off distance traveled (not just time) so the
    // sway feels like a real, repeatable track rather than a wobble
    // that'd keep going even if the camera stopped; mixed frequencies
    // plus a slower time term keep it from ever reading as a simple
    // back-and-forth loop. Bass drives how hard it banks, mid drives the
    // pitch/yaw swing. Low frequencies and modest amplitudes throughout
    // — this reads as gentle swaying, not a ride that whips around.
    float rollAngle = sin(travel * 0.18 + iTime * 0.15) * mix(0.15, 0.35, iAudioSlow.x)
                     + sin(travel * 0.31 - iTime * 0.22) * 0.1;
    // Yaw/pitch stay far more modest than roll: unlike roll (which only
    // rotates the screen, not the view direction), tilting where the
    // camera actually looks pushes more of the tunnel wall toward grazing
    // angles the fresnel/reflection terms weren't tuned for, and reads as
    // a whitewash well before the tilt itself looks dramatic.
    float pitchAngle = cos(travel * 0.13 + iTime * 0.11) * mix(0.025, 0.06, iAudioSlow.y)
                      + cos(travel * 0.22 - iTime * 0.17) * 0.018;
    float yawAngle = sin(travel * 0.15 - iTime * 0.13) * mix(0.03, 0.07, iAudioSlow.y)
                    + sin(travel * 0.26 + iTime * 0.19) * 0.022;
    vec3 tiltedAxisFwd = normalize(axisFwd + basisA * sin(yawAngle) + basisB * sin(pitchAngle));

    float worldBlend = clamp(iWorldBlend, 0.0, 1.0);
    vec3 camPos = mix(normalCamPos, axisCamPos, worldBlend);
    vec3 fwd = normalize(mix(normalFwd, tiltedAxisFwd, worldBlend));

    // Dive into the hole on the way in: the donut/orb is still what's
    // rendering (iDimension < 0.5) right up until the geometry swap, so
    // this window is entry-only — exit's pre-cut stretch already has
    // iDimension == 1 (tunnel), never triggering this. Spans nearly the
    // whole alignment swing (worldBlend 0.15→0.95, not just its tail) --
    // a first version only kicked in over the back half and turned out to
    // be too narrow a slice of real time (the whole pre-cut swing is only
    // ~2.5s) to read as a real dolly rather than a last-instant snap.
    // Pushed most of the way to camTarget (not all the way, to keep the
    // math from degenerating) so the hole grows to fill the screen right
    // as composite.frag's matching portalBlackout hits full black, and
    // the cut itself happens entirely inside that darkness.
    //
    // fwd is also forced toward axisFwd (dead-on down the hole's axis,
    // not tiltedAxisFwd's own yaw/pitch sway) over the same window --
    // camPos alone closing in on camTarget wasn't enough on its own
    // (verified via screenshot): the swing's own fwd blend hadn't
    // necessarily finished converging by the time the dive kicks in, so
    // the camera could still be very close but looking a little off-axis,
    // which reads as the donut's tube surface looming into view rather
    // than open dark hole.
    if (iDimension < 0.5) {
        float diveIn = smoothstep(0.15, 0.95, worldBlend);
        camPos = mix(camPos, camTarget, diveIn * 0.97);
        fwd = normalize(mix(fwd, axisFwd, diveIn));
    }
    // Guards the case where fwd ends up nearly parallel to the usual
    // world-up reference (very possible here, since the hole's frozen
    // axis can point anywhere) — cross(fwd, worldUp) would otherwise
    // collapse toward zero length and normalize() would blow up.
    vec3 upRef = abs(fwd.y) > 0.99 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0);
    vec3 right = normalize(cross(fwd, upRef));
    vec3 up = cross(right, fwd);
    // Roll doesn't change where the camera looks, only how "up" tilts
    // relative to it — the banking-into-a-turn part of the coaster feel.
    // Scaled by worldBlend so it's fully off in normal donut-orbit mode.
    float effectiveRoll = rollAngle * worldBlend;
    float rollCos = cos(effectiveRoll), rollSin = sin(effectiveRoll);
    vec3 rolledRight = right * rollCos + up * rollSin;
    vec3 rolledUp = up * rollCos - right * rollSin;
    right = rolledRight;
    up = rolledUp;

    vec2 shakeUv = uv + vec2(sin(iTime * 41.0), cos(iTime * 37.0)) * iMotion.w * 0.01;

    vec3 ro = camPos;
    vec3 rd = normalize(fwd + shakeUv.x * right + shakeUv.y * up);

    // Hoisted above the raymarch loop (was declared down in the hit-shading
    // block) so the volumetric scattering below can use the same light
    // direction the surface shading does.
    vec3 lightDir = normalize(vec3(0.6, 0.7, -0.5));

    float t = 0.0;
    vec3 glowAccum = vec3(0.0);
    vec3 hitGlowColor = vec3(0.0);
    bool hit = false;
    // Volumetric light scattering: in-scattered light + transmittance
    // accumulated along the SAME steps the primary raymarch already
    // takes, rather than a separate march -- an extra shadow-ray pass
    // per step would double the cost of what's already the frame's most
    // expensive loop. Unshadowed by the actual geometry (no occlusion
    // test against the donut/tunnel wall) -- a stylized approximation,
    // not a physically exact one, but the phase function alone (peaking
    // when the view ray points toward the light) is what actually sells
    // "shaft of light", and that doesn't need real occlusion to read.
    // Density is a cheap analytic falloff plus a single sin wobble, not
    // true 3D noise -- this runs up to 100 times per pixel, and the
    // room's domain-repetition already taught how fast an extra per-step
    // cost compounds here.
    vec3 volumetric = vec3(0.0);
    float transmittance = 1.0;

    // Tunnel-mode tuning: rays skimming near-tangent to a wall that
    // surrounds the camera on all sides converge far slower than typical
    // rays past/around a small compact object like the donut, which is
    // most of why the tunnel cost more per frame than the scene it
    // replaced. A looser hit epsilon and a mild over-relaxed step (both
    // safe for one smooth, constant-curvature surface — no thin features
    // to tunnel through) cut the average iteration count without visible
    // quality loss. maxDist stays generous (not shortened the way it
    // once was) specifically for rays near dead-center: a ray only
    // θ radians off the tube's exact axis needs roughly radius/θ of
    // travel to drift into a wall, so cutting maxDist short left a
    // visible cone straight ahead that never converged — background
    // showing through where a wall should be, reading as "the end of
    // the tunnel" instead of an endless corridor. The 100-iteration cap
    // below bounds the actual cost regardless of this value.
    float hitEps = mix(1e-4, 1.2e-3, iDimension);
    float stepScale = mix(1.0, 1.35, iDimension);
    float maxDist = mix(20.0, 70.0, iDimension);
    for (int i = 0; i < 100; ++i) {
        vec3 p = ro + rd * t;
        vec3 gcol;
        float d = mapDist(p, axisDir, camTarget, gcol);
        glowAccum += (0.006 / (0.04 + d * d)) * gcol * 0.006;

        if (d < hitEps) { hit = true; hitGlowColor = gcol; break; }
        t += d * stepScale;
        if (t > maxDist) break;
    }

    // Volumetric fog: a second, fixed-step pass over [0, t] now that the
    // primary march above has settled on how far this ray actually
    // reaches (t, capped at maxDist if it missed everything) -- sampling
    // a fixed count of evenly-spaced points along that known distance,
    // rather than accumulating inline on the primary march's own steps
    // (an earlier version did that). That first version still read as
    // faintly grainy after removing an earlier per-step position+time
    // wobble: even a perfectly smooth density function gets integrated
    // over a DIFFERENT NUMBER of steps for adjacent pixels when reusing
    // the primary march's adaptive stepping, since their rays converge
    // (or reach maxDist) after different numbers of iterations -- that
    // step-count difference alone shows up as visible per-pixel noise
    // once multiplied through transmittance many times over. A fixed
    // count sidesteps that by construction. Doesn't touch mapDist at all
    // -- density below is a pure analytic function of position -- so this
    // is cheap regardless of what the primary march itself cost.
    {
        float pulse = 0.92 + 0.08 * sin(iTime * 0.3);
        // Forward-scattering phase function: brightest when this ray
        // happens to be looking roughly toward the light. No ambient
        // floor -- an earlier version added one unconditionally so a
        // shaft was easier to catch on camera, but that meant every
        // single ray accumulated fog regardless of direction, which is
        // what actually produced a constant wash over the whole scene:
        // the "shaft" read as omnipresent haze instead of a directional
        // highlight. Better for this to be honestly subtle/rare than
        // reliably wrong.
        float phase = pow(max(dot(rd, lightDir), 0.0), 4.0) * 5.0;
        const int kVolSteps = 16;
        float volStepLen = t / float(kVolSteps);
        for (int i = 0; i < kVolSteps; ++i) {
            vec3 p = ro + rd * (volStepLen * (float(i) + 0.5));
            // Denser near the donut/orb (falls off with distance from
            // camTarget, the classic "haze pooled around the subject"
            // look); a low near-constant haze in the tunnel instead,
            // since nothing there is "the subject" for fog to pool
            // around -- distance from an arbitrary point wouldn't mean
            // anything.
            float density = iDimension > 0.5
                ? 0.012 * pulse
                : exp(-length(p - camTarget) * 0.22) * 0.11 * pulse;
            volumetric += transmittance * density * phase * volStepLen;
            transmittance *= exp(-density * volStepLen * 0.35);
        }
    }

    // Both fade out entirely by worldBlend (bolts/particles read as this
    // creature's own electrical activity — they'd look like debris once
    // there's no creature there to be attached to), but without this
    // guard the FUNCTIONS still ran in full every frame regardless —
    // particleField's 14-particle loop and boltField's up-to-6 random-walk
    // paths (14 segments each) — for a contribution that gets multiplied
    // down to nothing. Skipping the calls once they're negligible is free
    // performance with zero visual difference, worth the most exactly
    // when deep in the tunnel where the framerate was struggling.
    float particleDepth = 1e9;
    vec3 particleGlow = vec3(0.0);
    float boltDepth = 1e9;
    float boltMask = 0.0;
    vec3 boltGlow = vec3(0.0);
    if (worldBlend < 0.99) {
        particleGlow = particleField(ro, rd, iAudioSlow.z, particleDepth);
        boltGlow = boltField(uv, ro, fwd, right, up, clamp(beatEnergy, 0.0, 1.0), boltDepth, boltMask);
    }

    // Dust darkens only the distant nebula clouds behind it (genuine
    // occlusion, not just a lighter tint); the glowing-wisp half is added
    // on top of the fully composited background further down, so bright
    // gas patches read clearly against rings/stars instead of being a
    // uniform veil over everything.
    // All of these (nebula/rings/starfield/fog, like bolts/particles
    // below) are screen-space cues keyed to raw uv, centered on the
    // screen's geometric center — fine as a "flying through space" hint
    // behind the donut, but once actually inside the tunnel the camera's
    // own roll/pitch/yaw sway means the tube's real vanishing point
    // drifts off that fixed center, so a ring layer that doesn't isn't
    // reading as "receding into the tunnel", it's reading as visibly not
    // lined up with it. Fading all of it out via worldBlend (full off by
    // the time the geometry cut happens) sidesteps the mismatch entirely
    // — the real tunnel walls carry the "flying through space" feeling
    // once we're actually inside one.
    float bgFade = 1.0 - worldBlend;
    vec4 fog = ambientFog(uv, iMotion.x, iAudioSlow.x);
    vec3 col = nebula(uv, iMotion.x) * (1.0 - fog.a) * bgFade;
    // Two independently-paced ring layers (far: wide/slow/dim, near:
    // tight/fast/dimmer-still) read as parallax depth rather than one flat
    // tunnel — the difference in apparent rate implies distance. Tighter
    // spacing and faster advance on the other side of a portal for a
    // more hyperspace-like tunnel.
    col += tunnelRings(uv, iMotion.x, mix(1.6, 2.4, iDimension), mix(0.3, 0.55, iDimension), 0.5) * bgFade;
    col += tunnelRings(uv, iMotion.x, mix(2.9, 4.4, iDimension), mix(0.62, 1.0, iDimension), 0.24) * bgFade;
    col += starField(uv, iMotion.x, iSpeed) * bgFade;
    // Bolts/particles read as this creature's own electrical activity —
    // they'd look like debris floating in open space once there's no
    // creature there to be attached to, so they fade out over the same
    // approach-and-align window the camera uses to line up with the hole,
    // rather than popping off abruptly at the hard geometry cut.
    col += particleGlow * (1.0 - worldBlend);
    col += fog.rgb * bgFade;
    col += boltGlow * (1.0 - worldBlend);

    // Belt-and-suspenders for the "endless tunnel" fix above: any ray
    // that still doesn't converge (only possible within a sliver of a
    // degree of dead-center even with the generous maxDist) should read
    // as the corridor receding into darkness, not as open-space
    // background — that misread as a wall capping the tunnel.
    if (!hit) {
        col *= mix(1.0, 0.1, iDimension);
    }

    if (hit) {
        vec3 p = ro + rd * t;
        // Analytic normal for the cylinder — exactly the negated radial
        // direction, no finite-difference sampling needed — rather than
        // calcNormal's 6 extra mapDist calls. This matters far more here
        // than it would have for the donut: in the tunnel, the wall fills
        // the whole view, so there's no cheap "missed, background only"
        // path for most pixels the way there was around the donut — this
        // is now the majority-case cost per frame, not a small silhouette.
        vec3 n;
        if (iDimension > 0.5) {
            vec3 rel = p - camTarget;
            vec3 radial = rel - axisDir * dot(rel, axisDir);
            n = -normalize(radial);
        } else {
            n = calcNormal(p, axisDir, camTarget);
        }
        vec3 viewDir = -rd;
        vec3 halfV = normalize(lightDir + viewDir);

        // Electric veins across the surface: evaluated on the actual 3D
        // surface position (domain-warped for organic branching) so there's
        // no seam/pole for charge to visibly collect at. Dim and
        // slow-flickering at rest, arcing brighter and faster on beats.
        // Computed here, ahead of the material response below, because
        // veinGlow now doubles as the roughness driver -- see there.
        float flickerRate = mix(3.0, 16.0, clamp(beatEnergy, 0.0, 1.0));
        float veinGlow;
        float veins = electricVeins(p, flickerRate, veinGlow);
        // Cut hard (was mix(0.22, 1.2, ...)) -- found via a direct debug
        // swatch splitting out spec/veinColor/streakGlow that veinColor
        // specifically was the one actually saturating (reading ~250/255)
        // during an energetic passage, while spec (already cut above) and
        // streakGlow were both moderate. This term alone was carrying the
        // "way too bright" report, not fresnel or spec.
        float veinIntensity = mix(0.15, 0.55, beatEnergy);

        // Physically-motivated material response (Fresnel-Schlick +
        // roughness) rather than the old flat power-curve fresnel and a
        // fixed specular exponent applied uniformly everywhere -- that
        // read as one uniform "wet marble" coat regardless of what was
        // actually being looked at. F0 = 0.04 is a typical dielectric
        // (water/wet-plastic) baseline reflectance at normal incidence,
        // matching the piece's existing glossy look. Roughness is driven
        // by the same crack/vein pattern already being computed for the
        // surface glow above: cracked/veined patches read rougher (a
        // broader, dimmer highlight, less complete grazing brightening --
        // real microfacet self-shadowing, approximated here rather than
        // derived), the smoother marble between reads glossier (tight,
        // bright highlight) -- one physical source for "why does this
        // look interesting" instead of the old shimmer sine hack standing
        // in for it. Tunnel gets a rougher baseline range than the donut:
        // its concave wrap-around geometry means nearly everything in
        // view is at least somewhat grazing, and a donut-like glossy
        // baseline there was exactly what used to whitewash the whole
        // wall (see fresnelPow's old per-dimension split, now folded in
        // here instead).
        // Tunnel range pushed rougher still (was 0.35-0.85) -- reported as
        // "way too bright" even after the kFresnelOverdrive cut below, and
        // fresnel governs most of the tunnel's visible surface (it's
        // concave, so nearly everything reads as at least somewhat
        // grazing), so a glossier-leaning average here was compounding
        // across most of the frame, not just at a few edges.
        float roughness = iDimension > 0.5
            ? clamp(mix(0.55, 0.95, veinGlow), 0.15, 0.98)
            : clamp(mix(0.1, 0.6, veinGlow), 0.05, 0.9);
        // Clamped, not just the max(dot,0) floor below: normalize()'s
        // floating-point error (worst on calcNormal's finite-difference
        // result) can push dot(n,viewDir) a hair past 1.0, which makes this
        // pow()'s base a hair negative. GLSL's pow(negative, x) is
        // undefined — NaN in practice — and that NaN poisons iridescent via
        // the fresnel*0.4 mix factor below, then base/col, then spreads
        // into visible blocky patches once bloom's blur passes average a
        // NaN pixel into its neighbors.
        float cosTheta = clamp(dot(n, viewDir), 0.0, 1.0);
        const float kF0 = 0.04;
        float fresnelSchlick = kF0 + (1.0 - kF0) * pow(1.0 - cosTheta, 5.0);
        float fresnel = fresnelSchlick * mix(1.0, 0.4, roughness);

        // Soft self-shadow against the main light: donut crevices. Skipped
        // in the tunnel entirely -- verified via a raw-shadow debug swatch
        // that it was reading near-zero across virtually the whole wall
        // there, not a bug in the technique but a structural mismatch:
        // from inside a tube that surrounds the camera on all sides, a
        // shadow ray toward almost any fixed external light direction
        // curves back into that same nearby wall within a fraction of a
        // unit, so there's no light angle that reads as "unshadowed"
        // except right at the silhouette's grazing edge. The donut is an
        // actual open volume with separated occluders, where this has
        // something real to say.
        float shadow = iDimension > 0.5 ? 1.0
            : softShadow(p + n * 0.02, lightDir, 6.0, 10.0, 0.5, axisDir, camTarget);

        float diff = max(dot(n, lightDir), 0.0) * shadow;
        // Shininess/energy from roughness (smooth -> tight, bright peak;
        // rough -> broad, dim peak) instead of one fixed exponent applied
        // to every surface regardless of how rough it reads. Tunnel gets
        // its own, tighter/dimmer range: spec is pure white (vec3(1.0)
        // below, no hue), and broadening it for "roughness" the way a
        // real material would is exactly wrong on a concave wall that
        // fills the entire screen -- a wide white highlight there covers
        // a huge fraction of visible pixels, not a small patch, so it
        // reads as an outright whitewash rather than a highlight. Found
        // this by direct report ("way too bright") persisting even after
        // cutting fresnel hard, which pointed at a separate, uncolored
        // contributor stacking on top of it.
        float shininess = iDimension > 0.5 ? mix(180.0, 70.0, roughness) : mix(120.0, 8.0, roughness);
        float specStrength = iDimension > 0.5 ? mix(0.45, 0.1, roughness) : mix(1.0, 0.3, roughness);
        float spec = pow(max(dot(n, halfV), 0.0), shininess) * specStrength * (0.5 + treb * 0.8) * shadow;
        // calcAO's 5 more mapDist calls are meant to catch self-occlusion
        // in the donut's curved crevices; a cylinder wall is uniformly
        // curved with nothing nearby to occlude it, so true AO there would
        // come back essentially constant anyway — skip straight to that
        // constant rather than paying for 5 samples per pixel to confirm
        // what's already known, on what's now the majority-case pixel.
        float ao = iDimension > 0.5 ? 1.0 : calcAO(p, n, axisDir, camTarget);

        // Environment reflection: the same nebula/starfield functions used
        // for the real background, resampled along the reflection vector
        // (equirectangular-ish: azimuth/elevation of the reflected ray) so
        // the surface picks up the colors and structure of the space
        // around it instead of reading as a lit object floating in front
        // of a backdrop it never actually touches. Strongest at grazing
        // angles via fresnel, like a real glossy coating. Skipped
        // entirely in the tunnel, not just dimmed: it was already cut to
        // minor visual weight there (the white-wash fix above), and unlike
        // the donut's silhouette, the tunnel wall fills nearly the whole
        // view — this is two extra noise-octave function calls on what's
        // now the majority-case pixel for a contribution that barely
        // showed anyway. Inside a tunnel there's also less conceptual
        // need for it: you're surrounded by the tunnel itself, not a
        // small object reflecting a separate space around it.
        vec3 envColor = vec3(0.0);
        if (iDimension < 0.5) {
            vec3 refl = reflect(rd, n);
            vec2 envUV = vec2(atan(refl.z, refl.x), refl.y) * vec2(0.35, 0.7);
            envColor = nebula(envUV, iMotion.x) * 2.2 + starField(envUV, iMotion.x, iSpeed) * 0.6;
        }

        // Tunnel wall hue: a smooth axial gradient (no angular modulation)
        // for the ambient tube tone, so there's no static-looking pattern
        // sitting underneath the streak glow below.
        vec3 veinHue;
        vec3 streakGlow = vec3(0.0);
        if (iDimension > 0.5) {
            float rawAngle = angleAroundAxis(p, camTarget, axisDir);
            float alongAxis = dot(p - camTarget, axisDir);
            veinHue = paletteColor(alongAxis * 0.1 + iTime * 0.15);

            // Glowing light streaks racing down the tunnel, on the same
            // angle+alongAxis spiral coordinate the old kaleidoscope fold
            // traced out as flat, static stripes — now animated (an
            // -iTime phase) so it reads as actual light shooting past
            // rather than a plain pattern painted on the wall. A sharp
            // pow() falloff keeps each streak a thin bright line instead
            // of a filled wedge. Two layers at different pitch, speed,
            // and direction avoid it reading as one simple spinning
            // pinwheel; speed picks up with bass so the tunnel visibly
            // quickens on louder passages.
            float streakSpeed = mix(0.6, 2.2, iAudioSlow.x);
            const float kStreakCount = 6.0;
            const float kStreakAngle = 6.28318 / kStreakCount;
            // rawAngle comes from atan2 (angleAroundAxis), which jumps by
            // exactly 2π at its branch cut -- a seam fixed in world space
            // (wherever that cut falls relative to the tunnel's frozen
            // local basis), independent of the camera's own roll/pitch/
            // yaw sway, matching a report of a seam that "stays generally
            // on one side, moves around a little" as the camera sways
            // past it. Multiplying rawAngle by anything that doesn't map
            // that 2π jump onto an exact multiple of kStreakAngle leaves
            // mod(spiral, kStreakAngle) landing on a different phase right
            // at the cut. 1.3 didn't (2π*1.3 / kStreakAngle = 7.8, not an
            // integer); 4/3 does (2π*(4/3) / kStreakAngle = 8 exactly),
            // for a barely-perceptible change to the spiral's wind rate.
            const float kSpiralWind = 4.0 / 3.0;

            float spiral1 = rawAngle * kSpiralWind + alongAxis * 0.3 - iTime * streakSpeed;
            float dist1 = abs(mod(spiral1, kStreakAngle) - kStreakAngle * 0.5) / (kStreakAngle * 0.5);
            float streak1 = pow(1.0 - clamp(dist1, 0.0, 1.0), 22.0);

            float spiral2 = rawAngle * kSpiralWind - alongAxis * 0.22 + iTime * streakSpeed * 0.55 + kStreakAngle * 0.5;
            float dist2 = abs(mod(spiral2, kStreakAngle) - kStreakAngle * 0.5) / (kStreakAngle * 0.5);
            float streak2 = pow(1.0 - clamp(dist2, 0.0, 1.0), 26.0);

            vec3 streakHue1 = paletteColor(rawAngle / 6.28318 + iTime * 0.2);
            vec3 streakHue2 = paletteColor(rawAngle / 6.28318 + 0.4 + iTime * 0.2);
            streakGlow = streakHue1 * streak1 * 0.7 + streakHue2 * streak2 * 0.5;
        } else {
            float tubeAngle = atan(p.y, length(p.xz) - mix(1.0 + iAudioSlow.x * 0.25, 0.0, iMorph));
            veinHue = paletteColor(tubeAngle / 6.28318 + iTime * 0.05);
        }
        vec3 veinColor = veinHue * (veins * 2.8 + veinGlow * 0.9) * veinIntensity;

        // Gentle fresnel tint rather than a full swap, so the silhouette
        // edge doesn't read as a separate solid-colored ring.
        vec3 iridescent = mix(hitGlowColor, uPalette[1], fresnel * 0.4);

        // Beat flash: base color snaps toward white briefly on hits, capped
        // so the electric veins stay legible even on big hits. Cut hard
        // (was 0.35/0.55) -- reported as "the flashes are too bright" once
        // this was riding on top of the materials pass's own brightness
        // increase (see kFresnelOverdrive below) rather than the older,
        // dimmer base it was originally tuned against.
        vec3 flashColor = mix(iridescent, vec3(1.0), clamp(beatEnergy * 0.15, 0.0, 0.3));

        // fresnel is already roughness-damped above, but confirmed via
        // direct report ("the tunnel is way too bright") that this still
        // ran hot: the old per-dimension fresnelBoost this replaced was a
        // much harder 0.35 for the tunnel specifically, and the physical
        // Schlick curve (exponent 5) it's paired with is less steep than
        // the old hand-tuned exponent-7 curve, so more of the tunnel's
        // (mostly grazing, since it's concave) surface reads as reflective
        // than before even with roughness damping applied. Dropped below
        // 1.0 rather than just removing the overdrive.
        const float kFresnelOverdrive = 0.45;
        // Floor and scale both trimmed slightly further (was 0.6/0.18) to
        // compensate for the materials pass dropping the old shimmer
        // term entirely -- shimmer oscillated 0.76-1.0 (averaging ~0.88),
        // so losing it without adjustment was a flat ~14% brightness
        // increase on every hit surface that went uncompensated at the
        // time.
        vec3 base = flashColor * (diff * 0.55 + 0.14) * ao;
        // Rim highlight dims in shadow too rather than staying fully lit
        // regardless -- partial (mix floor 0.35, not 0) so silhouette
        // edges stay readable even in deep shadow.
        col = base + fresnel * iridescent * kFresnelOverdrive * mix(0.35, 1.0, shadow) + spec * vec3(1.0) + veinColor;
        col += envColor * mix(0.12, 0.85, fresnel) * ao;
        col += streakGlow * mix(0.15, 0.85, beatEnergy);

        // Fade toward darkness with hit distance, tunnel only. Two jobs
        // at once: sells "endless corridor receding into haze" rather
        // than a wall capping the view, and tames the fresnel hot-spot
        // that the generous maxDist above otherwise creates right at the
        // vanishing point — a near-axis-parallel ray's hit is, by
        // definition, at an extremely grazing angle, so without this the
        // fresnel-driven brightness term (col += fresnel*iridescent*1.3
        // above) lights up exactly the pixels furthest away, the
        // opposite of how distance should read.
        if (iDimension > 0.5) {
            col *= clamp(1.0 - t / maxDist, 0.15, 1.0);
        }
    }

    col += glowAccum * (1.0 + iAudioSlow.x * 1.5);

    // Volumetric fog sits in front of everything else composited so far
    // (surface, glow, background layers) -- transmittance dims what's
    // behind it, then the in-scattered light itself is added on top.
    // Tinted warm-white rather than a flat white "sunbeam" so it still
    // reads as belonging to this piece's own palette-driven look.
    col = col * transmittance + volumetric * vec3(1.0, 0.95, 0.85) * 0.6;

    // Both onset tint and shockwaves are screen-space effects keyed to
    // the raw screen center — shockwaves especially, being literal
    // expanding circles via length(uv), read fine as a beat-synced flash
    // over the donut's small silhouette, but once actually inside the
    // tunnel they (a) don't track the tube's real, camera-tilt-shifted
    // vanishing point, the same mismatch the background rings/nebula/
    // starfield above already fade out for, and (b) fought the tunnel's
    // own "recede into darkness" mood with a bright music-synced circular
    // flash that isn't coming from where the tunnel actually converges.
    // Faded via worldBlend like those other layers rather than gated
    // strictly on iDimension, so they still ease out smoothly through
    // the approach instead of popping off at the hard cut.
    // Kept small: this is a flat lift over every pixel on every beat, so
    // at the old 0.15 it washed the whole frame toward grey in time with
    // the music rather than reading as a flash.
    col += onset * vec3(1.0, 0.9, 0.8) * 0.04 * bgFade;
    col += shockwaves(uv, iShockwave) * bgFade;

    // Alpha carries depth for the composite pass's depth-of-field blend, with
    // a negative value meaning "always sharp, skip focus comparison
    // entirely." Trying to have the donut's own focus tracked (median
    // sampling etc.) kept breaking in new ways as things orbiting near it
    // changed depth, so it's simpler and far more robust to just never
    // blur the donut or its bolts, full stop, and only apply real
    // depth-of-field to the ambient background/particle layers.
    float outputDepth = -1.0;
    if (!hit) {
        // Purely screen-position depth for every ambient layer, particles
        // included — deliberately NOT each particle's real orbit depth.
        // That was tried, but particles constantly drift toward and away
        // from camera, so "sharp near the edge" only held true whenever
        // enough particles happened to currently be close, then reverted to
        // looking blurry again moments later as they orbited on. A fixed
        // radial rule is less "realistic" but stays visually consistent.
        float r = length(uv);
        float radialDepth = mix(17.0, 2.2, smoothstep(0.05, 1.15, r));
        outputDepth = radialDepth;
        // Geometric mask, not color brightness: even the bolt's faint outer
        // fringe should stay pinned to the sentinel "always sharp" depth.
        float boltWeight = smoothstep(0.0, 0.02, boltMask);
        outputDepth = mix(outputDepth, -1.0, boltWeight);
    }

    fragColor = vec4(col, outputDepth);
}
