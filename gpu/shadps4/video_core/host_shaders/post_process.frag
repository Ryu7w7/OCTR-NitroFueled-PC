// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#version 450

layout (location = 0) in vec2 uv;
layout (location = 0) out vec4 color;

layout (binding = 0) uniform sampler2D texSampler;

layout (push_constant) uniform settings {
    float gamma;
    bool hdr;
    bool srgb_input;
    float hdr_peak; // bbport: nits the display shows at most (0: the picture as it is)
    float hdr_gain; // bbport: brightness factor
} pp;

// SMPTE ST 2084 (PQ): code value <-> nits
const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
vec3 pq_to_nits(vec3 e) {
    const vec3 p = pow(clamp(e, 0.0, 1.0), vec3(1.0 / m2));
    return 10000.0 * pow(max(p - c1, 0.0) / (c2 - c3 * p), vec3(1.0 / m1));
}
vec3 nits_to_pq(vec3 l) {
    const vec3 y = pow(clamp(l / 10000.0, 0.0, 1.0), vec3(m1));
    return pow((c1 + c2 * y) / (1.0 + c3 * y), vec3(m2));
}

// The game's PQ picture scaled, then rolled off above 75% of the peak so that nothing past the
// display's brightest clips (the brightest channel decides, so hues stay).
vec3 fit_hdr(vec3 e) {
    vec3 l = pq_to_nits(e) * pp.hdr_gain;
    if (pp.hdr_peak > 0.0) {
        const float peak = pp.hdr_peak, knee = 0.75 * peak;
        const float m = max(max(l.r, l.g), l.b);
        if (m > knee) {
            const float fitted = knee + (peak - knee) * (1.0 - exp(-(m - knee) / (peak - knee)));
            l *= fitted / m;
        }
    }
    return nits_to_pq(l);
}

const float cutoff = 0.0031308, a = 1.055, b = 0.055, d = 12.92;
vec3 gamma(vec3 rgb) {
    return mix(
        a * pow(rgb, vec3(1.0 / (2.4 + 1.0 - pp.gamma))) - b,
        d * rgb / pp.gamma,
        lessThan(rgb, vec3(cutoff))
    );
}

// Exact inverse of gamma() at unit gamma, for buffers that are sRGB encoded but must be sampled
// through a UNORM view because Vulkan has no sRGB variant of the 10-bit format.
vec3 degamma(vec3 rgb) {
    return mix(
        pow(max(rgb + b, 0.0) / a, vec3(2.4)),
        rgb / d,
        lessThan(rgb, vec3(d * cutoff))
    );
}

void main() {
    vec4 color_linear = texture(texSampler, uv);
    if (pp.hdr) {
        color = color_linear;
        if (pp.hdr_peak > 0.0 || pp.hdr_gain != 1.0) {
            color.rgb = fit_hdr(color_linear.rgb);
        }
    } else {
        if (pp.srgb_input) color_linear.rgb = degamma(color_linear.rgb);
        color = vec4(gamma(color_linear.rgb), color_linear.a);
    }
}
