// Fur (G-buffer) pixel shader, written for the PC in place of the translated Fur_e5b3ee2d_ps.
//
// Same interface as the translation (bindings b0, b13, t0/1/2/4/6/12, VARYING15.. at locations
// 0..) and the same arithmetic, minus the GCN emulation: the hair is a height field marched from
// the strand root (VARYING17.zw) toward the pixel's coordinate, up to 32 steps, sampling the
// strand texture (t12); a pixel the march misses is discarded. Then the usual G-buffer output:
// tangent-space normal map, albedo, specular, view-space normal and depth term.

cbuffer CB0 : register(b0) { float4 cb[16]; };
cbuffer CTRSwizzle : register(b13) { uint4 ctr_swz[8]; uint4 ctr_vin[8]; float4 ctr_scale; float4 ctr_tscale[8]; };

Texture2D<float4> t0 : register(t0);   // albedo
Texture2D<float4> t1 : register(t1);   // normal map (xy)
Texture2D<float4> t2 : register(t2);   // specular / material
Texture2D<float4> t4 : register(t4);   // material scalar
Texture2D<float4> t6 : register(t6);   // fur mask
Texture2D<float4> t12 : register(t12); // strand height field
SamplerState sm0 : register(s0);
SamplerState sm1 : register(s1);
SamplerState sm2 : register(s2);
SamplerState sm4 : register(s4);
SamplerState sm6 : register(s6);
SamplerState sm12 : register(s12);

// the texture's channel order (the engine's formats against ours), as the translation applies it
float Sel(float4 v, uint c) { return c == 0u ? 0.0f : c == 1u ? 1.0f : v[c - 2u]; }
float4 Swz(float4 v, uint s) {
    return s == 0u ? v : float4(Sel(v, s & 7u), Sel(v, (s >> 3) & 7u), Sel(v, (s >> 6) & 7u), Sel(v, (s >> 9) & 7u));
}

struct PSIn {
    float4 pos : SV_Position;
    [[vk::location(0)]] float4 tint : VARYING15;     // xyz: albedo tint
    [[vk::location(1)]] float4 uv : VARYING16;       // xy: material coordinates
    [[vk::location(2)]] float4 strand : VARYING17;   // xy: fur coordinates, zw: strand root
    [[vk::location(4)]] float4 shell : VARYING19;    // xyz: normal when the fur is not marched
    [[vk::location(5)]] float4 normal : VARYING20;   // xyz: geometric normal
    [[vk::location(6)]] float4 view : VARYING21;     // w: view depth
    [[vk::location(7)]] float4 tangent : VARYING22;  // xyz: tangent, w: G-buffer 1 alpha
    [[vk::location(8)]] float4 binormal : VARYING23; // xyz: binormal, w: G-buffer 2 alpha
    [[vk::location(9)]] float4 height : VARYING24;   // x > 0: marched, y..z: strand height range
};

struct PSOut {
    float4 c0 : SV_Target0;
    float4 c1 : SV_Target1;
    float4 c2 : SV_Target2;
    float4 c3 : SV_Target3;
};

