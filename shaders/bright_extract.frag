#version 410 core

uniform sampler2D uScene;
uniform float uThreshold;

out vec4 fragColor;

void main() {
    ivec2 texel = ivec2(gl_FragCoord.xy) * 2;
    ivec2 sceneSize = textureSize(uScene, 0);
    texel = min(texel, sceneSize - ivec2(1));

    vec3 c = texelFetch(uScene, texel, 0).rgb;
    float lum = dot(c, vec3(0.2126, 0.7152, 0.0722));
    float mask = smoothstep(uThreshold, uThreshold * 2.0, lum);
    fragColor = vec4(c * mask, 1.0);
}
