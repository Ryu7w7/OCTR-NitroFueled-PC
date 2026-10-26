// Fur blur pixel shader, written for the PC in place of the translated FurBlur_ea39a637_ps.
//
// Blurs the scene color (t0) along the fur direction stored in the fur buffer (t1: x coverage,
// yz direction): the direction is the sum of four taps around the pixel; unless it is the
// neutral value, 16 taps along it (offsets -1/2 .. 7/16 of it) are averaged with the pixel,
// each counted only where the fur buffer has coverage. Same interface and arithmetic as the
// translation.

cbuffer CB0 : register(b0) { float4 cb[1]; }; // zw: texel size
cbuffer CTRSwizzle : register(b13) { uint4 ctr_swz[8]; uint4 ctr_vin[8]; float4 ctr_scale; float4 ctr_tscale[8]; };

Texture2D<float4> t0 : register(t0); // scene color
Texture2D<float4> t1 : register(t1); // fur buffer
SamplerState sm0 : register(s0);
SamplerState sm1 : register(s1);

float Sel(float4 v, uint c) { return c == 0u ? 0.0f : c == 1u ? 1.0f : v[c - 2u]; }
float4 Swz(float4 v, uint s) {
    return s == 0u ? v : float4(Sel(v, s & 7u), Sel(v, (s >> 3) & 7u), Sel(v, (s >> 6) & 7u), Sel(v, (s >> 9) & 7u));
}

struct PSIn {
    float4 pos : SV_Position;
    [[vk::location(0)]] float4 uv : VARYING15; // xy: screen coordinates
};

float4 main(PSIn IN) : SV_Target0 {
    const float2 uv = IN.uv.xy;
    const float2 h = 0.5f * cb[0].zw;
    const uint sw1 = ctr_swz[0].y, sw0 = ctr_swz[0].x;
    // the fur direction, from four taps around the pixel
    float2 dir = Swz(t1.Sample(sm1, float2(uv.x + h.x, uv.y - h.y)), sw1).yz;
    dir += Swz(t1.Sample(sm1, float2(uv.x + h.x, uv.y + h.y)), sw1).yz;
    dir += Swz(t1.Sample(sm1, float2(uv.x - h.x, uv.y + h.y)), sw1).yz;
    dir += Swz(t1.Sample(sm1, float2(uv.x - h.x, uv.y - h.y)), sw1).yz;
    const float4 color = Swz(t0.Sample(sm0, uv), sw0);
    const float neutral = asfloat(0x3ffefeffu); // four times the encoded zero direction
    if (dir.x == neutral && dir.y == neutral) {
        return color;
    }
    const float2 d = mad(0.05f, 0.25f * dir, asfloat(0xbccbff33u)); // decoded direction
    float4 sum = color;
    float count = 1.0f;
    [unroll] for (int i = 0; i < 16; ++i) {
        const float2 p = mad(d, float(i - 8) * 0.0625f, uv);
        if (Swz(t1.Sample(sm1, p), sw1).x > 0.0f) {
            sum += i == 8 ? color : Swz(t0.Sample(sm0, p), sw0);
            count += 1.0f;
        }
    }
    return rcp(count) * sum;
}