PSOut main(PSIn IN) {
    const bool fur = asuint(cb[6].x) != 0u;
    const bool march = IN.height.x > 0.0f;

    float3 n;                // geometric normal
    float strandMask = 0.0f; // the fur mask where the march hit
    float hitRatio = 0.0f;   // strand height / surface height there
    if (fur && march) {
        const float2 uv = IN.strand.xy, root = IN.strand.zw;
        const float mask = Swz(t6.Sample(sm6, uv), ctr_swz[1].z).x;
        const float2 d = uv - root;
        const float len = sqrt(mad(d.y, d.y, d.x * d.x));
        const float s = 256.0f * len;
        const float steps = max(min(32.0f, s), min(max(32.0f, s), 2.0f)); // median of 32, s, 2
        const float stepInv = rcp(steps);
        const float h0 = IN.height.y, dh = IN.height.z - IN.height.y;
        const float threshold = asfloat(0x3a83126fu) * cb[8].x; // 0.001
        bool hit = false;
        [loop] for (float i = 0.0f; i < steps; i += 1.0f) {
            const float t = mad(stepInv, -i, 1.0f);
            const float2 p = mad(d, t, root);
            const float4 r = Swz(t12.SampleLevel(sm12, p * cb[7].xy, 0.0f), ctr_swz[3].x);
            const float h = mad(dh, t, h0);
            const float surface = mad(mask * r.w, -cb[9].x, mask);
            const float ratio = h * rcp(surface);
            if (ratio <= 1.0f) {
                float a = saturate(max(0.0f, ratio));
                a = exp2(cb[9].y * log2(a));
                a = saturate(1.0f - a) * threshold;
                if (0.25f * r.z < a) {
                    hit = true;
                    strandMask = mask;
                    hitRatio = ratio;
                    break;
                }
            }
        }
        n = IN.normal.xyz * rsqrt(mad(IN.normal.z, IN.normal.z, mad(IN.normal.y, IN.normal.y, IN.normal.x * IN.normal.x)));
        if (!hit) {
            discard;
        }
    } else if (fur) {
        n = IN.shell.xyz;
    } else {
        n = IN.normal.xyz * rsqrt(mad(IN.normal.z, IN.normal.z, mad(IN.normal.y, IN.normal.y, IN.normal.x * IN.normal.x)));
    }

    // material
    const float2 uv = IN.uv.xy;
    const float4 nm = Swz(t1.Sample(sm1, uv), ctr_swz[0].y);
    const float4 albedoTex = Swz(t0.Sample(sm0, uv), ctr_swz[0].x);
    float m = Swz(t4.Sample(sm4, uv), ctr_swz[1].x).x;
    const float4 spec = Swz(t2.Sample(sm2, uv), ctr_swz[0].z);
    const float strength = saturate(cb[12].x * cb[12].y);

    // normal map: n * z + ny * tangent - nx * binormal
    const float ny = mad(2.0f, nm.y, -1.0f) * strength;
    const float nx = mad(2.0f, nm.x, -1.0f) * strength;
    const float z = sqrt(saturate(1.0f - mad(nx, nx, ny * ny)));
    float3 N;
    N.x = mad(-nx, IN.binormal.x, mad(ny, IN.tangent.x, n.x * z));
    N.y = mad(-nx, IN.binormal.y, mad(ny, IN.tangent.y, n.y * z));
    N.z = mad(-nx, IN.binormal.z, mad(ny, IN.tangent.z, n.z * z));
    N *= rsqrt(mad(N.z, N.z, mad(N.y, N.y, N.x * N.x)));

    float3 albedo = albedoTex.xyz * IN.tint.xyz;
    m = mad(cb[5].y, cb[5].x - m, m);
    float specX = spec.x;

    // view-space normal (xy, scaled by the depth term) and material outputs
    float vx = 0.0f, vy = 0.0f, c2y = 0.0f, c2z = 0.0f;
    if (!fur || march) {
        if (fur) {
            const float f = mad(cb[10].x * strandMask, sqrt(hitRatio) - 1.0f, 1.0f);
            albedo *= f;
            specX = specX * hitRatio * hitRatio;
        }
        const float ex = mad(cb[2].x, N.z, mad(cb[1].x, N.y, cb[0].x * N.x));
        const float ey = mad(cb[2].y, N.z, mad(cb[1].y, N.y, cb[0].y * N.x));
        const float inv = rsqrt(mad(ey, ey, ex * ex));
        const float depthScale = cb[8].y * rcp(mad(cb[2].w, IN.view.w, cb[3].w));
        const float depth = mad(depthScale, -mad(cb[2].z, IN.view.w, cb[3].z), cb[8].y);
        vx = ex * inv * depth;
        vy = ey * inv * depth;
        c2y = mad(cb[10].z, spec.y, cb[10].y);
        c2z = mad(cb[11].x, m, cb[10].w);
    }

    PSOut o;
    o.c0 = float4(albedo, specX);
    o.c1 = float4(mad(0.5f, N, 0.5f), IN.tangent.w);
    o.c2 = float4(spec.z, c2y, c2z, IN.binormal.w);
    o.c3 = float4(1.0f, mad(0.5f, vx, 0.5f), mad(0.5f, vy, 0.5f), float(asint(cb[4].x)) * asfloat(0x3eaaaaabu));
    return o;
}
