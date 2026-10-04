#version 410 core

// Additively accumulates one weighted bloom scale into the running total.
// Drawn once per scale with GL_ONE/GL_ONE blending enabled on the target.

uniform sampler2D uTex;
uniform vec2 uOutputSize;
uniform float uWeight;

out vec4 fragColor;

void main() {
    vec2 uv = gl_FragCoord.xy / uOutputSize;
    fragColor = vec4(texture(uTex, uv).rgb * uWeight, 1.0);
}
