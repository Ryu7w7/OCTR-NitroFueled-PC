#version 450
#extension GL_EXT_samplerless_texture_functions : require
// Copies color texels between targets of the same format (the engine's render target copies).
layout(set = 0, binding = 0) uniform texture2D source;
layout(push_constant) uniform Copy {
    ivec2 shift; // source texel = destination texel + shift
};
layout(location = 0) out vec4 color;
void main() {
    color = texelFetch(source, ivec2(gl_FragCoord.xy) + shift, 0);
}
