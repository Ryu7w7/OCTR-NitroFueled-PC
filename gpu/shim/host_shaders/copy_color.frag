#version 450
#extension GL_EXT_samplerless_texture_functions : require
// Copies color texels between targets of the same format (the engine's render target copies).
layout(set = 0, binding = 0) uniform texture2D source;
layout(push_constant) uniform Copy {
    vec2 shift;
    vec2 scale;
};
layout(location = 0) out vec4 color;
void main() {
    ivec2 pixel = ivec2(gl_FragCoord.xy * scale + shift);
    color = texelFetch(source, clamp(pixel, ivec2(0), textureSize(source, 0) - 1), 0);
}
