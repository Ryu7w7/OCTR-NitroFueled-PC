#version 450
#extension GL_EXT_samplerless_texture_functions : require
// Copies depth texels into a color target (the engine's depth -> R32F copies).
layout(set = 0, binding = 0) uniform texture2D depth;
layout(push_constant) uniform Copy {
    ivec2 shift; // source texel = destination texel + shift
};
layout(location = 0) out vec4 color;
void main() {
    color = vec4(texelFetch(depth, ivec2(gl_FragCoord.xy) + shift, 0).r);
}
