#version 410 core

uniform sampler2D uTex;
uniform vec2 uTexelStep;  // blur direction * (1/size), in the INPUT texture's texel units
uniform vec2 uOutputSize; // this pass's render target size, NOT uTex's size
// Explicit LOD to sample uTex at. 0 for same-resolution blurs (bloom chain,
// DOF's second pass). For DOF's first pass, sceneFB is supersampled and
// roughly 2x dofBlurFB's linear resolution, so this pass's sparse, widely-
// spaced taps were skipping right over the donut's crisp edge — sampling
// its own uncorrelated slice of the edge each frame as the donut moved,
// which read as a woven/moire pattern. sceneFB carries a per-frame-
// regenerated mip chain (hardware box-filtered, alias-safe) instead; this
// pass samples mip 1 of it, matching the resolution ratio.
uniform float uLod;

out vec4 fragColor;

const float kWeights[5] = float[5](0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216);

void main() {
    // uv must be relative to this pass's own output size, not uTex's size —
    // those differ when downsample-blurring the full-res scene into a
    // half-res target for depth of field (they happened to match for the
    // bloom chain, which is why this bug didn't show up there).
    vec2 uv = gl_FragCoord.xy / uOutputSize;

    vec3 result = textureLod(uTex, uv, uLod).rgb * kWeights[0];
    for (int i = 1; i < 5; ++i) {
        vec2 offset = uTexelStep * float(i);
        result += textureLod(uTex, uv + offset, uLod).rgb * kWeights[i];
        result += textureLod(uTex, uv - offset, uLod).rgb * kWeights[i];
    }
    fragColor = vec4(result, 1.0);
}
