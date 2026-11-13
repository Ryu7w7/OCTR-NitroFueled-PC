#version 450
#extension GL_EXT_samplerless_texture_functions : require
// Copies depth texels into a color target (the engine's depth -> R32F copies).
layout(set = 0, binding = 0) uniform texture2D depth;
layout(push_constant) uniform Copy {
    vec2 shift;
    vec2 scale;
};
layout(location = 0) out vec4 color;
void main() {
    ivec2 pixel = ivec2(gl_FragCoord.xy * scale + shift);
    color = vec4(texelFetch(depth, clamp(pixel, ivec2(0), textureSize(depth, 0) - 1), 0).r);
}
