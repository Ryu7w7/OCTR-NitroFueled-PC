// Blended uber (G-buffer) pixel shader, written for the PC in place of the translated
// UberShaderBlended_8c4835d9_ps: two materials (A: t0/t1/t2, B: t4/t7/t14) mixed by a blend
// mask, over a parallax-occlusion height field (t6).
//
// Same interface as the translation (bindings b0, b13, t0/1/2/4/6/7/14, VARYING15.. at
// locations 0..) and the same arithmetic, minus the GCN emulation. The height field is marched
// from the top layer down, n = lerp(cb[6].y, cb[6].x, |cos view|) layers, along the view
// direction in tangent space; the hit height (interpolated between the last two layers) shifts
// both materials' coordinates. The offsets are written as A·d/d17 (the translation's
// rsqrt(len)·len·d/d17 cancels the same way, without its NaN at len 0).
//
// The march fetches four layers at a time (independent fetches, their latency overlapped
// instead of one dependent fetch per layer) and tests them in order: the same hit, with the
// positions and layer heights summed one step at a time as in the translation.

cbuffer CB0 : register(b0) { float4 cb[16]; };
cbuffer CTRSwizzle : register(b13) { uint4 ctr_swz[8]; uint4 ctr_vin[8]; float4 ctr_scale; float4 ctr_tscale[8]; };

Texture2D<float4> t0 : register(t0);   // material A albedo (a: blend term)
Texture2D<float4> t1 : register(t1);   // material A normal map (xy)
Texture2D<float4> t2 : register(t2);   // material A specular
Texture2D<float4> t4 : register(t4);   // material B albedo (a: blend term)
Texture2D<float4> t6 : register(t6);   // height field (x)
Texture2D<float4> t7 : register(t7);   // material B normal map (xy)
Texture2D<float4> t14 : register(t14); // material B specular
SamplerState sm0 : register(s0);
SamplerState sm1 : register(s1);
SamplerState sm2 : register(s2);
SamplerState sm4 : register(s4);
SamplerState sm6 : register(s6);
SamplerState sm7 : register(s7);
SamplerState sm14 : register(s14);

// the texture's channel order (the engine's formats against ours), as the translation applies it
float Sel(float4 v, uint c) { return c == 0u ? 0.0f : c == 1u ? 1.0f : v[c - 2u]; }
float4 Swz(float4 v, uint s) {
    return s == 0u ? v : float4(Sel(v, s & 7u), Sel(v, (s >> 3) & 7u), Sel(v, (s >> 6) & 7u), Sel(v, (s >> 9) & 7u));
}

struct PSIn {
    float4 pos : SV_Position;
    [[vk::location(0)]] float4 tint : VARYING15;  // xyz: albedo tint, w: blend bias
    [[vk::location(1)]] float4 uv : VARYING16;    // xy: material A, zw: material B
    [[vk::location(2)]] float4 normal : VARYING17;    // xyz: normal
    [[vk::location(3)]] float4 tangentB : VARYING18;  // xyz: material B tangent
    [[vk::location(4)]] float4 binormalB : VARYING19; // xyz: material B binormal
    [[vk::location(6)]] float4 tangentA : VARYING21;  // xyz: material A tangent, w: G-buffer 1 alpha
    [[vk::location(7)]] float4 binormalA : VARYING22; // xyz: material A binormal, w: G-buffer 2 alpha
    [[vk::location(8)]] float4 world : VARYING23;     // xyz: position, w: lowest height
};

struct PSOut {
    float4 c0 : SV_Target0;
    float4 c1 : SV_Target1;
    float4 c2 : SV_Target2;
    float4 c3 : SV_Target3;
};

// a tangent-space normal map sample (2·xy − 1, scaled) on a frame: n·z + y·t − x·b
float3 MapNormal(float2 nm, float scale, float3 n, float3 t, float3 b) {
    const float x = mad(2.0f, nm.x, -1.0f) * scale;
    const float y = mad(2.0f, nm.y, -1.0f) * scale;
    const float xy = mad(x, x, y * y);
    const float z = sqrt(saturate(1.0f - xy));
    const float r = rsqrt(mad(z, z, xy));
    return n * (z * r) + t * (y * r) - b * (x * r);
}

