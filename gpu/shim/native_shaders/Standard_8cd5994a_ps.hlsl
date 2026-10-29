// Standard (G-buffer) pixel shader, written for the PC in place of the translated
// Standard_8cd5994a_ps, the most drawn shader in a race: albedo, tangent-space normal map and
// specular from one coordinate. Same interface as the translation (bindings b0, b13, t0/1/2,
// VARYING15.. at locations 0..) and the same arithmetic, minus the GCN emulation.

cbuffer CB0 : register(b0) { float4 cb[3]; };
cbuffer CTRSwizzle : register(b13) { uint4 ctr_swz[8]; uint4 ctr_vin[8]; float4 ctr_scale; float4 ctr_tscale[8]; };

Texture2D<float4> t0 : register(t0); // albedo
Texture2D<float4> t1 : register(t1); // normal map (xy)
Texture2D<float4> t2 : register(t2); // specular / material
SamplerState sm0 : register(s0);
SamplerState sm1 : register(s1);
SamplerState sm2 : register(s2);

// the texture's channel order (the engine's formats against ours), as the translation applies it
float Sel(float4 v, uint c) { return c == 0u ? 0.0f : c == 1u ? 1.0f : v[c - 2u]; }
float4 Swz(float4 v, uint s) {
    return s == 0u ? v : float4(Sel(v, s & 7u), Sel(v, (s >> 3) & 7u), Sel(v, (s >> 6) & 7u), Sel(v, (s >> 9) & 7u));
}

struct PSIn {
    float4 pos : SV_Position;
    [[vk::location(0)]] float4 tint : VARYING15;     // xyz: albedo tint
    [[vk::location(1)]] float4 uv : VARYING16;       // xy: coordinates
    [[vk::location(3)]] float4 normal : VARYING18;   // xyz: normal
    [[vk::location(4)]] float4 tangent : VARYING19;  // xyz: tangent, w: G-buffer 1 alpha
    [[vk::location(5)]] float4 binormal : VARYING20; // xyz: binormal, w: G-buffer 2 alpha
};

struct PSOut {
    float4 c0 : SV_Target0;
    float4 c1 : SV_Target1;
    float4 c2 : SV_Target2;
    float4 c3 : SV_Target3;
};

PSOut main(PSIn IN) {
    const float2 uv = IN.uv.xy;
    const float2 nm = Swz(t1.Sample(sm1, uv), ctr_swz[0].y).xy;
    const float3 albedo = Swz(t0.Sample(sm0, uv), ctr_swz[0].x).xyz;
    const float3 spec = Swz(t2.Sample(sm2, uv), ctr_swz[0].z).xyz;

    // normal map: n·z + y·tangent − x·binormal
    const float strength = saturate(cb[2].x * cb[2].y);
    const float y = mad(2.0f, nm.y, -1.0f) * strength;
    const float x = mad(2.0f, nm.x, -1.0f) * strength;
    const float z = sqrt(saturate(1.0f - mad(x, x, y * y)));
    float3 N = IN.normal.xyz * z + IN.tangent.xyz * y - IN.binormal.xyz * x;
    N *= rsqrt(dot(N, N));

    PSOut o;
    o.c0 = float4(albedo * IN.tint.xyz, spec.x);
    o.c1 = float4(mad(0.5f, N, 0.5f), IN.tangent.w);
    o.c2 = float4(spec.z, spec.y, cb[1].x * cb[1].y, IN.binormal.w);
    o.c3 = float4(0.0f, 0.5f, 0.5f, float(asint(cb[0].x)) * asfloat(0x3eaaaaabu));
    return o;
}