PSOut main(PSIn IN) {
    const float3 V = cb[2].xyz - IN.world.xyz;
    const float d17 = dot(V, IN.normal.xyz);
    const float k = cb[6].z * rcp(d17);
    const float2 offB = k * float2(dot(V, IN.tangentB.xyz), dot(V, IN.binormalB.xyz));
    const float2 offA = k * float2(dot(V, IN.tangentA.xyz), dot(V, IN.binormalA.xyz));

    // parallax occlusion: march the height field in material B's frame (cb[7].z) or A's
    const bool onB = asuint(cb[7].z) != 0u;
    float2 p = onB ? IN.uv.zw : IN.uv.xy;
    const float cosV = min(1.0f, abs(d17 * rsqrt(dot(V, V))));
    const float layers = mad(cosV, cb[6].x - cb[6].y, cb[6].y);
    const int n = int(layers);
    const float dLayer = rcp(-trunc(layers));
    const float2 dp = (onB ? offB : offA) * dLayer;
    const float lod = t6.CalculateLevelOfDetail(sm6, p);
    const uint swH = ctr_swz[1].z;
    const float floorH = IN.world.w;

    float prevH = max(Swz(t6.SampleLevel(sm6, p, lod), swH).x, floorH);
    float prevLayer = 1.0f;
    float height = 0.0f;
    [loop] for (int i = 0; i < n; i += 4) {
        float2 q[4];
        float layer[4], h[4];
        [unroll] for (int k = 0; k < 4; ++k) {
            p += dp;
            q[k] = p;
            layer[k] = (k ? layer[k - 1] : prevLayer) + dLayer;
        }
        [unroll] for (int j = 0; j < 4; ++j) {
            h[j] = max(Swz(t6.SampleLevel(sm6, q[j], lod), swH).x, floorH);
        }
        bool hit = false;
        [unroll] for (int m = 0; m < 4; ++m) {
            if (!hit && i + m < n) {
                if (h[m] > layer[m]) {
                    // where the view ray crosses the height field between the last two layers
                    const float a = prevLayer - prevH, b = layer[m] - h[m];
                    const float den = a - b;
                    if (den != 0.0f) {
                        height = rcp(den) * mad(-prevLayer, b, a * layer[m]);
                    }
                    hit = true;
                } else {
                    prevH = h[m];
                    prevLayer = layer[m];
                }
            }
        }
        if (hit) {
            break;
        }
    }
    const float depth = 1.0f - height;
    const float2 uvA = mad(-offA, depth, IN.uv.xy);
    const float2 uvB = mad(-offB, depth, IN.uv.zw);

    // the two materials and their blend
    const float4 albedoB = Swz(t4.Sample(sm4, uvB), ctr_swz[1].x);
    const float3 specB = Swz(t14.Sample(sm14, uvB), ctr_swz[3].z).xyz;
    const float3 specA = Swz(t2.Sample(sm2, uvA), ctr_swz[0].z).xyz;
    const float2 nmA = Swz(t1.Sample(sm1, uvA), ctr_swz[0].y).xy;
    const float4 albedoA = Swz(t0.Sample(sm0, uvA), ctr_swz[0].x);
    const float2 nmB = Swz(t7.Sample(sm7, uvB), ctr_swz[1].w).xy;

    const float bias = saturate(1.0f - (cb[7].y + IN.tint.w));
    const float mask = mad(mad(cb[3].z, albedoA.w, cb[3].w), mad(cb[3].x, albedoB.w, cb[3].y), -bias);
    const float blend = saturate(cb[7].x * mask);

    const float3 albedo = lerp(albedoA.xyz, albedoB.xyz, cb[4].x * blend) * IN.tint.xyz;
    const float3 spec = lerp(specA, specB, cb[5].xyz * blend);

    // normals: each material's map on its own frame, blended, renormalized
    const float3 NA = MapNormal(nmA, saturate(cb[8].x * cb[8].z), IN.normal.xyz, IN.tangentA.xyz, IN.binormalA.xyz);
    const float3 NB = MapNormal(nmB, saturate(cb[8].z * cb[8].y), IN.normal.xyz, IN.tangentB.xyz, IN.binormalB.xyz);
    float3 N = lerp(NA, NB, cb[4].y * blend);
    N *= rsqrt(dot(N, N));

    PSOut o;
    o.c0 = float4(albedo, 0.0f);
    o.c1 = float4(mad(0.5f, N, 0.5f), IN.tangentA.w);
    o.c2 = float4(spec.z, spec.y, mad(cb[1].y, cb[1].x - spec.x, spec.x), IN.binormalA.w);
    o.c3 = float4(0.0f, 0.5f, 0.5f, float(asint(cb[0].x)) * asfloat(0x3eaaaaabu));
    return o;
}
