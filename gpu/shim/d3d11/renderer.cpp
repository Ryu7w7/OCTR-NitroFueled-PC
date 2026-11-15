// D3D11 renderer driven by the engine's own render state (see gfx.h).
#include <windows.h>
#include <intrin.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "engine.h"
#include "formats.h"
#include "gfx.h"
#include "glue.h"
#include "glyphs.h"
#include "texture.h"
#include "../renderdoc_app.h"

FILE* g_log_mirror = nullptr;  // common.h LOG

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
using namespace eng;

namespace {

// ------------------------------------------------------------------ shader index
struct ShaderInfo {
    std::string name;
    std::string kind;  // vs, ps, es (VS feeding a GS), gs
    bool vertex = false;
    std::vector<std::pair<int, int>> textures;  // slot, dims
    std::vector<std::pair<int, bool>> samplers;  // slot, compare
    std::vector<int> cbuffers;
    std::vector<std::pair<int, int>> inputs;  // sem, size
    std::vector<u8> bytecode;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11GeometryShader> gs;
    std::once_flag once;  // loaded (the preload threads, or the first draw that needs it)
    std::atomic<bool> ready{false};   // its shader object exists: draws may use it (set last, release)
    std::atomic<bool> used{false};  // a draw used it (the first use is noted: NoteFirstUse)
    u32 cube_slots = 0;   // texture slots the bytecode declares TextureCube: they take a cube view
    bool video = false;  // Bink's YCbCr -> RGB pixel shader: a video is on screen
};

struct Surface {
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11ShaderResourceView> srv, srv_array;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11DepthStencilView> dsv, dsv_ro;
    u32 w = 0, h = 0;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    bool rendered = false;
    u32 comp_swap = 0;  // CB_COLOR_INFO.COMP_SWAP of the render target that produced the data
    float scale = 1.0f;  // physical size = logical (w, h) * scale (RenderScale supersampling)
    // The game wrote or freed its memory (a video decoded where a menu target was): the texture
    // there is read from memory until the GPU draws to the target again.
    std::shared_ptr<std::atomic<bool>> written_over;
    u64 bytes = 0;
};

// RenderScale setting (percent): screen-sized render targets are created that much larger
// (supersampling anti-aliasing / higher output resolution); the final blit filters down.
// The game's frame: 1920x1080, or 2560x1440 in PS4 Pro mode (setting PS4Pro). RenderScale is the
// internal resolution in percent of 1920x1080 either way (200 = 3840x2160).
bool ProMode() {
    static const bool pro = ConfigInt("PS4Pro", 1) != 0;
    return pro;
}
float RenderScale() {
    static const float s = std::clamp(ConfigInt("RenderScale", 200), 50, 400) / 100.0f * (ProMode() ? 0.75f : 1.0f);
    return s;
}

// Render targets derived from the frame (full, half, quarter, ..., velocity tiles of 10 / 20 px)
float SurfaceScale(u32 w, u32 h) {
    static const std::pair<std::vector<u32>, std::vector<u32>> sizes = [] {
        const u32 bw = ProMode() ? 2560 : 1920, bh = ProMode() ? 1440 : 1080;
        std::vector<u32> ws, hs;
        for (u32 d : {1u, 2u, 4u, 8u, 16u, 10u, 20u}) ws.push_back((bw + d - 1) / d), hs.push_back((bh + d - 1) / d);
        return std::make_pair(ws, hs);
    }();
    bool wok = std::find(sizes.first.begin(), sizes.first.end(), w) != sizes.first.end();
    bool hok = std::find(sizes.second.begin(), sizes.second.end(), h) != sizes.second.end();
    return wok && hok ? RenderScale() : 1.0f;
}

u32 Scaled(u32 v, float s) { return std::max<u32>(1, (u32)(v * s + 0.5f)); }

// Shadow maps (16-bit depth targets, the cascade atlas is 2048x8192) are created at this scale of
// their PS4 size: setting ShadowQuality 0 / 1 / 2 = half / as on PS4 / double (sharper shadows).
// The game sees its own size; viewports, depth bias and texel loads follow the scale like they do
// for the supersampled screen targets.
float ShadowMapScale(u32 w, u32 h) {
    static unsigned gen = ~0u;
    static float scale = 1.0f;
    if (gen != ConfigGeneration()) {
        gen = ConfigGeneration();
        static const float kScales[] = {0.5f, 1.0f, 2.0f};
        scale = kScales[std::clamp(ConfigInt("ShadowQuality", 1), 0, 2)];
        LOG("gfx: shadow maps at x%.2f", scale);
    }
    return std::min(scale, 16384.0f / std::max(w, h));  // D3D11 texture size limit
}

struct Stats {
    u64 draws = 0, drawn = 0, no_shader = 0, no_target = 0, no_layout = 0, clears = 0, texture_refresh = 0,
        buffer_create = 0, buffer_refresh = 0, copies = 0;
};

std::mutex g_mutex;
std::atomic<float> g_game_gamma{1.0f};
bool g_init_failed = false;
ComPtr<ID3D11Device> g_dev;
ComPtr<ID3D11DeviceContext> g_ctx;
ComPtr<ID3D11DeviceContext1> g_ctx1;  // constant buffer offsets (D3D11.1)
ComPtr<IDXGISwapChain> g_swap;
ComPtr<ID3D11RenderTargetView> g_swap_rtv;
fs::path g_shader_dir;
std::atomic<u64> g_video_ms{0};  // last draw with a video shader
// debugging aid: CTR_TRACE_DEPTH=<hex base>,<first frame> logs, for 12 frames, the draws per viewport
// into that depth target and the clears on it
u64 g_trace_depth = 0, g_trace_frame = 0;
std::string g_trace;
std::vector<std::pair<std::string, struct Surface>> g_after_shots;  // CTR_CAPTURE_AFTER
std::atomic<float> g_picture[4] = {0.0f, 0.0f, 1.0f, 1.0f};  // game picture in the back buffer (0..1)
std::unordered_map<u64, ShaderInfo> g_shaders;          // by content hash
struct CachedProgram {
    ShaderInfo* info;
    u64 fingerprint;  // which program that address held
};
std::unordered_map<u64, CachedProgram> g_shader_by_code;  // by code address
std::unordered_map<u64, Surface> g_surfaces;            // by base address
std::unordered_map<u64, TextureObject> g_textures;      // asset textures by T# hash
std::unordered_map<u64, ComPtr<ID3D11InputLayout>> g_layouts;
std::unordered_map<u64, ComPtr<ID3D11BlendState>> g_blend;
std::unordered_map<u64, ComPtr<ID3D11RasterizerState>> g_raster;
std::unordered_map<u64, ComPtr<ID3D11DepthStencilState>> g_depth;
ComPtr<ID3D11SamplerState> g_samp_linear, g_samp_cmp;
ComPtr<ID3D11DepthStencilState> g_depth_off;
ComPtr<ID3D11Buffer> g_cb[3][16];  // VS, PS, GS
ComPtr<ID3D11Buffer> g_swz_cb[2];
ComPtr<ID3D11Buffer> g_vb_ring, g_ib_ring, g_zero_vb;
u32 g_vb_pos = 0, g_ib_pos = 0;
constexpr u32 kVbRing = 64 << 20, kIbRing = 16 << 20;
Surface g_placeholder;
ComPtr<ID3D11VertexShader> g_blit_vs;
ComPtr<ID3D11PixelShader> g_blit_ps, g_copy_ps;
ComPtr<ID3D11Buffer> g_copy_cb;
Stats g_stats;
u64 g_frame = 0;
std::atomic<u64> g_present_us{0};  // time blocked in Present
bool g_capture = false;  // logging every draw of the captured frame
bool g_capture_armed = false;    // F11 pressed: the capture starts at the next flip (GfxSnapshot)
bool g_capture_by_flip = false;  // the capture running is F11's, from one flip to the next
fs::path g_capture_dir;          // F11's capture folder

// RenderDoc, when the game was started with it (BB_RENDERDOC, bbport_d3d11.cpp): F11's capture is
// also its frame capture, and each draw in it is named after its shaders.
RENDERDOC_API_1_1_2* RenderDoc() {
    static RENDERDOC_API_1_1_2* api = [] {
        RENDERDOC_API_1_1_2* found = nullptr;
        if (HMODULE m = GetModuleHandleA("renderdoc.dll"))
            if (auto get = reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(m, "RENDERDOC_GetAPI")))
                if (get(eRENDERDOC_API_Version_1_1_2, reinterpret_cast<void**>(&found)) != 1) found = nullptr;
        return found;
    }();
    return api;
}
u64 g_capture_frame = [] {
    char v[32] = {};
    return GetEnvironmentVariableA("CTR_CAPTURE", v, sizeof(v)) ? strtoull(v, nullptr, 10) : 0ull;
}();
double g_draw_ms = 0;  // time spent in GfxDraw since the last report
double g_map_ms = 0, g_cb_ms = 0, g_issue_ms = 0;
// where a draw can stall, since the last report (ms) and the worst single draw
double g_shader_ms = 0, g_build_ms = 0, g_refresh_ms = 0, g_buffer_ms = 0, g_lock_ms = 0, g_worst_draw = 0;
u64 g_builds = 0, g_build_bytes = 0, g_refresh_bytes = 0;
double g_sec[9];  // per-section draw profile
#define SECTION(n) do { double t_ = NowMs(); g_sec[n] += t_ - sec_t; sec_t = t_; } while (0)

// Read about twenty times per draw: the processor's counter, measured once against the system's.
double NowMs() {
    static const double ms_per_tick = [] {
        LARGE_INTEGER f, a, b;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&a);
        const u64 start = __rdtsc();
        do {
            QueryPerformanceCounter(&b);
        } while (b.QuadPart - a.QuadPart < f.QuadPart / 200);
        return (b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart / (double)(__rdtsc() - start);
    }();
    return __rdtsc() * ms_per_tick;
}
extern double g_worst_draw_ms;
struct DrawTimer {
    double t0 = NowMs();
    ~DrawTimer() {
        const double ms = NowMs() - t0;
        g_draw_ms += ms;
        g_worst_draw_ms = std::max(g_worst_draw_ms, ms);
    }
};
double g_worst_draw_ms = 0;

void DrainDebugMessages(int max = 16) {
    ComPtr<ID3D11InfoQueue> q;
    if (!g_dev || FAILED(g_dev.As(&q))) return;
    UINT64 n = q->GetNumStoredMessages();
    for (UINT64 i = 0; i < n && i < (UINT64)max; i++) {
        SIZE_T len = 0;
        q->GetMessage(i, nullptr, &len);
        std::vector<char> buf(len);
        auto* m = (D3D11_MESSAGE*)buf.data();
        if (SUCCEEDED(q->GetMessage(i, m, &len)) && m->Severity <= D3D11_MESSAGE_SEVERITY_WARNING &&
            !strstr(m->pDescription, "expects a Render Target View")) {
            LOG("d3d11: %.*s", (int)m->DescriptionByteLength, m->pDescription);
        }
    }
    q->ClearStoredMessages();
}

bool Readable(u64 a, u64 n) {
    return GuestReadable(a, n);
}

u64 Fnv(const u8* p, size_t n) {
    u64 h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

// Translated shaders (tools/pack_shaders.py): embedded in the exe (RCDATA "SHADERS"), or - in a
// release, which carries nothing of the game - shaders.bin made from the player's own dump by the
// first-start setup (first_start.cpp) in %LOCALAPPDATA%\CTRNF (0.1 releases: the game folder).
// A blob of another version than this exe's translator wants is made again.
constexpr u32 kShaderBlobVersion = 1;
std::unordered_map<std::string, std::pair<const u8*, u32>> g_embedded;
std::vector<u8> g_shader_file;

const u8* ShaderBlob(u32* total) {
    HRSRC r = FindResourceA(nullptr, "SHADERS", MAKEINTRESOURCEA(10) /* RT_RCDATA */);
    HGLOBAL g = r ? LoadResource(nullptr, r) : nullptr;
    const u8* p = g ? (const u8*)LockResource(g) : nullptr;
    *total = r ? SizeofResource(nullptr, r) : 0;
    if (p && *total >= 12 && memcmp(p, "CTRS", 4) == 0) return p;
    auto usable = [](const std::vector<u8>& v) {
        return v.size() >= 12 && memcmp(v.data(), "CTRS", 4) == 0 && R<u32>((u64)v.data() + 4) == kShaderBlobVersion;
    };
    if (!usable(g_shader_file)) g_shader_file.clear();
    *total = (u32)g_shader_file.size();
    return g_shader_file.empty() ? nullptr : g_shader_file.data();
}

std::string LoadEmbeddedShaders() {
    u32 total = 0;
    const u8* p = ShaderBlob(&total);
    if (!p) return {};
    u32 index_len = R<u32>((u64)p + 8);
    std::string index((const char*)p + 12, index_len);
    const u8* q = p + 12 + index_len;
    u32 count = R<u32>((u64)q);
    q += 4;
    for (u32 i = 0; i < count && q < p + total; i++) {
        u16 nl = R<u16>((u64)q);
        std::string name((const char*)q + 2, nl);
        u32 size = R<u32>((u64)q + 2 + nl);
        g_embedded[name] = {q + 6 + nl, size};
        q += 6 + nl + size;
    }
    return index;
}

void LoadShaderIndex() {
    char env[MAX_PATH] = {};
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    // CTR_SHADERS (a translated shader folder) overrides the shaders embedded in the exe
    std::string index;
    if (GetEnvironmentVariableA("CTR_SHADERS", env, MAX_PATH)) {
        g_shader_dir = fs::path(env);
        std::ifstream f(g_shader_dir / "shaders.idx");
        index.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    } else {
        index = LoadEmbeddedShaders();
        g_shader_dir = g_shader_file.empty() ? "<embedded>" : "<shaders.bin>";
    }
    std::istringstream f(index);
    std::string line;
    ShaderInfo* cur = nullptr;
    while (std::getline(f, line)) {
        std::istringstream is(line);
        char tag;
        is >> tag;
        if (tag == 'S') {
            std::string hash, name, kind;
            is >> hash >> name >> kind;
            u64 key = std::stoull(hash, nullptr, 16);
            if (g_shaders.count(key)) {
                cur = nullptr;  // identical code listed under another effect
                continue;
            }
            cur = &g_shaders[key];
            cur->name = name;
            cur->kind = kind;
            cur->vertex = kind == "vs" || kind == "es";
        } else if (cur && tag == 'T') {
            int s, d;
            is >> s >> d;
            cur->textures.push_back({s, d});
        } else if (cur && tag == 'M') {
            int s;
            std::string k;
            is >> s >> k;
            cur->samplers.push_back({s, k == "cmp"});
        } else if (cur && tag == 'B') {
            int s;
            is >> s;
            cur->cbuffers.push_back(s);
        } else if (cur && tag == 'I') {
            int s, n;
            is >> s >> n;
            cur->inputs.push_back({s, n});
        }
    }
    LOG("gfx: %zu translated shaders indexed from %s", g_shaders.size(), g_shader_dir.string().c_str());
}

const char* kBlitHlsl = R"(
Texture2D t0 : register(t0);
SamplerState s0 : register(s0);
void vsmain(uint id : SV_VertexID, out float4 pos : SV_Position, out float2 uv : TEXCOORD0) {
    uv = float2((id << 1) & 2, id & 2);
    pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
// Downsampling resolve of the supersampled frame: 4x4 bilinear taps spread over the source
// footprint of the output pixel, averaged in linear light (box filter, any scale ratio).
// The game's own gamma (sceVideoOutAdjustColor), as shadPS4's post process pass: the picture is
// sRGB encoded, so it goes back to linear and is encoded again with that gamma.
cbuffer present : register(b0) { float game_gamma; };
float3 degamma(float3 c) { return c < 0.04045 ? c / 12.92 : pow(max(c + 0.055, 0) / 1.055, 2.4); }
float3 regamma(float3 c) {
    return c < 0.0031308 ? 12.92 * c / game_gamma : 1.055 * pow(max(c, 0), 1 / (3.4 - game_gamma)) - 0.055;
}
float4 psmain(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float2 fp = float2(ddx(uv).x, ddy(uv).y);
    uint w, h;
    t0.GetDimensions(w, h);
    float2 ratio = abs(fp) * float2(w, h);
    float3 c = 0;
    if (max(ratio.x, ratio.y) <= 1.01) {
        c = t0.Sample(s0, uv).rgb;
    } else {
        [unroll] for (int y = 0; y < 4; y++)
            [unroll] for (int x = 0; x < 4; x++)
                c += pow(abs(t0.SampleLevel(s0, uv + fp * ((float2(x, y) + 0.5) / 4 - 0.5), 0).rgb), 2.2);
        c = pow(c / 16, 1 / 2.2);
    }
    return float4(game_gamma == 1 ? c : regamma(degamma(c)), 1);
}
// Texel copy between surfaces that CopySubresourceRegion cannot copy (depth into a color
// surface, different supersampling): source texel = target pixel * ratio + offset.
cbuffer copy : register(b0) { float2 copy_ratio; float2 copy_offset; };
float4 copyps(float4 pos : SV_Position) : SV_Target {
    return t0.Load(int3(floor(pos.xy) * copy_ratio + copy_offset, 0));
}
)";

bool g_ready = false;
void PreloadShaders();
void ForgetBound();

bool Init() {
    if (g_ready) return true;
    if (g_init_failed || !g_dev) return false;
    g_ready = true;
    {
        D3D11_FEATURE_DATA_D3D11_OPTIONS o{};
        if (SUCCEEDED(g_dev->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &o, sizeof(o))) &&
            o.ConstantBufferOffsetting)
            g_ctx.As(&g_ctx1);
        LOG("gfx: constant buffer offsetting %s", g_ctx1 ? "on" : "off");
    }

    D3D11_BUFFER_DESC bd{};
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bd.ByteWidth = kVbRing;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    g_dev->CreateBuffer(&bd, nullptr, &g_vb_ring);
    bd.ByteWidth = kIbRing;
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    g_dev->CreateBuffer(&bd, nullptr, &g_ib_ring);
    D3D11_BUFFER_DESC zd{};
    zd.ByteWidth = 64;
    zd.Usage = D3D11_USAGE_DEFAULT;
    zd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    // Inputs no engine element feeds: zeros, and white at +16 for a missing vertex color, as the
    // native Vulkan renderer found (a mesh without colors is not a black mesh).
    float zeros[16] = {0, 0, 0, 1, 1, 1, 1, 1};
    D3D11_SUBRESOURCE_DATA zinit{zeros};
    g_dev->CreateBuffer(&zd, &zinit, &g_zero_vb);

    D3D11_SAMPLER_DESC smp{};
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    smp.MaxLOD = D3D11_FLOAT32_MAX;
    g_dev->CreateSamplerState(&smp, &g_samp_linear);
    smp.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    smp.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
    g_dev->CreateSamplerState(&smp, &g_samp_cmp);
    D3D11_DEPTH_STENCIL_DESC dsd{};
    g_dev->CreateDepthStencilState(&dsd, &g_depth_off);

    // 1x1 grey placeholder for textures that are not converted yet
    D3D11_TEXTURE2D_DESC td{};
    td.Width = td.Height = 1;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    u32 grey = 0xFF808080;
    D3D11_SUBRESOURCE_DATA tinit{&grey, 4};
    g_dev->CreateTexture2D(&td, &tinit, &g_placeholder.tex);
    g_dev->CreateShaderResourceView(g_placeholder.tex.Get(), nullptr, &g_placeholder.srv);
    D3D11_SHADER_RESOURCE_VIEW_DESC av{};
    av.Format = td.Format;
    av.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    av.Texture2DArray.MipLevels = 1;
    av.Texture2DArray.ArraySize = 1;
    g_dev->CreateShaderResourceView(g_placeholder.tex.Get(), &av, &g_placeholder.srv_array);

    ComPtr<ID3DBlob> vsb, psb, cpb, err;
    D3DCompile(kBlitHlsl, strlen(kBlitHlsl), "blit", nullptr, nullptr, "vsmain", "vs_5_0", 0, 0, &vsb, &err);
    D3DCompile(kBlitHlsl, strlen(kBlitHlsl), "blit", nullptr, nullptr, "psmain", "ps_5_0", 0, 0, &psb, &err);
    D3DCompile(kBlitHlsl, strlen(kBlitHlsl), "blit", nullptr, nullptr, "copyps", "ps_5_0", 0, 0, &cpb, &err);
    if (vsb && psb && cpb) {
        g_dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g_blit_vs);
        g_dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_blit_ps);
        g_dev->CreatePixelShader(cpb->GetBufferPointer(), cpb->GetBufferSize(), nullptr, &g_copy_ps);
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = 16;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        g_dev->CreateBuffer(&bd, nullptr, &g_copy_cb);
    }
    LoadShaderIndex();
    PreloadShaders();
    ForgetBound();
    LOG("gfx: D3D11 device ready");
    return true;
}

// ------------------------------------------------------------------ shaders
// Saves a shader that has no ahead-of-time translation (code + Gnm struct) so that
// tools/gcn2hlsl.py can translate it: missing_shaders/Rt_<hash>_<vs|ps>.bin/.gnm
void DumpMissingShader(u64 handle, u64 code, u32 size, u64 hash, bool vertex) {
    const fs::path dir = UserRoot() / "missing_shaders";  // collected for translation
    std::error_code ec;
    fs::create_directories(dir, ec);
    char leaf[64];
    snprintf(leaf, sizeof(leaf), "Rt_%016llx_%s", (unsigned long long)hash, vertex ? "vs" : "ps");
    const std::string name = (dir / leaf).string();
    if (FILE* f = fopen((std::string(name) + ".bin").c_str(), "wb")) {
        fwrite((const void*)code, 1, size, f);
        fclose(f);
    }
    u64 gnm = Q(handle + SH_GNM);
    if (Readable(gnm, 0x200)) {
        if (FILE* f = fopen((std::string(name) + ".gnm").c_str(), "wb")) {
            fwrite((const void*)gnm, 1, 0x200, f);
            fclose(f);
        }
    }
}

// The texture slots a DXBC declares as TextureCube, from its resource bindings (RDEF chunk): a
// slot that is sampled through the wrong kind of view reads nothing.
u32 CubeSlots(const std::vector<u8>& code) {
    const auto word = [&](size_t at) { return at + 4 <= code.size() ? R<u32>((u64)code.data() + at) : 0u; };
    if (code.size() < 32 || memcmp(code.data(), "DXBC", 4) != 0) return 0;
    u32 slots = 0;
    for (u32 i = 0, chunks = word(28); i < chunks; i++) {
        const size_t chunk = word(32 + i * 4);
        if (chunk + 8 > code.size() || memcmp(code.data() + chunk, "RDEF", 4) != 0) continue;
        const size_t rdef = chunk + 8, bindings = rdef + word(rdef + 12);
        for (u32 k = 0, count = word(rdef + 8); k < count; k++) {
            const size_t b = bindings + k * 32;  // name, type, return type, dimension, samples, point...
            if (word(b + 4) == 2 && word(b + 12) == 9 && word(b + 20) < 32) slots |= 1u << word(b + 20);
        }
    }
    return slots;
}

// A translation compiled here, the first time it is needed (the build at start only translates:
// GCN -> HLSL takes about 20 ms a shader, fxc about 1.5 s, 3 times less without its optimizer, which
// the GPU driver's own compiler makes up for). The result is kept next to the HLSL as <name>.dxbc,
// written whole (a temporary file renamed), so the next start finds it.
std::atomic<int> g_compiled{0};
std::atomic<int> g_compile_failed{0};
double g_compile_t0 = 0;

bool CompileTranslation(ShaderInfo* info) {
    if (g_shader_dir.empty() || g_shader_dir.string()[0] == '<') return false;  // embedded: nothing to compile
    std::ifstream src(g_shader_dir / (info->name + ".hlsl"), std::ios::binary);
    if (!src) return false;
    const std::string text((std::istreambuf_iterator<char>(src)), std::istreambuf_iterator<char>());
    const char* target = info->kind == "gs" ? "gs_5_0" : info->vertex ? "vs_5_0" : "ps_5_0";
    // Pixel shaders with the optimizer, as tools/shaders/compile_d3d11.py: unoptimized, the GPU driver
    // ran some wrong (wrong colours in the post process, vegetation killed at its alpha test).
    const bool optimized = info->kind != "gs" && !info->vertex;
    const UINT flags = (optimized ? D3DCOMPILE_OPTIMIZATION_LEVEL0 : D3DCOMPILE_SKIP_OPTIMIZATION) |
                       D3DCOMPILE_ENABLE_STRICTNESS;
    ComPtr<ID3DBlob> code, err;
    const HRESULT hr = D3DCompile(text.data(), text.size(), info->name.c_str(), nullptr, nullptr, "main", target,
                                  flags, 0, &code, &err);
    if (FAILED(hr) || !code) {
        if (g_compile_failed++ < 20)
            LOG("gfx: %s does not compile: %s", info->name.c_str(),
                err ? std::string((const char*)err->GetBufferPointer(), err->GetBufferSize()).substr(0, 160).c_str() : "?");
        return false;
    }
    const u8* p = (const u8*)code->GetBufferPointer();
    info->bytecode.assign(p, p + code->GetBufferSize());
    std::error_code ec;
    const fs::path out = g_shader_dir / (info->name + ".dxbc");
    const fs::path tmp = g_shader_dir / (info->name + ".dxbc.tmp" + std::to_string(GetCurrentThreadId()));
    {
        std::ofstream f(tmp, std::ios::binary);
        f.write((const char*)p, (std::streamsize)code->GetBufferSize());
    }
    fs::rename(tmp, out, ec);
    if (ec) fs::remove(tmp, ec);
    const int n = ++g_compiled;
    if (n == 1 || n % 200 == 0) LOG("gfx: %d shaders compiled here (%.0f s)", n, (NowMs() - g_compile_t0) / 1000.0);
    return true;
}

// Reads one shader and makes its object. Device methods are free threaded: the preload threads
// do this for every shader, so that no draw waits for the driver's compiler.
void LoadShader(ShaderInfo* info) {
    info->video = info->name == "Rt_6d0ae01fb1b6a3de_ps" || info->name == "Rt_06414427fa62da56_ps";
    if (info->bytecode.empty()) {
        // a shader written for the PC in place of the translation, where there is one
        // (tools/shaders/native_d3d11.py; BB_D3D11_PC_SHADERS=0: the translations)
        static const bool pc_shaders = [] {
            const char* env = std::getenv("BB_D3D11_PC_SHADERS");
            return !(env && env[0] == '0');
        }();
        std::ifstream in;
        if (pc_shaders) in.open(g_shader_dir / (info->name + ".pc.dxbc"), std::ios::binary);
        if (in.is_open()) {
            LOG("gfx: %s: the PC shader in place of the translation", info->name.c_str());
        } else {
            in.clear();
            in.open(g_shader_dir / (info->name + ".dxbc"), std::ios::binary);
        }
        info->bytecode.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        if (info->bytecode.empty() && !CompileTranslation(info)) return;  // no draw uses it
    }
    HRESULT hr =
        info->kind == "gs"
            ? g_dev->CreateGeometryShader(info->bytecode.data(), info->bytecode.size(), nullptr, &info->gs)
        : info->vertex
            ? g_dev->CreateVertexShader(info->bytecode.data(), info->bytecode.size(), nullptr, &info->vs)
            : g_dev->CreatePixelShader(info->bytecode.data(), info->bytecode.size(), nullptr, &info->ps);
    if (FAILED(hr)) LOG("gfx: cannot create shader %s (0x%08lx)", info->name.c_str(), hr);
    info->cube_slots = CubeSlots(info->bytecode);
    if (SUCCEEDED(hr)) info->ready.store(true, std::memory_order_release);
}

// Draws waiting for a shader right now (ReadyOrQueue): the preload threads stand aside meanwhile.
std::atomic<int> g_draws_waiting{0};

// Loads (and compiles, the first time: CompileTranslation) every shader in the background, below
// normal priority: first in the order the game uses them (BB_D3D11_ORDER, run.py:
// tools/shaders/d3d11_order.txt), then the rest. A few threads only (the game runs meanwhile), which
// stand aside while a draw waits; a draw that needs a shader sooner makes it itself, or waits for
// the thread already on it (ReadyOrQueue).
void PreloadShaders() {
    static std::vector<ShaderInfo*> list;
    static std::atomic<size_t> next{0};
    static std::atomic<unsigned> running{0};
    {
        std::unordered_map<std::string, ShaderInfo*> by_name;
        for (auto& [hash, info] : g_shaders) by_name[info.name] = &info;
        std::unordered_set<ShaderInfo*> listed;
        if (const char* order = std::getenv("BB_D3D11_ORDER")) {
            std::ifstream in(order);
            for (std::string line; std::getline(in, line);) {
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
                auto it = by_name.find(line);
                if (it != by_name.end() && listed.insert(it->second).second) list.push_back(it->second);
            }
        }
        const size_t ordered = list.size();
        for (auto& [hash, info] : g_shaders)
            if (!listed.count(&info)) list.push_back(&info);
        LOG("gfx: %zu shaders, %zu of them in the order the game uses them", list.size(), ordered);
    }
    g_compile_t0 = NowMs();
    const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
    const unsigned threads = std::max(1u, std::min(4u, cores / 2 > 1 ? cores / 2 - 1 : 1u));
    running = threads;
    for (unsigned t = 0; t < threads; t++) {
        std::thread([] {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
            const double t0 = NowMs();
            for (;;) {
                while (g_draws_waiting.load(std::memory_order_relaxed) > 0) Sleep(2);
                ShaderInfo* info = nullptr;
                {
                    const size_t i = next.fetch_add(1);
                    if (i >= list.size()) break;
                    info = list[i];
                }
                std::call_once(info->once, LoadShader, info);
            }
            if (--running == 0)
                LOG("gfx: every shader ready after %.1f s (%d compiled here, %d failed)", (NowMs() - t0) / 1000.0,
                    g_compiled.load(), g_compile_failed.load());
        }).detach();
    }
}

// A shader a draw needs now. Not ready yet (v0.8.8: the game compiles its shaders as it runs, none
// before it starts): loaded and compiled here, or waited for while a preload thread does it. The draw
// waits, a hitch the first time; left out instead, surfaces went missing and the picture flickered.
// The order in which the game first uses its shaders, appended to the user folder's
// d3d11-shader-order.txt (frame and name): from players' sessions comes the list of what to compile
// first, before the game starts and then in the background.
void NoteFirstUse(const ShaderInfo* info) {
    static FILE* f = []() -> FILE* {
        const fs::path path = UserRoot() / "d3d11-shader-order.txt";
        std::error_code ec;
        if (fs::exists(path, ec) && fs::file_size(path, ec) > (2u << 20)) return nullptr;  // enough sessions
        FILE* file = _wfopen(path.c_str(), L"a");
        if (file) fprintf(file, "# session\n");
        return file;
    }();
    if (!f) return;
    fprintf(f, "%llu %s\n", (unsigned long long)g_frame, info->name.c_str());
    fflush(f);
}

ShaderInfo* ReadyOrQueue(ShaderInfo* info) {
    if (!info) return nullptr;
    if (!info->used.exchange(true)) NoteFirstUse(info);
    if (info->ready.load(std::memory_order_acquire)) return info;
    const double t0 = NowMs();
    g_draws_waiting++;
    std::call_once(info->once, LoadShader, info);
    g_draws_waiting--;
    const double waited = NowMs() - t0;
    g_shader_ms += waited;
    static int told = 0;
    if (waited > 100 && told++ < 20) LOG("gfx: a draw waited %.0f ms for %s", waited, info->name.c_str());
    return info->ready.load(std::memory_order_acquire) ? info : nullptr;
}

// Which program is at `code` now: the shader hash of its OrbShdr footer (the program's second
// dword is the distance to it), else a hash of its first bytes; with its size.
u64 ProgramFingerprint(u64 code, u32 size) {
    if (size >= 8 && R<u32>(code) == 0xBEEB03FFu) {
        const u64 footer = code + ((u64)R<u32>(code + 4) + 1) * 8;
        if (footer + 24 <= code + size + 64 && Readable(footer, 24) && memcmp((const void*)footer, "OrbShdr", 7) == 0)
            return R<u64>(footer + 16) ^ ((u64)size << 40);
    }
    return Fnv((const u8*)code, std::min<u32>(size, 64)) ^ ((u64)size << 40);
}

// The translated shader of a bound engine shader, cached by program address and checked against
// the program there: the game unloads shader packages and loads others at the same addresses
// when it changes screens (the track select's preview video drew with the title screen's shader).
ShaderInfo* GetShader(u64 handle, bool vertex) {
    if (!handle || !Readable(handle, SH_GNM + 8) || !Readable(Q(handle + SH_GNM), 16)) return nullptr;
    u32 size;
    u64 code = ShaderCode(handle, &size);
    const u64 fingerprint = size && Readable(code, size) ? ProgramFingerprint(code, size) : 0;
    auto it = g_shader_by_code.find(code);
    if (it != g_shader_by_code.end()) {
        if (it->second.fingerprint == fingerprint) return ReadyOrQueue(it->second.info);
        static int replaced = 0;
        if (replaced++ < 40)
            LOG("gfx: another program at %llx now (%s before)", (unsigned long long)code,
                it->second.info ? it->second.info->name.c_str() : "none");
    }
    ShaderInfo* info = nullptr;
    if (size && Readable(code, size)) {
        u64 hash = Fnv((const u8*)code, size);
        auto f = g_shaders.find(hash);
        if (f != g_shaders.end()) info = &f->second;
        else DumpMissingShader(handle, code, size, hash, vertex);
    }
    if (!info) {
        LOG("gfx: no translated shader for code %llx size %u", (unsigned long long)code, size);
    }
    g_shader_by_code[code] = CachedProgram{info, fingerprint};
    return ReadyOrQueue(info);
}

// ------------------------------------------------------------------ surfaces
u64 g_surface_generation = 1;  // moves when a texture address may now mean another surface

bool Current(const Surface& s) {
    return !s.written_over || !s.written_over->load(std::memory_order_acquire);
}

// The GPU draws to a surface: its memory is its own again, and watched.
void DrawnTo(u64 base, Surface& s) {
    if (!s.written_over) {
        s.written_over = std::make_shared<std::atomic<bool>>(false);
    } else if (s.written_over->exchange(false, std::memory_order_relaxed)) {
        static int reclaimed = 0;
        if (reclaimed++ < 16) LOG("gfx: target %llx drawn to again", (unsigned long long)base);
    } else {
        return;
    }
    g_surface_generation++;
    GuestWatchTarget(base, s.bytes, s.written_over);
}

// The color surface at `base` (a render target, or the destination of a texture copy), created
// or re-created when its size or format changes.
Surface* MakeColorSurface(u64 base, u32 w, u32 hh, DXGI_FORMAT fmt, float scale, u32 comp_swap, const char* what) {
    Surface& s = g_surfaces[base];
    if (s.tex && s.w == w && s.h == hh && s.fmt == fmt && s.rtv) {
        DrawnTo(base, s);
        return &s;
    }
    s = Surface{};
    s.scale = scale;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = Scaled(w, s.scale);
    td.Height = Scaled(hh, s.scale);
    td.MipLevels = td.ArraySize = 1;
    td.Format = TypelessOf(fmt);
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &s.tex))) return nullptr;
    D3D11_RENDER_TARGET_VIEW_DESC rd{};
    rd.Format = fmt;
    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    g_dev->CreateRenderTargetView(s.tex.Get(), &rd, &s.rtv);
    D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = fmt == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : fmt;
    sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MipLevels = 1;
    g_dev->CreateShaderResourceView(s.tex.Get(), &sv, &s.srv);
    sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    sv.Texture2DArray.MipLevels = 1;
    sv.Texture2DArray.ArraySize = 1;
    g_dev->CreateShaderResourceView(s.tex.Get(), &sv, &s.srv_array);
    s.w = w;
    s.h = hh;
    s.fmt = fmt;
    s.comp_swap = comp_swap;
    bool block_compressed;
    s.bytes = (u64)w * hh * FormatBytes(fmt, &block_compressed);
    DrawnTo(base, s);
    LOG("gfx: %s %llx %ux%u (x%.2f) format %d swap %u", what, (unsigned long long)base, w, hh, s.scale, (int)fmt,
        s.comp_swap);
    return &s;
}

// A target resolved before: draws keep the same few targets for hundreds of draws. What the engine
// object says and what the surface is now are both checked (a copy can remake a surface).
struct TargetMemo {
    u64 handle = 0, base = 0;
    u32 words[4] = {};
    Surface* surface = nullptr;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    u32 w = 0, h = 0;
    Surface* Hit(u64 handle_now, const u32* now) const {
        if (handle != handle_now || memcmp(words, now, sizeof(words)) != 0 || !surface) return nullptr;
        return surface->fmt == fmt && surface->w == w && surface->h == h ? surface : nullptr;
    }
    void Set(u64 handle_now, u64 base_now, const u32* now, Surface* s) {
        handle = handle_now;
        base = base_now;
        memcpy(words, now, sizeof(words));
        surface = s;
        fmt = s->fmt;
        w = s->w;
        h = s->h;
    }
};
TargetMemo g_color_memo[64], g_depth_memo[64];

Surface* GetColorTarget(u64 h) {
    if (!Readable(h, 0x50)) return nullptr;
    // Gnm::RenderTarget registers start at +0x10: CB_COLOR_BASE, PITCH, SLICE, VIEW, INFO, ATTRIB
    u64 base = (u64)R<u32>(h + 0x10) << 8;
    u32 info = R<u32>(h + 0x20);
    u32 wh = R<u32>(h + 0x4c);
    const u32 words[4] = {(u32)(base >> 8), info, wh, 0};
    TargetMemo& memo = g_color_memo[((h >> 4) ^ (h >> 12)) & 63];
    if (Surface* known = memo.Hit(h, words); known && known->rtv) {
        DrawnTo(memo.base, *known);
        return known;
    }
    u32 w = wh & 0xFFFF, hh = wh >> 16;
    DXGI_FORMAT fmt = ColorTargetFormat(info);
    if (!base || !w || !hh || fmt == DXGI_FORMAT_UNKNOWN) {
        static int warned = 0;
        if (warned++ < 8)
            LOG("gfx: unusable color target handle %llx base %llx info %x size %ux%u", (unsigned long long)h,
                (unsigned long long)base, info, w, hh);
        return nullptr;
    }
    Surface* made = MakeColorSurface(base, w, hh, fmt, SurfaceScale(w, hh), (info >> 11) & 3, "render target");
    if (made) memo.Set(h, base, words, made);
    return made;
}

// Gnm::DepthRenderTarget: +0x50 DB_Z_INFO, +0x54 DB_STENCIL_INFO, +0x58 DB_Z_READ_BASE,
// +0x5c DB_STENCIL_READ_BASE, +0x60/64 write bases, +0x68 DB_DEPTH_SIZE, +0x80 width|height<<16
Surface* GetDepthTarget(u64 h) {
    if (!Readable(h, 0x84)) return nullptr;
    u64 base = (u64)R<u32>(h + 0x58) << 8;
    u32 wh = R<u32>(h + 0x80);
    const u32 words[4] = {(u32)(base >> 8), wh, R<u32>(h + 0x50), R<u32>(h + 0x54)};
    TargetMemo& memo = g_depth_memo[((h >> 4) ^ (h >> 12)) & 63];
    if (Surface* known = memo.Hit(h, words); known && known->dsv) {
        DrawnTo(memo.base, *known);
        return known;
    }
    u32 w = wh & 0xFFFF, hh = wh >> 16;
    if (!base || !w || !hh) return nullptr;
    // DB_Z_INFO.FORMAT 1 = Z_16 (shadow maps: their depth bias is set in 16-bit units, so they
    // must be 16-bit here too), else 32-bit float; DB_STENCIL_INFO.FORMAT 1 = 8-bit stencil
    const bool z16 = (R<u32>(h + 0x50) & 3) == 1 && (R<u32>(h + 0x54) & 1) == 0;
    const DXGI_FORMAT dfmt = z16 ? DXGI_FORMAT_D16_UNORM : DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    const float scale = z16 ? ShadowMapScale(w, hh) : SurfaceScale(w, hh);
    Surface& s = g_surfaces[base];
    if (s.tex && s.w == w && s.h == hh && s.dsv && s.fmt == dfmt && s.scale == scale) {
        DrawnTo(base, s);
        memo.Set(h, base, words, &s);
        return &s;
    }
    s = Surface{};
    s.scale = scale;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = Scaled(w, s.scale);
    td.Height = Scaled(hh, s.scale);
    td.MipLevels = td.ArraySize = 1;
    td.Format = z16 ? DXGI_FORMAT_R16_TYPELESS : DXGI_FORMAT_R32G8X24_TYPELESS;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &s.tex))) return nullptr;
    D3D11_DEPTH_STENCIL_VIEW_DESC dd{};
    dd.Format = dfmt;
    dd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    g_dev->CreateDepthStencilView(s.tex.Get(), &dd, &s.dsv);
    dd.Flags = D3D11_DSV_READ_ONLY_DEPTH | (z16 ? 0 : D3D11_DSV_READ_ONLY_STENCIL);
    g_dev->CreateDepthStencilView(s.tex.Get(), &dd, &s.dsv_ro);
    D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = z16 ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MipLevels = 1;
    g_dev->CreateShaderResourceView(s.tex.Get(), &sv, &s.srv);
    sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    sv.Texture2DArray.MipLevels = 1;
    sv.Texture2DArray.ArraySize = 1;
    g_dev->CreateShaderResourceView(s.tex.Get(), &sv, &s.srv_array);
    s.w = w;
    s.h = hh;
    s.fmt = dfmt;
    s.bytes = (u64)w * hh * (z16 ? 2 : 4);
    DrawnTo(base, s);
    memo.Set(h, base, words, &s);
    LOG("gfx: depth target %llx %ux%u (x%.2f)%s", (unsigned long long)base, w, hh, s.scale, z16 ? " 16-bit" : "");
    return &s;
}

D3D11_COMPARISON_FUNC Cmp(u32 f) {
    static const D3D11_COMPARISON_FUNC t[] = {
        D3D11_COMPARISON_NEVER, D3D11_COMPARISON_LESS, D3D11_COMPARISON_EQUAL, D3D11_COMPARISON_LESS_EQUAL,
        D3D11_COMPARISON_GREATER, D3D11_COMPARISON_NOT_EQUAL, D3D11_COMPARISON_GREATER_EQUAL,
        D3D11_COMPARISON_ALWAYS};
    return t[f & 7];
}

D3D11_STENCIL_OP StencilOp(u32 op) {
    switch (op) {
    case 1: return D3D11_STENCIL_OP_ZERO;
    case 2:  // ONES
    case 3:  // REPLACE_TEST
    case 4: return D3D11_STENCIL_OP_REPLACE;  // REPLACE_OP
    case 5: return D3D11_STENCIL_OP_INCR_SAT;
    case 6: return D3D11_STENCIL_OP_DECR_SAT;
    case 7: return D3D11_STENCIL_OP_INVERT;
    case 8: return D3D11_STENCIL_OP_INCR;
    case 9: return D3D11_STENCIL_OP_DECR;
    default: return D3D11_STENCIL_OP_KEEP;
    }
}

// DB_DEPTH_CONTROL: STENCIL_ENABLE [0] Z_ENABLE [1] Z_WRITE_ENABLE [2] ZFUNC [6:4]
//   BACKFACE_ENABLE [7] STENCILFUNC [10:8] STENCILFUNC_BF [22:20]
// DB_STENCILREFMASK (ctx+0x114): TESTVAL [7:0] MASK [15:8] WRITEMASK [23:16] OPVAL [31:24]
// DB_STENCIL_CONTROL (ctx+0x118): FAIL [3:0] ZPASS [7:4] ZFAIL [11:8] and back-face copies
ID3D11DepthStencilState* GetDepthState(u32 ctl, u32 refmask, u32 ops) {
    bool stencil = ctl & 1;
    u64 key = (u64)(ctl & (stencil ? 0x7007F7 : 0x76)) | (stencil ? ((u64)(refmask & 0xFFFF00) << 24) | ((u64)(ops & 0xFFFFFF) << 48) : 0);
    auto& d = g_depth[key];
    if (d) return d.Get();
    D3D11_DEPTH_STENCIL_DESC desc{};
    desc.DepthEnable = (ctl >> 1) & 1;
    desc.DepthWriteMask = (ctl >> 2) & 1 ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    desc.DepthFunc = Cmp((ctl >> 4) & 7);
    desc.StencilEnable = stencil;
    desc.StencilReadMask = (refmask >> 8) & 0xFF;
    desc.StencilWriteMask = (refmask >> 16) & 0xFF;
    desc.FrontFace = {StencilOp(ops & 0xF), StencilOp((ops >> 8) & 0xF), StencilOp((ops >> 4) & 0xF),
                      Cmp((ctl >> 8) & 7)};
    desc.BackFace = (ctl >> 7) & 1 ? D3D11_DEPTH_STENCILOP_DESC{StencilOp((ops >> 12) & 0xF), StencilOp((ops >> 20) & 0xF),
                                                              StencilOp((ops >> 16) & 0xF), Cmp((ctl >> 20) & 7)}
                                   : desc.FrontFace;
    g_dev->CreateDepthStencilState(&desc, &d);
    return d.Get();
}

constexpr int kDimsCube = 4;  // a slot the shader declares TextureCube

ID3D11ShaderResourceView* GetTexture(const u32* tsharp, int dims) {
    if (!tsharp) return dims == kDimsCube ? nullptr : dims == 3 ? g_placeholder.srv_array.Get() : g_placeholder.srv.Get();
    TSharp t = DecodeTSharp(tsharp);
    auto it = g_surfaces.find(t.base);
    if (it != g_surfaces.end() && it->second.srv) {
        if (Current(it->second)) {
            if (dims == kDimsCube) return nullptr;  // no cube targets
            return dims == 3 ? it->second.srv_array.Get() : it->second.srv.Get();
        }
        static int passed_over = 0;
        if (passed_over++ < 16)
            LOG("gfx: target %llx written over by the game: read from memory", (unsigned long long)t.base);
    }
    u64 key = Fnv((const u8*)tsharp, 24);
    auto tx = g_textures.find(key);
    if (tx == g_textures.end()) {
        const double t0 = NowMs();
        TextureObject obj;
        if (!BuildTexture(g_dev.Get(), t, &obj)) {
            static std::unordered_set<u64> warned;  // each texture once
            if (warned.size() < 200 && warned.insert(t.base).second) {
                // the nearest render / depth target below it (a view into a target?)
                u64 below = 0;
                for (const auto& [b, s] : g_surfaces)
                    if (b <= t.base && b > below) below = b;
                const Surface* bs = below ? &g_surfaces[below] : nullptr;
                LOG("gfx: texture not converted: tiling %u type %u dfmt %u nfmt %u %ux%u pitch %u base %llx "
                    "(target below: %llx %ux%u, +0x%llx)",
                    t.tiling_index, t.type, t.dfmt, t.nfmt, t.width, t.height, t.pitch, (unsigned long long)t.base,
                    (unsigned long long)below, bs ? bs->w : 0, bs ? bs->h : 0, (unsigned long long)(t.base - below));
            }
        }
        tx = g_textures.emplace(key, std::move(obj)).first;
        g_build_ms += NowMs() - t0;
        g_builds++;
        g_build_bytes += tx->second.mem_size;
    }
    else if (tx->second.srv && tx->second.checked_frame != g_frame) {
        // CPU-written textures (video frames, runtime atlases): rebuild when the guest wrote them
        tx->second.checked_frame = g_frame;
        if (const u64 writes = GuestWriteCount(); tx->second.seen_writes != writes) {
            const double t0 = NowMs();
            const bool written = GuestWrittenSince(tx->second.mem_base, tx->second.mem_size, false);
            if (written && RefreshTexture(g_ctx.Get(), t, &tx->second)) {
                g_stats.texture_refresh++;
                g_refresh_bytes += tx->second.mem_size;
                tx->second.seen_writes = writes;
            } else if (!written) {
                tx->second.seen_writes = writes;
            }
            g_refresh_ms += NowMs() - t0;
        }
    }
    tx->second.used_frame = g_frame;
    if (dims == kDimsCube) return tx->second.srv_cube.Get();
    if (tx->second.button_atlas && dims != 3)
        if (ID3D11ShaderResourceView* glyphs = ButtonAtlasOverride(g_dev.Get(), tx->second.fmt)) return glyphs;
    if (tx->second.srv) return dims == 3 ? tx->second.srv_array.Get() : tx->second.srv.Get();
    return dims == 3 ? g_placeholder.srv_array.Get() : g_placeholder.srv.Get();
}

// ------------------------------------------------------------------ fixed function state
D3D11_BLEND Blend(u32 f) {
    static const D3D11_BLEND t[] = {
        D3D11_BLEND_ZERO, D3D11_BLEND_ONE, D3D11_BLEND_SRC_COLOR, D3D11_BLEND_INV_SRC_COLOR,
        D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_DEST_ALPHA,
        D3D11_BLEND_INV_DEST_ALPHA, D3D11_BLEND_DEST_COLOR, D3D11_BLEND_INV_DEST_COLOR,
        D3D11_BLEND_SRC_ALPHA_SAT, D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_BLEND_FACTOR,
        D3D11_BLEND_INV_BLEND_FACTOR, D3D11_BLEND_SRC1_COLOR, D3D11_BLEND_INV_SRC1_COLOR,
        D3D11_BLEND_SRC1_ALPHA, D3D11_BLEND_INV_SRC1_ALPHA, D3D11_BLEND_BLEND_FACTOR,
        D3D11_BLEND_INV_BLEND_FACTOR};
    return f < 21 ? t[f] : D3D11_BLEND_ONE;
}
D3D11_BLEND_OP BlendOp(u32 f) {
    switch (f) {
    case 1: return D3D11_BLEND_OP_SUBTRACT;
    case 2: return D3D11_BLEND_OP_MIN;
    case 3: return D3D11_BLEND_OP_MAX;
    case 4: return D3D11_BLEND_OP_REV_SUBTRACT;
    default: return D3D11_BLEND_OP_ADD;
    }
}

ID3D11BlendState* GetBlend(u32 ctl, u32 mask) {
    u64 key = ((u64)mask << 32) | ctl;
    auto& b = g_blend[key];
    if (b) return b.Get();
    D3D11_BLEND_DESC d{};
    d.IndependentBlendEnable = TRUE;
    for (int i = 0; i < 8; i++) {
        auto& rt = d.RenderTarget[i];
        rt.BlendEnable = (ctl >> 30) & 1;
        rt.SrcBlend = Blend(ctl & 0x1F);
        rt.BlendOp = BlendOp((ctl >> 5) & 7);
        rt.DestBlend = Blend((ctl >> 8) & 0x1F);
        if ((ctl >> 29) & 1) {
            rt.SrcBlendAlpha = Blend((ctl >> 16) & 0x1F);
            rt.BlendOpAlpha = BlendOp((ctl >> 21) & 7);
            rt.DestBlendAlpha = Blend((ctl >> 24) & 0x1F);
        } else {
            rt.SrcBlendAlpha = rt.SrcBlend;
            rt.BlendOpAlpha = rt.BlendOp;
            rt.DestBlendAlpha = rt.DestBlend;
        }
        // alpha factors may not reference colour
        auto fix = [](D3D11_BLEND x) {
            switch (x) {
            case D3D11_BLEND_SRC_COLOR: return D3D11_BLEND_SRC_ALPHA;
            case D3D11_BLEND_INV_SRC_COLOR: return D3D11_BLEND_INV_SRC_ALPHA;
            case D3D11_BLEND_DEST_COLOR: return D3D11_BLEND_DEST_ALPHA;
            case D3D11_BLEND_INV_DEST_COLOR: return D3D11_BLEND_INV_DEST_ALPHA;
            case D3D11_BLEND_SRC1_COLOR: return D3D11_BLEND_SRC1_ALPHA;
            case D3D11_BLEND_INV_SRC1_COLOR: return D3D11_BLEND_INV_SRC1_ALPHA;
            default: return x;
            }
        };
        rt.SrcBlendAlpha = fix(rt.SrcBlendAlpha);
        rt.DestBlendAlpha = fix(rt.DestBlendAlpha);
        rt.RenderTargetWriteMask = (mask >> (i * 4)) & 0xF;
    }
    g_dev->CreateBlendState(&d, &b);
    return b.Get();
}

// `rkey`: the engine's packed rasterizer state (context +0x190): bits 0-1 fill, 2-3 cull,
// bit 4 sign + bits 5-30 polygon offset scale (PA_SU_POLY_OFFSET_*_SCALE, 1/16 units),
// bits 31-62 the offset (float bits). Decals use it against z-fighting.
ID3D11RasterizerState* GetRaster(u32 setup, bool scissor, u64 rkey = 0, float scale = 1.0f) {
    bool offset_on = (setup >> 11) & 3;
    if (!offset_on) rkey = 0;
    u64 key = Fnv((const u8*)&rkey, 8) ^ ((u64)scissor << 32) ^ setup ^ ((u64)(scale * 64) << 40);
    auto& r = g_raster[key];
    if (r) return r.Get();
    D3D11_RASTERIZER_DESC d{};
    if (rkey) {
        float slope = (float)((rkey >> 5) & 0x3FFFFFF) * ((rkey & 0x10) ? -1.0f : 1.0f);
        u32 obits = (u32)(rkey >> 31);
        float units;
        memcpy(&units, &obits, 4);
        // GCN scales the slope factor by 16; supersampled targets halve the per-pixel slope
        d.SlopeScaledDepthBias = slope / 16.0f * scale;
        d.DepthBias = (INT)units;
    }
    d.FillMode = D3D11_FILL_SOLID;
    bool cf = setup & 1, cb = (setup >> 1) & 1;
    d.CullMode = cf && cb ? D3D11_CULL_NONE : cf ? D3D11_CULL_FRONT : cb ? D3D11_CULL_BACK : D3D11_CULL_NONE;
    d.FrontCounterClockwise = ((setup >> 2) & 1) == 0;
    d.DepthClipEnable = TRUE;
    d.ScissorEnable = scissor;
    g_dev->CreateRasterizerState(&d, &r);
    return r.Get();
}

// ------------------------------------------------------------------ dynamic geometry
// A Map that fails is usually the first sign of a lost device: Present stops the game a moment
// later, and this says where it was seen and why.
void NoteMapFailure(const char* what, HRESULT result) {
    static bool said = false;
    if (said) return;
    said = true;
    LOG("gfx: Map of the %s failed (0x%08lx), device state 0x%08lx", what, (unsigned long)result,
        (unsigned long)g_dev->GetDeviceRemovedReason());
}

bool Upload(ComPtr<ID3D11Buffer>& ring, u32& pos, u32 cap, const void* src, u32 size, u32* offset) {
    if (size > cap) return false;
    D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
    pos = (pos + 255) & ~255u;
    if (pos + size > cap) {
        pos = 0;
        mode = D3D11_MAP_WRITE_DISCARD;
    }
    D3D11_MAPPED_SUBRESOURCE m;
    double t0 = NowMs();
    HRESULT mhr = g_ctx->Map(ring.Get(), 0, mode, 0, &m);
    g_map_ms += NowMs() - t0;
    if (FAILED(mhr)) {
        NoteMapFailure("geometry ring", mhr);
        return false;
    }
    memcpy((u8*)m.pData + pos, src, size);
    g_ctx->Unmap(ring.Get(), 0);
    *offset = pos;
    pos += size;
    return true;
}

// What the device context has bound: a draw sets only what changed. Every Set call costs a lock
// and the driver's own tracking, and most draws of a frame share nearly all their state.
struct BoundState {
    ID3D11RenderTargetView* rtv[8];
    ID3D11DepthStencilView* dsv;
    u32 nrt;
    D3D11_VIEWPORT viewport;
    D3D11_RECT scissor;
    u32 raster_setup, raster_scissor;
    u64 raster_key;
    float raster_scale;
    u32 blend_control, blend_mask;
    u32 depth_on, depth_control, depth_refmask, depth_ops, stencil_ref;
    ID3D11VertexShader* vs;
    ID3D11PixelShader* ps;
    ID3D11GeometryShader* gs;
    ID3D11InputLayout* layout;
    D3D11_PRIMITIVE_TOPOLOGY topology;
    ID3D11ShaderResourceView* srv[2][16];  // real pointers or null: set in ranges
    ID3D11SamplerState* smp[2][16];
    u64 smp_key[2][16];
    ID3D11Buffer* vb[16];
    UINT vb_stride[16], vb_offset[16];
    ID3D11Buffer* ib;
    DXGI_FORMAT ib_format;
    UINT ib_offset;
};
BoundState g_bound;

// All constant data of a draw goes into one big dynamic buffer: one Map (NO_OVERWRITE) per draw,
// bound per slot with a 256-byte aligned offset (*SetConstantBuffers1). What a slot already holds
// is neither copied nor bound again: most of a frame's constants are the same as the last draw's.
ComPtr<ID3D11Buffer> g_cb_ring;
u32 g_cb_ring_pos = 0;
constexpr u32 kCbRing = 16 << 20;
struct CbCopy {
    std::vector<u8> data;
    u32 size = 0, generation = 0;
};
CbCopy g_cb_copy[3][14];   // stage, slot: what is bound there
u32 g_cb_generation = 1;   // moves when the ring wraps or the bindings are forgotten
struct CbPending {
    int stage, slot;
    const void* data;
    u32 size;
};
CbPending g_cb_pending[64];
int g_cb_pendings = 0;

// The record of what is bound no longer holds: everything is set again by the next draw.
void ForgetBound() {
    static ID3D11ShaderResourceView* const no_views[16] = {};
    static ID3D11SamplerState* const no_samplers[16] = {};
    memset((void*)&g_bound, 0xFF, sizeof(g_bound));
    memset(g_bound.srv, 0, sizeof(g_bound.srv));
    memset(g_bound.smp, 0, sizeof(g_bound.smp));
    g_ctx->PSSetShaderResources(0, 16, no_views);
    g_ctx->VSSetShaderResources(0, 16, no_views);
    g_ctx->PSSetSamplers(0, 16, no_samplers);
    g_ctx->VSSetSamplers(0, 16, no_samplers);
    g_cb_generation++;
}

struct CbPart {
    int slot;
    const void* data;
    u32 size;
};

bool CbRingReady() {
    if (!g_ctx1) return false;
    if (!g_cb_ring) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = kCbRing;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(g_dev->CreateBuffer(&bd, nullptr, &g_cb_ring))) {
            g_ctx1.Reset();  // no large constant buffers: per-slot buffers
            return false;
        }
    }
    return true;
}

void CbQueue(int stage, const CbPart* parts, int n) {
    for (int i = 0; i < n && g_cb_pendings < 64; i++)
        g_cb_pending[g_cb_pendings++] = {stage, parts[i].slot, parts[i].data, parts[i].size};
}

// The constants of a draw, all in one generation of the ring: a wrap (WRITE_DISCARD) between two of
// them would leave the parts written before it in the discarded copy, and the draw would read
// whatever the new copy holds there - single frames with huge stray triangles (black wedges,
// vanishing objects). So the room for all of them is made before the first is looked at.
void CbFlush() {
    if (!g_cb_pendings) return;
    u32 room = 0;
    for (int i = 0; i < g_cb_pendings; i++) room += (g_cb_pending[i].size + 255) & ~255u;
    D3D11_MAP mode = D3D11_MAP_WRITE_NO_OVERWRITE;
    if (g_cb_ring_pos + room > kCbRing) {
        g_cb_ring_pos = 0;
        mode = D3D11_MAP_WRITE_DISCARD;
        g_cb_generation++;
    }
    bool changed[64];
    bool any = false;
    for (int i = 0; i < g_cb_pendings; i++) {
        const CbPending& p = g_cb_pending[i];
        const CbCopy& c = g_cb_copy[p.stage][p.slot];
        changed[i] = c.generation != g_cb_generation || c.size != p.size || memcmp(c.data.data(), p.data, p.size) != 0;
        any |= changed[i];
    }
    if (!any) {
        g_cb_pendings = 0;
        return;
    }
    D3D11_MAPPED_SUBRESOURCE m;
    if (const HRESULT mapped = g_ctx->Map(g_cb_ring.Get(), 0, mode, 0, &m); FAILED(mapped)) {
        NoteMapFailure("constants ring", mapped);
        g_cb_pendings = 0;
        return;
    }
    UINT first[64], num[64];
    u32 pos = g_cb_ring_pos;
    for (int i = 0; i < g_cb_pendings; i++) {
        if (!changed[i]) continue;
        const CbPending& p = g_cb_pending[i];
        const u32 sz = (p.size + 255) & ~255u;
        memcpy((u8*)m.pData + pos, p.data, p.size);
        first[i] = pos / 16;
        num[i] = sz / 16;
        pos += sz;
        CbCopy& c = g_cb_copy[p.stage][p.slot];
        c.data.assign((const u8*)p.data, (const u8*)p.data + p.size);
        c.size = p.size;
        c.generation = g_cb_generation;
    }
    g_ctx->Unmap(g_cb_ring.Get(), 0);
    g_cb_ring_pos = pos;
    ID3D11Buffer* b = g_cb_ring.Get();
    for (int i = 0; i < g_cb_pendings; i++) {
        if (!changed[i]) continue;
        const CbPending& p = g_cb_pending[i];
        const UINT slot = p.slot;
        if (p.stage == 0) g_ctx1->VSSetConstantBuffers1(slot, 1, &b, &first[i], &num[i]);
        else if (p.stage == 1) g_ctx1->PSSetConstantBuffers1(slot, 1, &b, &first[i], &num[i]);
        else g_ctx1->GSSetConstantBuffers1(slot, 1, &b, &first[i], &num[i]);
    }
    g_cb_pendings = 0;
}

void UpdateCBuffers(int stage, u64 shader, const ShaderInfo* si) {
    struct T { double t0 = NowMs(); ~T() { g_cb_ms += NowMs() - t0; } } timer;
    if (CbRingReady()) {
        u64 hdr = Q(shader + SH_HEADER);
        if (!Readable(hdr, HDR_CB_INDEX + 0x18)) return;
        s32 n = R<s32>(hdr + HDR_CB_MEMORY);
        u64 mem = Q(hdr + HDR_CB_MEMORY + 0x10), sizes = Q(hdr + HDR_CB_SIZE + 0x10),
            idx = Q(hdr + HDR_CB_INDEX + 0x10);
        CbPart parts[16];
        int np = 0;
        // MotionBlur=0: the blur pass falls back to its colour-grading-only branch when the
        // velocity scale (cb0 dword 8) is zero
        static unsigned motion_blur_gen = ~0u;
        static bool motion_blur = true;
        if (motion_blur_gen != ConfigGeneration())
            motion_blur_gen = ConfigGeneration(), motion_blur = ConfigInt("MotionBlur", 1) != 0;
        static thread_local u32 patched[64];
        for (s32 k = 0; k < n && k < 16; k++) {
            u64 data = Q(mem + k * 8);
            u32 size = (u32)R<u64>(sizes + k * 8);
            u32 slot = R<u32>(idx + k * 4);
            if (slot >= 13 || !data || !size || size > 65536 || !Readable(data, size)) continue;
            // the translations declare a buffer read at a computed offset with 512 entries (8 KB,
            // gcn2hlsl.py CB_COMPUTED_ENTRIES): a larger one would lose what lies past them
            if (size > 8192) {
                static int told = 0;
                if (told++ < 20)
                    LOG("gfx: %s binds a %u-byte constant buffer (slot %u), past the 8 KB its translation may read",
                        si->name.c_str(), size, slot);
            }
            if (!motion_blur && stage == 1 && slot == 0 && size >= 48 && size <= sizeof(patched) &&
                si->name.compare(0, 10, "MotionBlur") == 0) {
                memcpy(patched, (const void*)data, size);
                patched[8] = 0;
                data = (u64)patched;
            }
            parts[np++] = {(int)slot, (const void*)data, size};
            static const std::string after = [] {
                char v[128] = {};
                return GetEnvironmentVariableA("CTR_CAPTURE_AFTER", v, sizeof(v)) ? std::string(v) : std::string();
            }();
            // F11's capture: every draw's constants (the first 96 dwords)
            if (g_capture && (g_capture_by_flip || (!after.empty() && si->name.find(after) != std::string::npos))) {
                std::string w;
                for (u32 i = 0; i < size / 4 && i < (g_capture_by_flip ? 96u : 52u); i++) {
                    char b[16];
                    snprintf(b, sizeof(b), " %g", R<float>(data + i * 4));
                    w += b;
                }
                LOG("cap   %s cb%u size %u:%s", si->name.c_str(), slot, size, w.c_str());
            }
        }
        CbQueue(stage, parts, np);
        return;
    }
    u64 hdr = Q(shader + SH_HEADER);
    s32 n = R<s32>(hdr + HDR_CB_MEMORY);
    u64 mem = Q(hdr + HDR_CB_MEMORY + 0x10), sizes = Q(hdr + HDR_CB_SIZE + 0x10),
        idx = Q(hdr + HDR_CB_INDEX + 0x10);
    for (s32 k = 0; k < n && k < 16; k++) {
        u64 data = Q(mem + k * 8);
        u32 size = (u32)R<u64>(sizes + k * 8);
        u32 slot = R<u32>(idx + k * 4);
        if (slot >= 13 || !data || !size || size > 65536) continue;
        auto& cb = g_cb[stage][slot];
        if (!cb) {
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = 65536;
            bd.Usage = D3D11_USAGE_DYNAMIC;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            g_dev->CreateBuffer(&bd, nullptr, &cb);
        }
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(g_ctx->Map(cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            memcpy(m.pData, (const void*)data, size);
            g_ctx->Unmap(cb.Get(), 0);
        }
    }
    ID3D11Buffer* bufs[13];
    for (int i = 0; i < 13; i++) bufs[i] = g_cb[stage][i].Get();
    if (stage == 0) g_ctx->VSSetConstantBuffers(0, 13, bufs);
    else if (stage == 1) g_ctx->PSSetConstantBuffers(0, 13, bufs);
    else g_ctx->GSSetConstantBuffers(0, 13, bufs);
}

// Effective channel order seen by the shader: T# dst_sel applied to the memory order of the
// data (render targets may have been written with a component swap). Encoded per component as
// 0 = zero, 1 = one, 2..5 = logical x..w; 0 overall = identity.
u32 SwizzleCode(const u32* tsharp) {
    TSharp t = DecodeTSharp(tsharp);
    static const u8 kMemToLogical[4][4] = {{0, 1, 2, 3}, {2, 1, 0, 3}, {3, 2, 1, 0}, {3, 0, 1, 2}};
    u32 swap = 0;
    auto it = g_surfaces.find(t.base);
    if (it != g_surfaces.end() && Current(it->second) && t.dfmt != FMT_8 && t.dfmt != FMT_16 && t.dfmt != FMT_32 && t.dfmt != FMT_8_8 &&
        t.dfmt != FMT_16_16 && t.dfmt != FMT_32_32)
        swap = it->second.comp_swap;
    u32 code = 0;
    for (int c = 0; c < 4; c++) {
        u32 d = (t.dst_sel >> (3 * c)) & 7;
        u32 v = d == 0 ? 0 : d == 1 ? 1 : d >= 4 ? 2 + kMemToLogical[swap][d - 4] : 2 + c;
        code |= v << (3 * c);
    }
    return code == (2 | 3 << 3 | 4 << 6 | 5 << 9) ? 0 : code;
}

// b13 of every stage: per texture slot swizzle codes, then (VS) per input conversion modes
struct InfoCB {
    u32 swz[32];       // ctr_swz: texture channel order per slot
    u32 vin[32];       // ctr_vin: VS input conversion per semantic
    float scale[4];    // ctr_scale: x = 1 / scale of the bound render target, y = scale
    float tscale[32];  // ctr_tscale: scale of the texture bound per slot
    u32 pad[28];
};
static_assert(sizeof(InfoCB) == 512);
float g_draw_scale = 1.0f;  // scale of the render target of the current draw
float g_tscale[2][32];      // per stage texture slot scales (BindTextures)

InfoCB g_info[2];  // queued until the draw's constants go out

void WriteInfoCB(int stage, const u32* swz, const u32* vin) {
    if (CbRingReady()) {
        InfoCB& info = g_info[stage];
        memset(&info, 0, sizeof(info));
        memcpy(info.swz, swz, sizeof(info.swz));
        if (vin) memcpy(info.vin, vin, sizeof(info.vin));
        info.scale[0] = 1.0f / g_draw_scale;
        info.scale[1] = g_draw_scale;
        memcpy(info.tscale, g_tscale[stage], sizeof(info.tscale));
        CbPart part{13, &info, sizeof(info)};
        CbQueue(stage, &part, 1);
        return;
    }
    auto& cb = g_swz_cb[stage];
    if (!cb) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(InfoCB);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        g_dev->CreateBuffer(&bd, nullptr, &cb);
    }
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(g_ctx->Map(cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        InfoCB info{};
        memcpy(info.swz, swz, sizeof(info.swz));
        if (vin) memcpy(info.vin, vin, sizeof(info.vin));
        info.scale[0] = 1.0f / g_draw_scale;
        info.scale[1] = g_draw_scale;
        memcpy(info.tscale, g_tscale[stage], sizeof(info.tscale));
        memcpy(m.pData, &info, sizeof(info));
        g_ctx->Unmap(cb.Get(), 0);
    }
    if (stage == 0) g_ctx->VSSetConstantBuffers(13, 1, cb.GetAddressOf());
    else g_ctx->PSSetConstantBuffers(13, 1, cb.GetAddressOf());
}

u32 g_vs_swz[32];  // VS texture swizzles, written together with the input modes once the layout is known

// S# (GCN sampler descriptor) -> D3D11 sampler. PC extra: anisotropic filtering (setting
// Anisotropy, default 16) for filtered, mipmapped samplers.
std::unordered_map<u64, ComPtr<ID3D11SamplerState>> g_samplers;

D3D11_TEXTURE_ADDRESS_MODE AddressMode(u32 c) {
    switch (c & 7) {
    case 0: return D3D11_TEXTURE_ADDRESS_WRAP;
    case 1: return D3D11_TEXTURE_ADDRESS_MIRROR;
    case 2: return D3D11_TEXTURE_ADDRESS_CLAMP;
    case 3: return D3D11_TEXTURE_ADDRESS_MIRROR_ONCE;
    case 4:
    case 6: return D3D11_TEXTURE_ADDRESS_BORDER;
    default: return D3D11_TEXTURE_ADDRESS_MIRROR_ONCE;
    }
}

ID3D11SamplerState* GetSampler(const u32* ss, bool cmp) {
    static unsigned aniso_gen = ~0u;
    static int aniso_setting = 16;
    if (aniso_gen != ConfigGeneration()) {  // changed in the Video options: new samplers
        aniso_gen = ConfigGeneration();
        const int a = std::clamp(ConfigInt("Anisotropy", 16), 1, 16);
        if (a != aniso_setting) g_samplers.clear();
        aniso_setting = a;
    }
    u64 key = Fnv((const u8*)ss, 16) ^ (cmp ? 0x5bd1e995ull : 0);
    auto it = g_samplers.find(key);
    if (it != g_samplers.end()) return it->second.Get();
    D3D11_SAMPLER_DESC d{};
    d.AddressU = AddressMode(ss[0]);
    d.AddressV = AddressMode(ss[0] >> 3);
    d.AddressW = AddressMode(ss[0] >> 6);
    u32 max_aniso = 1u << std::min<u32>((ss[0] >> 9) & 7, 4);
    u32 depth_func = (ss[0] >> 12) & 7;
    u32 mag = (ss[2] >> 20) & 3, min = (ss[2] >> 22) & 3, mipf = (ss[2] >> 26) & 3;
    bool lin_mag = mag == 1 || mag == 3, lin_min = min == 1 || min == 3, lin_mip = mipf == 2;
    bool aniso = (mag >= 2 || min >= 2) || (aniso_setting > 1 && lin_min && lin_mag && mipf != 0);
    D3D11_FILTER_REDUCTION_TYPE red = cmp ? D3D11_FILTER_REDUCTION_TYPE_COMPARISON : D3D11_FILTER_REDUCTION_TYPE_STANDARD;
    if (aniso && !cmp) {
        d.Filter = D3D11_FILTER_ANISOTROPIC;
        d.MaxAnisotropy = std::max<u32>(max_aniso, aniso_setting);
    } else {
        d.Filter = D3D11_ENCODE_BASIC_FILTER(lin_min ? D3D11_FILTER_TYPE_LINEAR : D3D11_FILTER_TYPE_POINT,
                                             lin_mag ? D3D11_FILTER_TYPE_LINEAR : D3D11_FILTER_TYPE_POINT,
                                             lin_mip ? D3D11_FILTER_TYPE_LINEAR : D3D11_FILTER_TYPE_POINT, red);
    }
    static const D3D11_COMPARISON_FUNC funcs[] = {
        D3D11_COMPARISON_NEVER,   D3D11_COMPARISON_LESS,      D3D11_COMPARISON_EQUAL,         D3D11_COMPARISON_LESS_EQUAL,
        D3D11_COMPARISON_GREATER, D3D11_COMPARISON_NOT_EQUAL, D3D11_COMPARISON_GREATER_EQUAL, D3D11_COMPARISON_ALWAYS};
    d.ComparisonFunc = cmp ? (depth_func ? funcs[depth_func] : D3D11_COMPARISON_LESS_EQUAL) : D3D11_COMPARISON_NEVER;
    d.MinLOD = (ss[1] & 0xFFF) / 256.0f;
    d.MaxLOD = mipf == 0 ? d.MinLOD : ((ss[1] >> 12) & 0xFFF) / 256.0f;
    if (d.MaxLOD >= 15.9f) d.MaxLOD = D3D11_FLOAT32_MAX;
    static const int force_mip = ConfigInt("DebugForceMip", -1);  // debugging aid: sample one mip level
    if (force_mip >= 0) d.MinLOD = d.MaxLOD = (float)force_mip;
    d.MipLODBias = (float)((s32)((ss[2] & 0x3FFF) << 18) >> 18) / 256.0f;
    u32 border = ss[3] >> 30;  // 0 transparent black, 1 opaque black, 2 opaque white
    d.BorderColor[3] = border == 0 ? 0.0f : 1.0f;
    d.BorderColor[0] = d.BorderColor[1] = d.BorderColor[2] = border == 2 ? 1.0f : 0.0f;
    ComPtr<ID3D11SamplerState> st;
    if (FAILED(g_dev->CreateSamplerState(&d, &st))) return cmp ? g_samp_cmp.Get() : g_samp_linear.Get();
    if (g_samplers.size() < 8)
        LOG("gfx: sampler %08x %08x %08x %08x -> filter 0x%x addr %d/%d lod %.2f..%.2f bias %.2f cmp %d", ss[0],
            ss[1], ss[2], ss[3], (int)d.Filter, (int)d.AddressU, (int)d.AddressV, d.MinLOD, d.MaxLOD, d.MipLODBias,
            (int)d.ComparisonFunc);
    return (g_samplers[key] = st).Get();
}

// A T# resolved this frame: its view, channel order and scale are the same for the rest of it, and
// a frame binds the same few textures hundreds of times.
struct TextureMemo {
    u64 words[3];
    int dims;
    u64 frame, surfaces;
    ID3D11ShaderResourceView* srv;
    u32 swz;
    float scale;
};
TextureMemo g_texture_memo[1024];

const TextureMemo& ResolveTexture(const u32* tsharp, int dims) {
    u64 w[3];
    memcpy(w, tsharp, sizeof(w));
    u64 h = ((w[0] * 0x9E3779B97F4A7C15ull) ^ w[1]) * 0x9E3779B97F4A7C15ull;
    h = (h ^ w[2] ^ (u64)dims) * 0x9E3779B97F4A7C15ull;
    TextureMemo& m = g_texture_memo[h >> 54];
    if (m.frame == g_frame && m.surfaces == g_surface_generation && m.dims == dims && memcmp(m.words, w, sizeof(w)) == 0)
        return m;
    m.srv = GetTexture(tsharp, dims);
    m.swz = SwizzleCode(tsharp);
    m.scale = 1.0f;
    if (auto it = g_surfaces.find(DecodeTSharp(tsharp).base); it != g_surfaces.end() && Current(it->second))
        m.scale = it->second.scale;
    memcpy(m.words, w, sizeof(w));
    m.dims = dims;
    m.frame = g_frame;
    m.surfaces = g_surface_generation;
    return m;
}

void BindTextures(int stage, u64 ctx, u64 dev, const ShaderInfo* si) {
    u32 swz[32] = {};
    u64 tex_base = ctx + (stage == 0 ? CTX_VS_TEXTURES : CTX_PS_TEXTURES);
    for (int i = 0; i < 32; i++) g_tscale[stage][i] = 1.0f;
    ID3D11ShaderResourceView** bound = g_bound.srv[stage];
    int low = 16, high = -1;
    for (auto [slot, dims] : si->textures) {
        if (slot >= 16) continue;
        u64 t = Q(tex_base + slot * 8);
        if (!t) t = dev + DEV_MISSING_TEXTURE;
        const int wanted = si->cube_slots >> slot & 1 ? kDimsCube : dims;
        ID3D11ShaderResourceView* view;
        if (Readable(t, 32)) {
            const TextureMemo& m = ResolveTexture((const u32*)t, wanted);
            view = m.srv;
            swz[slot] = m.swz;
            g_tscale[stage][slot] = m.scale;
        } else {
            view = GetTexture(nullptr, wanted);
        }
        if (view != bound[slot]) {
            bound[slot] = view;
            low = std::min(low, slot);
            high = std::max(high, slot);
        }
    }
    if (high >= low) {
        if (stage == 0) g_ctx->VSSetShaderResources(low, high - low + 1, bound + low);
        else g_ctx->PSSetShaderResources(low, high - low + 1, bound + low);
    }
    if (stage == 0) memcpy(g_vs_swz, swz, sizeof(swz));
    else WriteInfoCB(stage, swz, nullptr);
    // the engine keeps packed sampler keys; its commit code expands them into S# like this
    u64 samp_base = ctx + (stage == 0 ? CTX_VS_SAMPLERS : CTX_PS_SAMPLERS);
    ID3D11SamplerState** samplers = g_bound.smp[stage];
    low = 16, high = -1;
    for (auto [slot, cmp] : si->samplers) {
        if (slot >= 16) continue;
        u32 k = (u32)Q(samp_base + slot * 8);
        const u64 sampler_key = (u64)k | (u64)cmp << 32;
        if (g_bound.smp_key[stage][slot] == sampler_key) continue;
        g_bound.smp_key[stage][slot] = sampler_key;
        u32 ss[4] = {0, 0xFFF000, 0x5000000, 0};
        if (k && stage == 0) {
            ss[0] = (k >> 8) & 0xFFF;
            ss[2] = (k & 0xF) << 20 | 0x4000000 | (k & 0xC0) << 18;
        } else if (k) {
            ss[0] = (k >> 8) & 0x7FFF;
            ss[2] = (k & 0x30) << 22 | (k & 0xC0) << 18 | (k & 0xF) << 20;
        }
        if (ID3D11SamplerState* sampler = GetSampler(ss, cmp); sampler != samplers[slot]) {
            samplers[slot] = sampler;
            low = std::min(low, slot);
            high = std::max(high, slot);
        }
        if (g_capture && cmp)
            LOG("cap   comparison sampler s%d key %08x -> S# %08x %08x depth func %u", slot, k, ss[0], ss[2],
                (ss[0] >> 12) & 7);
    }
    if (high >= low) {
        if (stage == 0) g_ctx->VSSetSamplers(low, high - low + 1, samplers + low);
        else g_ctx->PSSetSamplers(low, high - low + 1, samplers + low);
    }
}

struct VbBinding {
    u64 addr = 0;      // first vertex (buffer base + context offset)
    u32 stride = 0;
    u64 base = 0;      // the engine buffer resource: address and size in bytes
    u32 size = 0;
};

// Engine buffer resources (vertex / index): 24-byte records {u32 size; u64 address; ...}.
// Buffers of at least kCacheMin bytes live in GPU buffers created once and refreshed only when
// the guest writes them (write watch); smaller (per-frame) data goes through the upload rings.
constexpr u32 kCacheMin = 16 << 10;
struct GpuBuffer {
    ComPtr<ID3D11Buffer> buf;
    u64 addr = 0;
    u32 size = 0;
    u64 checked_frame = 0, used_frame = 0, seen_writes = 0, made_frame = 0;
    bool volatile_data = false;  // rewritten by the guest after creation: not cached any more
};
std::unordered_map<u64, GpuBuffer> g_gpu_buffers;

// Returns nullptr for data the guest keeps rewriting (callers upload it per draw instead: it may
// change several times within a frame).
ID3D11Buffer* CachedBufferChecked(u64 addr, u32 size, UINT bind) {
    u64 kv[2] = {addr, (u64)size << 8 | bind};
    auto& e = g_gpu_buffers[Fnv((const u8*)kv, 16)];
    e.used_frame = g_frame;
    e.addr = addr;
    e.size = size;
    if (e.volatile_data) return nullptr;
    struct T { double t0 = NowMs(); ~T() { g_buffer_ms += NowMs() - t0; } } timer;
    if (e.buf && e.checked_frame != g_frame) {
        e.checked_frame = g_frame;
        const u64 writes = GuestWriteCount();
        if (e.seen_writes != writes) {
            e.seen_writes = writes;
            if (GuestWrittenSince(addr, size, true)) {
                // written right after it was made: the game keeps rewriting it, and it is uploaded
                // per draw from now on. Long after: other data at this address (the next track),
                // made again.
                const bool rewritten = g_frame - e.made_frame < 300;
                e.buf.Reset();
                g_stats.buffer_refresh++;
                if (rewritten) {
                    e.volatile_data = true;
                    return nullptr;
                }
            }
        }
    }
    if (!e.buf) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = size;
        bd.Usage = D3D11_USAGE_DEFAULT;
        bd.BindFlags = bind;
        GuestResetWritten(addr, size);
        D3D11_SUBRESOURCE_DATA init{(const void*)addr};
        if (FAILED(g_dev->CreateBuffer(&bd, &init, &e.buf))) return nullptr;
        e.checked_frame = e.made_frame = g_frame;
        g_stats.buffer_create++;
    }
    return e.buf.Get();
}

// A buffer is looked up and checked once per frame: every draw of a mesh after the first asks the
// same thing.
struct BufferMemo {
    u64 addr = 0, frame = 0;
    u32 size = 0;
    UINT bind = 0;
    ID3D11Buffer* buffer = nullptr;
};
BufferMemo g_buffer_memo[512];

ID3D11Buffer* CachedBuffer(u64 addr, u32 size, UINT bind) {
    BufferMemo& m = g_buffer_memo[((addr >> 8) ^ (addr >> 20) ^ bind) & 511];
    if (m.frame == g_frame && m.addr == addr && m.size == size && m.bind == bind) return m.buffer;
    m.buffer = CachedBufferChecked(addr, size, bind);
    m.addr = addr;
    m.frame = g_frame;
    m.size = size;
    m.bind = bind;
    return m.buffer;
}

// Small buffers go through the upload rings at every draw. Those that hold the same bytes over a
// few frames are meshes, and stay on the GPU like the big ones, with the same write watch; what
// changes (particles, the GUI) keeps going through the rings, and the check is made only until a
// buffer has shown what it is. BB_D3D11_SMALL_BUFFERS=0 leaves them all in the rings.
struct SmallBuffer {
    u64 addr = 0, hash = 0, frame = 0;
    u32 size = 0, frames_same = 0;
};
SmallBuffer g_small_buffers[8192];
u64 g_vb_cached = 0, g_vb_uploads = 0, g_vb_ring_bytes = 0, g_ib_cached = 0, g_ib_uploads = 0, g_ib_ring_bytes = 0;

bool KeepOnGpu(u64 addr, u32 size) {
    constexpr u32 kFramesSame = 4;
    if (size >= kCacheMin) return true;
    static const bool on = [] {
        const char* env = std::getenv("BB_D3D11_SMALL_BUFFERS");
        return !(env && env[0] == '0');
    }();
    if (!on) return false;
    SmallBuffer& b = g_small_buffers[((addr >> 4) ^ (addr >> 17)) & 8191];
    if (b.addr != addr || b.size != size) {
        b.addr = addr;
        b.size = size;
        b.hash = Fnv((const u8*)addr, size);
        b.frame = g_frame;
        b.frames_same = 1;
        return false;
    }
    if (b.frames_same >= kFramesSame) return true;
    if (b.frame == g_frame) return false;
    b.frame = g_frame;
    const u64 hash = Fnv((const u8*)addr, size);
    b.frames_same = hash == b.hash ? b.frames_same + 1 : 1;
    b.hash = hash;
    return b.frames_same >= kFramesSame;
}

// A track's geometry is out of view for most of a lap: made again each time it came back, it cost
// a burst of buffer creations and a dip on every lap. A write or an unmap already retires a buffer.
constexpr u64 kBufferFrames = 30000;

constexpr u64 kIdleFrames = 600;  // unused this long, a copy of memory the game has moved on from goes

void EvictBuffers() {
    for (auto it = g_gpu_buffers.begin(); it != g_gpu_buffers.end();) {
        const GpuBuffer& e = it->second;
        const u64 idle = g_frame - e.used_frame;
        const bool gone = idle > kIdleFrames && GuestWrittenSince(e.addr, e.size, false);
        it = gone || idle > kBufferFrames ? g_gpu_buffers.erase(it) : std::next(it);
    }
}

// Nothing else frees textures, and every track brings its own: over a long session they would fill
// the video memory and then the system's. Past the budget (BB_D3D11_TEXTURE_MB, 1024 as the native
// Vulkan renderer's) the ones unused longest go; those whose memory is gone go at once.
void EvictTextures() {
    static const u64 budget = [] {
        const char* env = std::getenv("BB_D3D11_TEXTURE_MB");
        const long mb = env ? std::strtol(env, nullptr, 10) : 1024;
        return (u64)(mb > 0 ? mb : 1024) << 20;
    }();
    static u64 evicted = 0;
    std::vector<std::pair<u64, u64>> old;  // last use, key
    u64 total = 0;
    for (auto it = g_textures.begin(); it != g_textures.end();) {
        TextureObject& t = it->second;
        if (!t.srv) {  // not made: tried again, the memory may be there by now
            it = g_textures.erase(it);
            continue;
        }
        const bool idle = g_frame - t.used_frame > kIdleFrames && !t.button_atlas;
        if (idle && GuestWrittenSince(t.mem_base, t.mem_size, false)) {
            evicted++;
            it = g_textures.erase(it);
            continue;
        }
        total += t.mem_size;
        if (idle) old.emplace_back(t.used_frame, it->first);
        ++it;
    }
    if (total > budget) {
        std::sort(old.begin(), old.end());
        for (const auto& [used, key] : old) {
            if (total <= budget) break;
            auto it = g_textures.find(key);
            total -= it->second.mem_size;
            evicted++;
            g_textures.erase(it);
        }
    }
    if ((g_frame & 2047) == 1) {
        u64 buffers = 0;
        for (const auto& [key, e] : g_gpu_buffers)
            if (e.buf) buffers += e.size;
        LOG("gfx: memory: %zu textures %llu MiB (%llu evicted), %zu buffers %llu MiB", g_textures.size(),
            (unsigned long long)(total >> 20), (unsigned long long)evicted, g_gpu_buffers.size(),
            (unsigned long long)(buffers >> 20));
    }
}

// Builds the input layout for the VS from the engine's vertex formats.
void FillVertexBuffers(u64 ctx, const bool* used_slots, VbBinding* vbs) {
    for (int slot = 0; slot < 15; slot++) {
        if (!used_slots[slot]) continue;
        u64 vb = Q(ctx + CTX_VERTEX_BUFFERS + slot * 8);
        if (!Readable(vb, 16)) {  // no buffer in a slot the format uses: the draw is left out
            vbs[slot] = VbBinding{};
            continue;
        }
        vbs[slot].base = Q(vb + 8);
        vbs[slot].size = R<u32>(vb);
        vbs[slot].addr = vbs[slot].base + R<u32>(ctx + CTX_VB_OFFSETS + slot * 4);
        vbs[slot].stride = R<u32>(ctx + CTX_VB_STRIDES + slot * 4);
    }
}

// The matching depends only on the shader and the engine's vertex formats, which do not change:
// made once per pair, as the native Vulkan renderer does.
struct LayoutMemo {
    u64 vs = 0;
    u64 formats[16] = {};
    ID3D11InputLayout* layout = nullptr;
    bool used[16] = {};
    u32 vin[32] = {};
    std::string inputs;  // where each input comes from, for a frame capture
};
LayoutMemo g_layout_memo[512];

ID3D11InputLayout* GetLayout(u64 ctx, u64 vs, const ShaderInfo* si, VbBinding* vbs, bool* used_slots,
                             u32* vin_modes) {
    u64 formats[16];
    memcpy(formats, (const void*)(ctx + CTX_VERTEX_FORMATS), sizeof(formats));
    u64 memo_hash = vs;
    for (u64 f : formats) memo_hash = (memo_hash ^ f) * 0x100000001B3ull;
    LayoutMemo& memo = g_layout_memo[(memo_hash ^ memo_hash >> 29) & 511];
    if (memo.vs == vs && memcmp(memo.formats, formats, sizeof(formats)) == 0) {
        memcpy(used_slots, memo.used, sizeof(memo.used));
        memcpy(vin_modes, memo.vin, sizeof(memo.vin));
        FillVertexBuffers(ctx, used_slots, vbs);
        if (g_capture) LOG("    inputs%s", memo.inputs.c_str());
        return memo.layout;
    }
    u64 hdr = Q(vs + SH_HEADER);
    if (!Readable(hdr, HDR_INPUT_ELEMENTS + 0x18)) return nullptr;
    s32 nin = R<s32>(hdr + HDR_INPUT_ELEMENTS);
    u64 ins = Q(hdr + HDR_INPUT_ELEMENTS + 0x10);
    if (nin < 0 || !ins || !Readable(ins, (u64)nin * 12)) nin = 0;
    std::vector<D3D11_INPUT_ELEMENT_DESC> descs;
    std::string inputs;
    u64 key = (u64)si * 0x9E3779B97F4A7C15ull;
    for (auto [sem, size] : si->inputs) {
        u32 usage = 0xFF, uidx = 0xFF;
        for (s32 k = 0; k < nin; k++) {
            if (R<u32>(ins + k * 12 + 8) == (u32)sem) {
                usage = R<u32>(ins + k * 12);
                uidx = R<u32>(ins + k * 12 + 4);
            }
        }
        D3D11_INPUT_ELEMENT_DESC d{"ATTRIB", (UINT)sem, DXGI_FORMAT_R32G32B32A32_FLOAT, 15, 0,
                                   D3D11_INPUT_PER_VERTEX_DATA, 0};
        for (int slot = 0; slot < 16 && usage != 0xFF; slot++) {
            u64 vf = Q(ctx + CTX_VERTEX_FORMATS + slot * 8);
            if (!vf || !Readable(vf, 0x30)) continue;
            u64 el = Q(vf + 0x18), fm = Q(vf + 0x28);
            if (!el || !fm || !Readable(fm, 32 * 4)) continue;
            for (int k = 0; k < 32 && Readable(el + k * 12, 12) && R<u8>(el + k * 12) != ','; k++) {
                if (R<u8>(el + k * 12 + 4) == usage && R<u8>(el + k * 12 + 5) == uidx) {
                    DXGI_FORMAT f = VertexFormat(R<u32>(fm + k * 4));
                    if (f == DXGI_FORMAT_UNKNOWN) break;
                    d.Format = f;
                    if (sem < 32) vin_modes[sem] = VertexConversion(R<u32>(fm + k * 4));
                    d.InputSlot = slot;
                    d.AlignedByteOffset = R<u16>(el + k * 12 + 8);
                    used_slots[slot] = true;
                    slot = 16;
                    break;
                }
            }
        }
        if (d.InputSlot == 15) {
            used_slots[15] = true;
            d.AlignedByteOffset = usage == 4 ? 16 : 0;
        }
        descs.push_back(d);
        {
            char b[64];
            if (d.InputSlot == 15) snprintf(b, sizeof(b), " a%u:NONE(usage %u.%u)", (unsigned)sem, usage, uidx);
            else snprintf(b, sizeof(b), " a%u:s%u+%u/f%d", (unsigned)sem, d.InputSlot, d.AlignedByteOffset, (int)d.Format);
            inputs += b;
        }
        u64 part[2] = {(u64)sem << 32 | (u32)d.Format, (u64)d.InputSlot << 32 | d.AlignedByteOffset};
        key = (key ^ Fnv((const u8*)part, 16)) * 0x100000001B3ull;
    }
    FillVertexBuffers(ctx, used_slots, vbs);
    auto& l = g_layouts[key];
    if (!l) {
        HRESULT hr = g_dev->CreateInputLayout(descs.data(), (UINT)descs.size(), si->bytecode.data(),
                                              si->bytecode.size(), &l);
        if (FAILED(hr)) {
            LOG("gfx: input layout for %s failed (0x%08lx)", si->name.c_str(), hr);
            for (auto& d : descs)
                LOG("    ATTRIB%u fmt %d slot %u off %u", d.SemanticIndex, (int)d.Format, d.InputSlot,
                    d.AlignedByteOffset);
            DrainDebugMessages();
        }
    }
    memo.vs = vs;
    memcpy(memo.formats, formats, sizeof(formats));
    memo.layout = l.Get();
    memcpy(memo.used, used_slots, sizeof(memo.used));
    memcpy(memo.vin, vin_modes, sizeof(memo.vin));
    memo.inputs = inputs;
    if (g_capture) LOG("    inputs%s", inputs.c_str());
    return l.Get();
}

void WriteBmp(const char* path, const u8* rgba, u32 w, u32 h, u32 pitch, bool bgr_src) {
    FILE* f = fopen(path, "wb");
    if (!f) return;
    u32 row = w * 3, pad = (4 - row % 4) % 4, size = (row + pad) * h;
    u8 hdr[54] = {'B', 'M'};
    auto put32 = [&](int o, u32 v) { memcpy(hdr + o, &v, 4); };
    put32(2, 54 + size);
    put32(10, 54);
    put32(14, 40);
    put32(18, w);
    put32(22, h);
    hdr[26] = 1;
    hdr[28] = 24;
    put32(34, size);
    fwrite(hdr, 1, 54, f);
    std::vector<u8> line(row + pad);
    for (u32 y = 0; y < h; y++) {
        const u8* src = rgba + (size_t)(h - 1 - y) * pitch;
        for (u32 x = 0; x < w; x++) {
            line[x * 3 + 0] = src[x * 4 + (bgr_src ? 0 : 2)];
            line[x * 3 + 1] = src[x * 4 + 1];
            line[x * 3 + 2] = src[x * 4 + (bgr_src ? 2 : 0)];
        }
        fwrite(line.data(), 1, line.size(), f);
    }
    fclose(f);
}

float Half(u16 h) {
    u32 sgn = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    u32 f = e == 0 ? (m ? 0 : 0) : e == 31 ? (0x7F800000 | (m << 13)) : (((e - 15 + 127) << 23) | (m << 13));
    f |= sgn << 31;
    float r;
    memcpy(&r, &f, 4);
    return r;
}

// Logs the raw value of pixel (x, y) of a surface (capture debugging).
void ProbePixel(u64 base, Surface& s, u32 x, u32 y) {
    if (!s.tex || x >= s.w || y >= s.h) return;
    x = (u32)(x * s.scale);  // logical -> physical (RenderScale)
    y = (u32)(y * s.scale);
    D3D11_TEXTURE2D_DESC td;
    s.tex->GetDesc(&td);
    td.Width = td.Height = 1;
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> st;
    if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &st))) return;
    D3D11_BOX box{x, y, 0, x + 1, y + 1, 1};
    g_ctx->CopySubresourceRegion(st.Get(), 0, 0, 0, 0, s.tex.Get(), 0, &box);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(g_ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) return;
    const u8* p = (const u8*)m.pData;
    char buf[160];
    switch (s.fmt) {
    case DXGI_FORMAT_R16G16B16A16_FLOAT: {
        const u16* h = (const u16*)p;
        snprintf(buf, sizeof(buf), "%g %g %g %g", Half(h[0]), Half(h[1]), Half(h[2]), Half(h[3]));
        break;
    }
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: {
        float f;
        memcpy(&f, p, 4);
        snprintf(buf, sizeof(buf), "%g", f);
        break;
    }
    default:
        snprintf(buf, sizeof(buf), "raw %02x %02x %02x %02x", p[0], p[1], p[2], p[3]);
    }
    LOG("cap pixel %llx (%u,%u): %s", (unsigned long long)base, x, y, buf);
    g_ctx->Unmap(st.Get(), 0);
}

// Depth surfaces: the whole texture to a staging copy (depth-stencil resources cannot be copied
// in part), depth values saved as raw float32 (<path>.f32) with per-2048-row-band statistics.
void SaveDepth(Surface& s, const char* path) {
    D3D11_TEXTURE2D_DESC td;
    s.tex->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> st;
    if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &st))) return;
    g_ctx->CopyResource(st.Get(), s.tex.Get());
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(g_ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) return;
    std::vector<float> z((size_t)td.Width * td.Height);
    for (u32 y = 0; y < td.Height; y++)
        for (u32 x = 0; x < td.Width; x++) {
            const u8* px = (const u8*)m.pData + (size_t)y * m.RowPitch;
            if (s.fmt == DXGI_FORMAT_D16_UNORM) z[(size_t)y * td.Width + x] = ((const u16*)px)[x] / 65535.0f;
            else memcpy(&z[(size_t)y * td.Width + x], px + x * 8, 4);
        }
    g_ctx->Unmap(st.Get(), 0);
    std::string f32 = std::string(path) + ".f32";
    if (FILE* f = fopen(f32.c_str(), "wb")) {
        fwrite(z.data(), 4, z.size(), f);
        fclose(f);
    }
    for (u32 band = 0; band * 2048 < td.Height; band++) {
        float lo = 1e30f, hi = -1e30f;
        size_t below = 0, n = 0;
        for (u32 y = band * 2048; y < std::min(td.Height, (band + 1) * 2048); y++)
            for (u32 x = 0; x < td.Width; x++, n++) {
                float v = z[(size_t)y * td.Width + x];
                lo = std::min(lo, v), hi = std::max(hi, v);
                below += v < 0.9999f;
            }
        LOG("cap depth %s band %u: min %f max %f, %.1f%% below 1", path, band, lo, hi, n ? 100.0 * below / n : 0.0);
    }
}

// Draws `s` into an RGBA8 texture of the same size with the blit shader and saves it.
void SaveSurface(Surface& s, const char* path) {
    if (!g_blit_vs || !s.srv) return;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = s.w;
    td.Height = s.h;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> tmp, st;
    ComPtr<ID3D11RenderTargetView> rtv;
    if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &tmp))) return;
    g_dev->CreateRenderTargetView(tmp.Get(), nullptr, &rtv);
    g_ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
    D3D11_VIEWPORT v{0, 0, (float)s.w, (float)s.h, 0, 1};
    g_ctx->RSSetViewports(1, &v);
    g_ctx->RSSetState(GetRaster(0, false));  // no scissor / culling left over from the game's draws
    g_ctx->OMSetDepthStencilState(g_depth_off.Get(), 0);
    g_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    g_ctx->IASetInputLayout(nullptr);
    g_ctx->GSSetShader(nullptr, nullptr, 0);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx->VSSetShader(g_blit_vs.Get(), nullptr, 0);
    g_ctx->PSSetShader(g_blit_ps.Get(), nullptr, 0);
    g_ctx->PSSetShaderResources(0, 1, s.srv.GetAddressOf());
    g_ctx->PSSetSamplers(0, 1, g_samp_linear.GetAddressOf());
    g_ctx->Draw(3, 0);
    ForgetBound();
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    g_dev->CreateTexture2D(&td, nullptr, &st);
    g_ctx->CopyResource(st.Get(), tmp.Get());
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(g_ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) {
        WriteBmp(path, (const u8*)m.pData, s.w, s.h, m.RowPitch, false);
        g_ctx->Unmap(st.Get(), 0);
    }
}

void SaveScreenshot(const char* path) {
    ComPtr<ID3D11Texture2D> bb, st;
    g_swap->GetBuffer(0, IID_PPV_ARGS(&bb));
    D3D11_TEXTURE2D_DESC d;
    bb->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &st))) return;
    g_ctx->CopyResource(st.Get(), bb.Get());
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(g_ctx->Map(st.Get(), 0, D3D11_MAP_READ, 0, &m))) return;
    FILE* f = fopen(path, "wb");
    // CTR_SCREENSHOT_DIV=<k>: k x k box-filtered (long frame sequences)
    static const u32 div = [] {
        char v[8] = {};
        return GetEnvironmentVariableA("CTR_SCREENSHOT_DIV", v, sizeof(v)) ? std::max(1, atoi(v)) : 1;
    }();
    if (f) {
        u32 w = d.Width / div, h = d.Height / div, row = w * 3, pad = (4 - row % 4) % 4, size = (row + pad) * h;
        u8 hdr[54] = {'B', 'M'};
        auto put32 = [&](int o, u32 v) { memcpy(hdr + o, &v, 4); };
        put32(2, 54 + size);
        put32(10, 54);
        put32(14, 40);
        put32(18, w);
        put32(22, h);
        hdr[26] = 1;
        hdr[28] = 24;
        put32(34, size);
        fwrite(hdr, 1, 54, f);
        std::vector<u8> line(row + pad);
        for (u32 y = 0; y < h; y++) {
            for (u32 x = 0; x < w; x++) {
                u32 sum[3] = {};
                for (u32 dy = 0; dy < div; dy++) {
                    const u8* src = (const u8*)m.pData + (size_t)((h - 1 - y) * div + dy) * m.RowPitch + x * div * 4;
                    for (u32 dx = 0; dx < div; dx++)
                        for (int c = 0; c < 3; c++) sum[c] += src[dx * 4 + c];
                }
                line[x * 3 + 0] = (u8)(sum[2] / (div * div));
                line[x * 3 + 1] = (u8)(sum[1] / (div * div));
                line[x * 3 + 2] = (u8)(sum[0] / (div * div));
            }
            fwrite(line.data(), 1, line.size(), f);
        }
        fclose(f);
        LOG("gfx: screenshot -> %s", path);
    }
    g_ctx->Unmap(st.Get(), 0);
}

}  // namespace

void GfxDraw(u32 count, u64 index_addr, bool indexed) {
    const double lock_start = NowMs();
    std::lock_guard lk(g_mutex);
    g_lock_ms += NowMs() - lock_start;
    if (!Init()) return;
    DrawTimer timer;
    double sec_t = NowMs();
    g_stats.draws++;
    g_cb_pendings = 0;
    u64 dev = Q(g_image_base + kDeviceGlobal);
    u64 ctx = Q(dev + DEV_CONTEXT);
    if (!ctx) return;
    u64 vsh = Q(ctx + CTX_VS), psh = Q(ctx + CTX_PS);
    ShaderInfo* vs = GetShader(vsh, true);
    ShaderInfo* ps = psh ? GetShader(psh, false) : nullptr;
    if (ps && ps->video) g_video_ms = (u64)GetTickCount64();
    // an export shader runs in front of a geometry shader (with its embedded copy VS)
    u64 gsh = vs && vs->kind == "es" ? Q(ctx + CTX_GS) : 0;
    ShaderInfo* gs = gsh ? GetShader(gsh, false) : nullptr;
    SECTION(0);
    if (gsh && (!gs || !gs->gs)) {
        if (g_capture) LOG("cap draw skipped: missing geometry shader for %s", vs->name.c_str());
        g_stats.no_shader++;
        return;
    }
    // debugging aid: CTR_SKIP_SHADERS=<substring> drops draws whose shaders match
    static const std::string skip = [] {
        char v[128] = {};
        return GetEnvironmentVariableA("CTR_SKIP_SHADERS", v, sizeof(v)) ? std::string(v) : std::string();
    }();
    if (!skip.empty() && ((vs && vs->name.find(skip) != std::string::npos) ||
                          (ps && ps->name.find(skip) != std::string::npos) ||
                          (gs && gs->name.find(skip) != std::string::npos)))
        return;
    if (!vs || !vs->vs || (psh && (!ps || !ps->ps))) {
        if (g_capture) LOG("cap draw skipped: missing shader vs %s ps %s", vs ? vs->name.c_str() : "?",
                           ps ? ps->name.c_str() : (psh ? "?" : "-"));
        g_stats.no_shader++;
        return;
    }
    // render targets + viewport
    ID3D11RenderTargetView* rtv[8] = {};
    u32 nrt = 0;
    float rt_scale = 0;
    u32 rt_height = 0;
    for (int i = 0; i < 8; i++) {
        u64 h = Q(ctx + CTX_COLOR_TARGETS + i * 8);
        if (!h) continue;
        if (Surface* s = GetColorTarget(h)) {
            rtv[i] = s->rtv.Get();
            s->rendered = true;
            nrt = i + 1;
            if (!rt_scale) rt_scale = s->scale, rt_height = s->h;
        }
    }
    SECTION(1);
    Surface* depth = nullptr;
    if (u64 dh = Q(ctx + CTX_DEPTH_TARGET)) depth = GetDepthTarget(dh);
    if (!nrt && !depth) {
        g_stats.no_target++;
        return;
    }
    g_draw_scale = rt_scale ? rt_scale : depth ? depth->scale : 1.0f;
    u32 dctl = R<u32>(ctx + CTX_DEPTH_STENCIL_CONTROL);
    bool zwrite = (dctl >> 2) & 1;
    // a pass that samples the depth buffer while depth-testing binds it read-only
    bool depth_sampled = false;
    if (depth && ps) {
        const u32 depth_base = R<u32>(Q(ctx + CTX_DEPTH_TARGET) + 0x58);  // in units of 256 bytes
        for (auto [slot, dims] : ps->textures) {
            u64 t = Q(ctx + CTX_PS_TEXTURES + slot * 8);
            if (t && Readable(t, 32) && R<u32>(t) == depth_base && (R<u32>(t + 4) & 0xFF) == 0) depth_sampled = true;
        }
    }
    ID3D11DepthStencilView* dsv = depth ? (depth_sampled && !zwrite ? depth->dsv_ro.Get() : depth->dsv.Get()) : nullptr;
    if (g_trace_depth && g_frame >= g_trace_frame && (!g_trace_frame || g_frame < g_trace_frame + 12)) {
        u64 dh2 = Q(ctx + CTX_DEPTH_TARGET);
        u64 db = dh2 && Readable(dh2, 0x84) ? (u64)R<u32>(dh2 + 0x58) << 8 : 0;
        if (db && (db == g_trace_depth || g_trace_depth == 1)) {  // 1: every depth target
            auto tvp = R<ViewportI>(ctx + CTX_VIEWPORT);
            char b[64];
            if (g_trace_depth == 1)
                snprintf(b, sizeof(b), " %llx@%d,%d:%dx%d", (unsigned long long)db, tvp.x, tvp.y, tvp.w, tvp.h);
            else
                snprintf(b, sizeof(b), " d%d,%d", tvp.x, tvp.y);
            g_trace += b;
        }
    }
    if (depth_sampled && zwrite) dsv = nullptr;  // feedback loop: drop the depth target
    if (depth) depth->rendered = true;
    if (nrt != g_bound.nrt || dsv != g_bound.dsv || memcmp(rtv, g_bound.rtv, sizeof(rtv)) != 0) {
        // unbind our targets from the shader stages before using them as targets
        static ID3D11ShaderResourceView* const none[16] = {};
        g_ctx->PSSetShaderResources(0, 16, none);
        g_ctx->VSSetShaderResources(0, 16, none);
        memset(g_bound.srv, 0, sizeof(g_bound.srv));
        g_ctx->OMSetRenderTargets(nrt, rtv, dsv);
        memcpy(g_bound.rtv, rtv, sizeof(rtv));
        g_bound.nrt = nrt;
        g_bound.dsv = dsv;
    }
    auto vp = R<ViewportI>(ctx + CTX_VIEWPORT);
    const float ds = g_draw_scale;
    // the engine's viewport y counts from the bottom of the target (the shadow cascades of the
    // 2048x8192 atlas are at y 6144, 4096, 2048 for cascades 0, 1, 2)
    const u32 target_h = depth ? depth->h : rt_height;
    const s32 top = target_h && vp.y + vp.h <= (s32)target_h ? (s32)target_h - vp.y - vp.h : vp.y;
    D3D11_VIEWPORT v{vp.x * ds, top * ds, vp.w * ds, vp.h * ds, vp.zmin, vp.zmax};
    if (memcmp(&v, &g_bound.viewport, sizeof(v)) != 0) {
        g_ctx->RSSetViewports(1, &v);
        g_bound.viewport = v;
    }
    bool scissor = R<u8>(ctx + CTX_SCISSOR_ENABLED) != 0;
    if (scissor) {
        // bottom-up too (the GUI clips the sliding values of its option selectors with it)
        auto sc = R<ViewportI>(ctx + CTX_SCISSOR);
        const s32 sy = target_h ? (s32)target_h - sc.y - sc.h : sc.y;
        D3D11_RECT r{(LONG)(sc.x * ds), (LONG)(sy * ds), (LONG)((sc.x + sc.w) * ds), (LONG)((sy + sc.h) * ds)};
        if (memcmp(&r, &g_bound.scissor, sizeof(r)) != 0) {
            g_ctx->RSSetScissorRects(1, &r);
            g_bound.scissor = r;
        }
    }
    const u32 prim_setup = R<u32>(ctx + CTX_PRIM_SETUP);
    const u64 raster_key = Q(ctx + CTX_RASTER_STATE);
    if (prim_setup != g_bound.raster_setup || raster_key != g_bound.raster_key || (u32)scissor != g_bound.raster_scissor ||
        ds != g_bound.raster_scale) {
        g_ctx->RSSetState(GetRaster(prim_setup, scissor, raster_key, ds));
        g_bound.raster_setup = prim_setup;
        g_bound.raster_key = raster_key;
        g_bound.raster_scissor = scissor;
        g_bound.raster_scale = ds;
    }
    const u32 blend_control = R<u32>(ctx + CTX_BLEND_CONTROL), blend_mask = R<u32>(ctx + CTX_RT_MASK);
    if (blend_control != g_bound.blend_control || blend_mask != g_bound.blend_mask) {
        g_ctx->OMSetBlendState(GetBlend(blend_control, blend_mask), nullptr, 0xFFFFFFFF);
        g_bound.blend_control = blend_control;
        g_bound.blend_mask = blend_mask;
    }
    u32 refmask = R<u32>(ctx + CTX_STENCIL_CONTROL), sops = R<u32>(ctx + CTX_STENCIL_OP_CONTROL);
    bool uses_opval = ((sops & 0xF) == 4) || (((sops >> 4) & 0xF) == 4) || (((sops >> 8) & 0xF) == 4);
    const u32 stencil_ref = uses_opval ? refmask >> 24 : refmask & 0xFF;
    if ((u32)(dsv != nullptr) != g_bound.depth_on || dctl != g_bound.depth_control || refmask != g_bound.depth_refmask ||
        sops != g_bound.depth_ops || stencil_ref != g_bound.stencil_ref) {
        g_ctx->OMSetDepthStencilState(dsv ? GetDepthState(dctl, refmask, sops) : g_depth_off.Get(), stencil_ref);
        g_bound.depth_on = dsv != nullptr;
        g_bound.depth_control = dctl;
        g_bound.depth_refmask = refmask;
        g_bound.depth_ops = sops;
        g_bound.stencil_ref = stencil_ref;
    }

    SECTION(2);
    // shaders, constants, textures
    if (vs->vs.Get() != g_bound.vs) g_ctx->VSSetShader(g_bound.vs = vs->vs.Get(), nullptr, 0);
    ID3D11PixelShader* const pixel_shader = ps ? ps->ps.Get() : nullptr;
    if (pixel_shader != g_bound.ps) g_ctx->PSSetShader(g_bound.ps = pixel_shader, nullptr, 0);
    ID3D11GeometryShader* const geometry_shader = gs ? gs->gs.Get() : nullptr;
    if (geometry_shader != g_bound.gs) g_ctx->GSSetShader(g_bound.gs = geometry_shader, nullptr, 0);
    SECTION(6);
    if (gs) UpdateCBuffers(2, gsh, gs);
    UpdateCBuffers(0, vsh, vs);
    BindTextures(0, ctx, dev, vs);
    if (ps) {
        UpdateCBuffers(1, psh, ps);
        BindTextures(1, ctx, dev, ps);
    }
    SECTION(7);

    SECTION(3);
    // geometry
    u32 prim = R<u32>(ctx + CTX_PRIM_TYPE);
    std::vector<u32> gen;  // generated indices for fans / quads
    u32 isize = indexed ? R<u32>(ctx + CTX_INDEX_SIZE) : 4;
    u32 max_index = count ? count - 1 : 0;
    const u8* idata = nullptr;
    bool max_known = !indexed;
    auto scan_max_index = [&] {
        if (max_known) return;
        max_known = true;
        max_index = 0;
        for (u32 i = 0; i < count; i++) {
            u32 x = isize == 2 ? ((const u16*)idata)[i] : ((const u32*)idata)[i];
            if (x > max_index && x != 0xFFFF && x != 0xFFFFFFFF) max_index = x;
        }
    };
    if (indexed) {
        if (!Readable(index_addr, (u64)count * isize)) return;
        idata = (const u8*)index_addr;
    }
    auto src_index = [&](u32 i) -> u32 {
        return !indexed ? i : isize == 2 ? ((const u16*)idata)[i] : ((const u32*)idata)[i];
    };
    D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
    switch (prim) {
    case 0: topo = D3D11_PRIMITIVE_TOPOLOGY_POINTLIST; break;
    case 1: topo = D3D11_PRIMITIVE_TOPOLOGY_LINELIST; break;
    case 2: topo = D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP; break;
    case 4: topo = D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP; break;
    case 5:  // fan
        for (u32 i = 1; i + 1 < count; i++) gen.insert(gen.end(), {src_index(0), src_index(i), src_index(i + 1)});
        break;
    case 6:  // quads
        for (u32 i = 0; i + 3 < count; i += 4)
            gen.insert(gen.end(), {src_index(i), src_index(i + 1), src_index(i + 2), src_index(i),
                                   src_index(i + 2), src_index(i + 3)});
        break;
    default: break;
    }

    SECTION(4);
    bool used[16] = {};
    VbBinding vbs[16];
    u32 vin_modes[32] = {};
    ID3D11InputLayout* layout = GetLayout(ctx, vsh, vs, vbs, used, vin_modes);
    WriteInfoCB(0, g_vs_swz, vin_modes);
    if (!layout && !vs->inputs.empty()) {
        g_stats.no_layout++;
        return;
    }
    SECTION(3);
    CbFlush();
    SECTION(8);
    if (layout != g_bound.layout) g_ctx->IASetInputLayout(g_bound.layout = layout);
    // all ring uploads of a draw in one generation of the ring (see CbRingReserve): when it wraps after
    // an earlier slot of the draw, that slot's data went with the discarded copy - upload them again
    for (int pass = 0; pass < 2; pass++) {
        bool ring_used = false, wrapped = false;
        for (int slot = 0; slot < 16; slot++) {
            if (!used[slot]) continue;
            ID3D11Buffer* b = nullptr;
            UINT stride = 0, off = 0;
            if (slot == 15) {
                b = g_zero_vb.Get();
            } else if (vbs[slot].size && vbs[slot].addr >= vbs[slot].base &&
                       vbs[slot].addr < vbs[slot].base + vbs[slot].size && Readable(vbs[slot].base, vbs[slot].size) &&
                       KeepOnGpu(vbs[slot].base, vbs[slot].size) &&
                       (b = CachedBuffer(vbs[slot].base, vbs[slot].size, D3D11_BIND_VERTEX_BUFFER))) {
                stride = vbs[slot].stride;
                off = (UINT)(vbs[slot].addr - vbs[slot].base);
                g_vb_cached++;
            } else {
                scan_max_index();
                u64 bytes = (u64)(max_index + 1) * vbs[slot].stride;
                if (!vbs[slot].stride || !Readable(vbs[slot].addr, bytes)) return;
                if (!Upload(g_vb_ring, g_vb_pos, kVbRing, (const void*)vbs[slot].addr, (u32)bytes, &off)) return;
                g_vb_uploads++;
                g_vb_ring_bytes += bytes;
                wrapped |= ring_used && off == 0;
                ring_used = true;
                b = g_vb_ring.Get();
                stride = vbs[slot].stride;
            }
            if (b != g_bound.vb[slot] || stride != g_bound.vb_stride[slot] || off != g_bound.vb_offset[slot]) {
                g_ctx->IASetVertexBuffers(slot, 1, &b, &stride, &off);
                g_bound.vb[slot] = b;
                g_bound.vb_stride[slot] = stride;
                g_bound.vb_offset[slot] = off;
            }
        }
        if (!wrapped) break;
    }
    SECTION(5);
    if (topo != g_bound.topology) g_ctx->IASetPrimitiveTopology(g_bound.topology = topo);
    const auto set_indices = [](ID3D11Buffer* buffer, DXGI_FORMAT format, UINT offset) {
        if (buffer == g_bound.ib && format == g_bound.ib_format && offset == g_bound.ib_offset) return;
        g_ctx->IASetIndexBuffer(buffer, format, offset);
        g_bound.ib = buffer;
        g_bound.ib_format = format;
        g_bound.ib_offset = offset;
    };
    double ti = NowMs();
    struct IssueT { double t0; ~IssueT() { g_issue_ms += NowMs() - t0; } } issue{ti};
    UINT re_count = count, re_start = 0;  // the call made, for F11's pixel counts below
    bool re_indexed = indexed;
    if (g_capture_by_flip && RenderDoc()) {  // the draw named in RenderDoc's event list
        static ComPtr<ID3DUserDefinedAnnotation> marks;
        if (!marks) g_ctx.As(&marks);
        if (marks) {
            wchar_t name[160];
            swprintf(name, 160, L"%llu %hs / %hs", (unsigned long long)g_stats.draws, vs ? vs->name.c_str() : "?",
                     ps ? ps->name.c_str() : "-");
            marks->SetMarker(name);
        }
    }
    if (!gen.empty()) {
        u32 off;
        if (!Upload(g_ib_ring, g_ib_pos, kIbRing, gen.data(), (u32)(gen.size() * 4), &off)) return;
        set_indices(g_ib_ring.Get(), DXGI_FORMAT_R32_UINT, off);
        g_ctx->DrawIndexed((UINT)gen.size(), 0, 0);
        re_count = (UINT)gen.size(), re_indexed = true;
    } else if (indexed) {
        DXGI_FORMAT ifmt = isize == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
        u64 ib = Q(ctx + CTX_INDEX_BUFFER);
        u64 ib_base = ib && Readable(ib, 16) ? Q(ib + 8) : 0;
        u32 ib_size = ib_base ? R<u32>(ib) : 0;
        if (ib_size && index_addr >= ib_base && index_addr + (u64)count * isize <= ib_base + ib_size &&
            (index_addr - ib_base) % isize == 0 && Readable(ib_base, ib_size) && KeepOnGpu(ib_base, ib_size)) {
            ID3D11Buffer* b = CachedBuffer(ib_base, ib_size, D3D11_BIND_INDEX_BUFFER);
            if (!b) goto ring_indices;
            g_ib_cached++;
            set_indices(b, ifmt, 0);
            g_ctx->DrawIndexed(count, (UINT)((index_addr - ib_base) / isize), 0);
            re_start = (UINT)((index_addr - ib_base) / isize);
        } else {
        ring_indices:
            u32 off;
            if (!Upload(g_ib_ring, g_ib_pos, kIbRing, idata, count * isize, &off)) return;
            g_ib_uploads++;
            g_ib_ring_bytes += count * isize;
            set_indices(g_ib_ring.Get(), ifmt, off);
            g_ctx->DrawIndexed(count, 0, 0);
        }
    } else {
        g_ctx->Draw(count, 0);
    }
    g_stats.drawn++;
    // F11's capture: how many pixels the draw makes, then without the depth test, then without the
    // pixel shader. What a missing object is lost to: depth, a pixel shader that kills every pixel,
    // or geometry that never reaches the screen. The draw is made again (no colour written), in the
    // captured frame only (not under RenderDoc: its capture would hold these draws too).
    if (g_capture_by_flip && !RenderDoc()) {
        static ComPtr<ID3D11BlendState> no_write;
        if (!no_write) {
            D3D11_BLEND_DESC bd{};
            for (auto& rt : bd.RenderTarget) rt.RenderTargetWriteMask = 0;
            g_dev->CreateBlendState(&bd, &no_write);
        }
        const auto pixels = [&]() -> unsigned long long {
            const D3D11_QUERY_DESC qd{D3D11_QUERY_OCCLUSION, 0};
            ComPtr<ID3D11Query> q;
            if (FAILED(g_dev->CreateQuery(&qd, &q))) return ~0ull;
            g_ctx->Begin(q.Get());
            if (re_indexed) g_ctx->DrawIndexed(re_count, re_start, 0);
            else g_ctx->Draw(re_count, 0);
            g_ctx->End(q.Get());
            UINT64 n = 0;
            while (g_ctx->GetData(q.Get(), &n, sizeof n, 0) == S_FALSE) {
            }
            return n;
        };
        g_ctx->OMSetBlendState(no_write.Get(), nullptr, 0xFFFFFFFF);
        const unsigned long long drawn = pixels();
        g_ctx->OMSetDepthStencilState(g_depth_off.Get(), 0);
        const unsigned long long no_depth = pixels();
        g_ctx->PSSetShader(nullptr, nullptr, 0);
        const unsigned long long no_ps = pixels();
        LOG("    pixels %llu, without the depth test %llu, without the pixel shader %llu", drawn, no_depth, no_ps);
        ForgetBound();
    }
    if (g_capture) DrainDebugMessages(64);  // hazards next to the draw that caused them (CTR_D3D_DEBUG=1)
    // debugging aid: CTR_CAPTURE_AFTER=<shader name part> saves render target 0 right after each
    // matching draw of the captured frame
    static const std::string after = [] {
        char v[128] = {};
        return GetEnvironmentVariableA("CTR_CAPTURE_AFTER", v, sizeof(v)) ? std::string(v) : std::string();
    }();
    // F11's capture also keeps render target 0 after the first grass and double sided G-buffer draws
    static int auto_shots = 0;
    if (!g_capture) auto_shots = 0;
    const bool auto_shot = g_capture_by_flip && ps && auto_shots < 4 &&
                           (ps->name.rfind("GrassOutdoor", 0) == 0 || (prim_setup == 0x240 && dctl == 0x7007b7));
    if (auto_shot) auto_shots++;
    if (g_capture && ((!after.empty() && ps && ps->name.find(after) != std::string::npos) || auto_shot))
        if (u64 h = Q(ctx + CTX_COLOR_TARGETS); h && Readable(h, 0x50))
            if (Surface* s0 = GetColorTarget(h)) {  // copy now (no state change), save at present
                Surface copy = *s0;
                D3D11_TEXTURE2D_DESC td;
                s0->tex->GetDesc(&td);
                copy.tex.Reset(), copy.srv.Reset();
                D3D11_SHADER_RESOURCE_VIEW_DESC sd;
                s0->srv->GetDesc(&sd);
                if (SUCCEEDED(g_dev->CreateTexture2D(&td, nullptr, &copy.tex)) &&
                    SUCCEEDED(g_dev->CreateShaderResourceView(copy.tex.Get(), &sd, &copy.srv))) {
                    g_ctx->CopyResource(copy.tex.Get(), s0->tex.Get());
                    char path[96];
                    snprintf(path, sizeof(path), "after_%llu_%s.bmp", (unsigned long long)g_stats.draws, ps->name.c_str());
                    g_after_shots.emplace_back(g_capture_by_flip ? (g_capture_dir / path).string() : std::string(path), copy);
                }
            }
    if (g_capture) {
        std::string tx;
        for (auto [slot, dims] : ps ? ps->textures : vs->textures) {
            u64 t = Q(ctx + CTX_PS_TEXTURES + slot * 8);
            if (!t) continue;
            TSharp ts = DecodeTSharp((const u32*)t);
            char kind = g_surfaces.count(ts.base) ? 'R' : (g_textures.count(Fnv((const u8*)t, 24)) &&
                                                          g_textures[Fnv((const u8*)t, 24)].srv) ? 'A' : 'P';
            char b[160];
            snprintf(b, sizeof(b), " t%d=%llx(%ux%u f%u n%u m%u t%u %c sel%03x swz%x)", slot, (unsigned long long)ts.base,
                     ts.width, ts.height, ts.dfmt, ts.nfmt, ts.tiling_index, ts.type, kind, ts.dst_sel,
                     SwizzleCode((const u32*)t));
            tx += b;
        }
        u64 rt0 = Q(ctx + CTX_COLOR_TARGETS);
        if (!Readable(rt0, 0x50)) rt0 = 0;
        if (gs) {
            LOG("cap draw %llu: geometry shader %s", (unsigned long long)g_stats.draws, gs->name.c_str());
            u64 hdr = Q(gsh + SH_HEADER);
            s32 n = R<s32>(hdr + HDR_CB_MEMORY);
            u64 mem = Q(hdr + HDR_CB_MEMORY + 0x10), sizes = Q(hdr + HDR_CB_SIZE + 0x10), idx = Q(hdr + HDR_CB_INDEX + 0x10);
            for (s32 k = 0; k < n && k < 4; k++) {
                u64 data = Q(mem + k * 8);
                std::string w;
                for (int i = 28; i < 40 && data; i++) {
                    char b[16];
                    snprintf(b, sizeof(b), " %08x", R<u32>(data + i * 4));
                    w += b;
                }
                LOG("    gs cb slot %u size %llu dw28..39:%s", R<u32>(idx + k * 4), (unsigned long long)R<u64>(sizes + k * 8),
                    w.c_str());
            }
        }
        u64 dh = Q(ctx + CTX_DEPTH_TARGET);
        u64 dbase = dh && Readable(dh, 0x84) ? (u64)R<u32>(dh + 0x58) << 8 : 0;
        LOG("cap draw %llu: vs %s ps %s rt0 %llx nrt %u depth %s %llx vp %d,%d %dx%d prim %u count %u%s blend %08x mask %08x dctl %08x setup %08x rkey %016llx%s",
            (unsigned long long)g_stats.draws, vs->name.c_str(), ps ? ps->name.c_str() : "-",
            (unsigned long long)(rt0 ? (u64)R<u32>(rt0 + 0x10) << 8 : 0), nrt,
            dsv ? (dsv == (depth ? depth->dsv_ro.Get() : nullptr) ? "ro" : "rw") : "-", (unsigned long long)dbase, vp.x,
            vp.y, vp.w, vp.h, prim, count,
            indexed ? " indexed" : "", R<u32>(ctx + CTX_BLEND_CONTROL), R<u32>(ctx + CTX_RT_MASK), dctl,
            R<u32>(ctx + CTX_PRIM_SETUP), (unsigned long long)Q(ctx + CTX_RASTER_STATE), tx.c_str());
        {
            // RE aid: vertex buffer 0 and index buffer resource objects
            u64 rs = Q(ctx + CTX_VERTEX_BUFFERS), sm = Q(ctx + CTX_INDEX_BUFFER);
            std::string txt;
            char b[48];
            for (u64 h : {rs, sm}) {
                snprintf(b, sizeof(b), " | %llx:", (unsigned long long)h);
                txt += b;
                if (h && Readable(h, 32))
                    for (int k = 0; k < 8; k++) {
                        snprintf(b, sizeof(b), " %08x", R<u32>(h + k * 4));
                        txt += b;
                    }
            }
            LOG("    vb0/ib%s", txt.c_str());
        }
        std::string mrts;
        for (int i = 1; i < 8; i++) {
            u64 h = Q(ctx + CTX_COLOR_TARGETS + i * 8);
            if (h && Readable(h, 0x50)) {
                char b[40];
                snprintf(b, sizeof(b), " rt%d=%llx", i, (unsigned long long)((u64)R<u32>(h + 0x10) << 8));
                mrts += b;
            }
        }
        if (!mrts.empty()) LOG("    mrt:%s", mrts.c_str());
    }
}

namespace {
// kClearRenderTarget(ctx, flags, float4* color, stencil, float depth): the depth value travels in
// xmm0 (SysV passes floats in vector registers), the rest in rdi/esi/rdx/ecx.
SYSV void ClearHook(u64 ctx, u32 flags, const float* color, u32 stencil, float depth) {
    std::lock_guard lk(g_mutex);
    if (!Init()) return;
    g_stats.clears++;
    if (g_capture) {
        float z = depth;
        u64 rt0 = Q(ctx + CTX_COLOR_TARGETS);
        if (!Readable(rt0, 0x50)) rt0 = 0;
        auto vp = R<ViewportI>(ctx + CTX_VIEWPORT);
        auto sc = R<ViewportI>(ctx + CTX_SCISSOR);
        u64 dh = Q(ctx + CTX_DEPTH_TARGET);
        LOG("cap clear: flags %x color %.3f %.3f %.3f %.3f depth %f stencil %u rt0 %llx depth %llx vp %d,%d %dx%d "
            "scissor %d %d,%d %dx%d", flags,
            (flags & 1) ? color[0] : 0, (flags & 1) ? color[1] : 0, (flags & 1) ? color[2] : 0,
            (flags & 1) ? color[3] : 0, z, stencil,
            (unsigned long long)(rt0 ? (u64)R<u32>(rt0 + 0x10) << 8 : 0),
            (unsigned long long)(dh && Readable(dh, 0x84) ? (u64)R<u32>(dh + 0x58) << 8 : 0), vp.x, vp.y, vp.w, vp.h,
            R<u8>(ctx + CTX_SCISSOR_ENABLED), sc.x, sc.y, sc.w, sc.h);
    }
    if (g_trace_depth && g_frame >= g_trace_frame && (!g_trace_frame || g_frame < g_trace_frame + 12) && (flags & 6)) {
        u64 dh2 = Q(ctx + CTX_DEPTH_TARGET);
        u64 db = dh2 && Readable(dh2, 0x84) ? (u64)R<u32>(dh2 + 0x58) << 8 : 0;
        if (db && (db == g_trace_depth || g_trace_depth == 1)) {
            char b[48];
            snprintf(b, sizeof(b), " CLEAR(%llx)", (unsigned long long)db);
            g_trace += b;
        }
    }
    if (flags & 6) {
        if (u64 dh = Q(ctx + CTX_DEPTH_TARGET)) {
            if (Surface* d = GetDepthTarget(dh)) {
                float z = depth;
                UINT cf = ((flags & 2) ? D3D11_CLEAR_DEPTH : 0) |
                          ((flags & 4) && d->fmt != DXGI_FORMAT_D16_UNORM ? D3D11_CLEAR_STENCIL : 0);
                g_ctx->ClearDepthStencilView(d->dsv.Get(), cf, z, (u8)stencil);
                d->rendered = true;
            }
        }
    }
    if (flags & 1) {
        for (int i = 0; i < 8; i++) {
            u64 h = Q(ctx + CTX_COLOR_TARGETS + i * 8);
            if (!h) continue;
            if (Surface* s = GetColorTarget(h)) {
                g_ctx->ClearRenderTargetView(s->rtv.Get(), color);
                s->rendered = true;
            }
        }
    }
}

// kCopyTexture(ctx, const T# src, const T# dst, const s32 p[8]): p = src x, y, dst x, y, width,
// height (-1: all), src mip, dst mip, with y counted from the bottom like the viewport. The PS4
// runs a copy compute shader (8x8 threads, user data dst x, dst top, src x, src top, w, h); here
// it is a copy between our surfaces, the destination becoming one if it is not yet. Split screen
// with 3-4 players copies each view's picture this way before its motion blur.
SYSV u64 CopyTextureHook(u64 ctx, const u32* src, const u32* dst, const s32* p) {
    (void)ctx;
    if (!src || !dst) return 0;
    if (p[0] < 0 || p[1] < 0 || p[2] < 0 || p[3] < 0) return 1;
    const TSharp s = DecodeTSharp(src), d = DecodeTSharp(dst);
    const u32 s_last = src[3] < 0xE0000000 ? s.last_level : 0, d_last = dst[3] < 0xE0000000 ? d.last_level : 0;
    if ((u32)p[6] > s_last || (u32)p[7] > d_last) return 1;
    std::lock_guard lk(g_mutex);
    if (!Init()) return 0;
    auto level = [](u32 v, s32 mip) { return std::max<u32>(1, v >> mip); };
    const u32 sw = level(s.width, p[6]), sh = level(s.height, p[6]);
    const u32 dw = level(d.width, p[7]), dh = level(d.height, p[7]);
    u32 w = p[4] >= 0 ? p[4] : sw, h = p[5] >= 0 ? p[5] : sh;
    const u32 sx = std::min<u32>(p[0], sw - 1), sy = std::min<u32>(p[1], sh - 1);
    w = std::min(w, sw - sx), h = std::min(h, sh - sy);
    const u32 dx = std::min<u32>(p[2], dw - 1), dy = std::min<u32>(p[3], dh - 1);
    w = std::min(w, dw - dx), h = std::min(h, dh - dy);
    const u32 s_top = sh - sy - h, d_top = dh - dy - h;

    // source: one of our surfaces, else a texture loaded from guest memory
    ID3D11Texture2D* stex = nullptr;
    ID3D11ShaderResourceView* ssrv = nullptr;
    DXGI_FORMAT sfmt = DXGI_FORMAT_UNKNOWN;
    float ks = 1.0f;
    u32 swap = 0;
    auto it = g_surfaces.find(s.base);
    if (it != g_surfaces.end() && it->second.tex && it->second.srv && Current(it->second)) {
        stex = it->second.tex.Get(), ssrv = it->second.srv.Get(), sfmt = it->second.fmt;
        ks = it->second.scale, swap = it->second.comp_swap;
    } else if (ID3D11ShaderResourceView* v = GetTexture(src, 2); v && v != g_placeholder.srv.Get()) {
        auto tx = g_textures.find(Fnv((const u8*)src, 24));
        if (tx != g_textures.end() && tx->second.tex) stex = tx->second.tex.Get(), ssrv = v, sfmt = tx->second.fmt;
    }
    const DXGI_FORMAT dfmt = TextureFormat(d.dfmt, d.nfmt, d.dst_sel);
    static int logged = 0;
    const bool log = g_capture || logged < 16;
    if (!stex || p[6] || p[7] || dfmt == DXGI_FORMAT_UNKNOWN || !g_copy_ps) {
        if (log) {
            LOG("gfx: copy %llx (%ux%u mip %d%s) -> %llx (%ux%u mip %d format %d) not done", (unsigned long long)s.base,
                s.width, s.height, p[6], stex ? "" : ", no data", (unsigned long long)d.base, d.width, d.height,
                p[7], (int)dfmt);
            logged++;
        }
        return 0;
    }
    const float kd = d.width == level(s.width, 0) && d.height == level(s.height, 0) ? ks : SurfaceScale(d.width, d.height);
    Surface* ds = MakeColorSurface(d.base, d.width, d.height, dfmt, kd, swap, "copy target");
    if (!ds || ds->tex.Get() == stex) return 0;
    g_stats.copies++;
    D3D11_TEXTURE2D_DESC sd, dd;
    stex->GetDesc(&sd), ds->tex->GetDesc(&dd);
    auto px = [](u32 v, float k, UINT limit) { return std::min<UINT>((UINT)(v * k + 0.5f), limit); };
    const UINT ox = px(dx, ds->scale, dd.Width), oy = px(d_top, ds->scale, dd.Height);
    const UINT ow = px(dx + w, ds->scale, dd.Width) - ox, oh = px(d_top + h, ds->scale, dd.Height) - oy;
    const bool depth = sfmt == DXGI_FORMAT_D32_FLOAT_S8X24_UINT || sfmt == DXGI_FORMAT_D16_UNORM;
    if (!depth && TypelessOf(sfmt) == TypelessOf(ds->fmt) && ks == ds->scale) {
        D3D11_BOX box{px(sx, ks, sd.Width), px(s_top, ks, sd.Height), 0, 0, 0, 1};
        box.right = std::min<UINT>(box.left + ow, sd.Width), box.bottom = std::min<UINT>(box.top + oh, sd.Height);
        if (box.right > box.left && box.bottom > box.top)
            g_ctx->CopySubresourceRegion(ds->tex.Get(), 0, ox, oy, 0, stex, 0, &box);
    } else if (ow && oh) {  // depth into a color surface, or a different scale: texel copy shader
        const float ratio = ks / ds->scale;
        const float cb[4] = {ratio, ratio, sx * ks - ox * ratio, s_top * ks - oy * ratio};
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(g_ctx->Map(g_copy_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            memcpy(m.pData, cb, sizeof(cb));
            g_ctx->Unmap(g_copy_cb.Get(), 0);
        }
        ID3D11ShaderResourceView* none[32] = {};
        g_ctx->PSSetShaderResources(0, 32, none);
        g_ctx->OMSetRenderTargets(1, ds->rtv.GetAddressOf(), nullptr);
        D3D11_VIEWPORT v{(float)ox, (float)oy, (float)ow, (float)oh, 0, 1};
        g_ctx->RSSetViewports(1, &v);
        g_ctx->RSSetState(GetRaster(0, false));
        g_ctx->OMSetDepthStencilState(g_depth_off.Get(), 0);
        g_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
        g_ctx->IASetInputLayout(nullptr);
        g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_ctx->VSSetShader(g_blit_vs.Get(), nullptr, 0);
        g_ctx->GSSetShader(nullptr, nullptr, 0);
        g_ctx->PSSetShader(g_copy_ps.Get(), nullptr, 0);
        g_ctx->PSSetConstantBuffers(0, 1, g_copy_cb.GetAddressOf());
        g_ctx->PSSetShaderResources(0, 1, &ssrv);
        g_ctx->Draw(3, 0);
        g_ctx->PSSetShaderResources(0, 1, none);
        ForgetBound();
    }
    if (log) {
        LOG("gfx: copy %llx %u,%u %ux%u -> %llx %u,%u%s", (unsigned long long)s.base, sx, s_top, w, h,
            (unsigned long long)d.base, dx, d_top, depth ? " (depth)" : "");
        logged++;
    }
    ds->rendered = true;
    return 0;
}
}  // namespace

SYSV bool GfxClearHook(u64 ctx, u32 flags, const float* color, u32 stencil, float depth) {
    ClearHook(ctx, flags, color, stencil, depth);
    return true;
}

SYSV bool GfxCopyHook(u64 ctx, const u32* src, const u32* dst, const s32* p) {
    CopyTextureHook(ctx, src, dst, p);
    return true;
}

void GfxStart(ID3D11Device* device, ID3D11DeviceContext* context, IDXGISwapChain1* swap_chain, const char* shaders) {
    std::lock_guard lk(g_mutex);
    if (shaders) {
        SetEnvironmentVariableA("CTR_SHADERS", shaders);
    }
    swap_chain->QueryInterface(IID_PPV_ARGS(&g_swap));
    g_ctx = context;
    g_dev = device; // last: the swap thread may be presenting already
}

void GfxForgetState() {
    ForgetBound();
}

void GfxNotePresent(double ms) {
    g_present_us.fetch_add((u64)(ms * 1000.0), std::memory_order_relaxed);
}

void GfxSetGamma(float gamma) {
    if (gamma > 0.0f && gamma != g_game_gamma.exchange(gamma)) LOG("gfx: the game's gamma is %.3f", gamma);
}

std::mutex& GfxMutex() {
    return g_mutex;
}

namespace {
// ------------------------------------------------------------------ F12 recording
// F12 records the next frames at 1280x720 into %APPDATA%/CTRNF/capture/<time>/ (for hunting
// flicker in play). The GPU scales each picture down, it reaches the CPU four frames later
// through a ring of staging textures and a thread writes the files, so the game keeps its pace.
std::atomic<int> g_record_request{0};
std::mutex g_record_mutex;
std::condition_variable g_record_cv;
std::vector<std::pair<std::string, std::vector<u8>>> g_record_queue;
// test runs: CTR_RECORD_SIZE=<w>x<h> (smaller frames for long recordings), CTR_RECORD_FRAMES=<n>, and
// CTR_GLITCH_TRAP=1, which keeps only the frames that turn suddenly dark (and the one before each),
// logged as "glitch trap" - a long recorded drive without such lines has no single-frame glitches
u32 kRecordW = 1280, kRecordH = 720;
const bool g_glitch_trap = GetEnvironmentVariableA("CTR_GLITCH_TRAP", nullptr, 0) > 0;

void RecordWriter() {
    for (;;) {
        std::pair<std::string, std::vector<u8>> item;
        {
            std::unique_lock lk(g_record_mutex);
            g_record_cv.wait(lk, [] { return !g_record_queue.empty(); });
            item = std::move(g_record_queue.front());
            g_record_queue.erase(g_record_queue.begin());
        }
        WriteBmp(item.first.c_str(), item.second.data(), kRecordW, kRecordH, kRecordW * 4, false);
    }
}

void RecordFrame(ID3D11ShaderResourceView* picture) {
    static int left = 0, n = 0;
    static fs::path dir;
    static ComPtr<ID3D11Texture2D> scaled, ring[4];
    static ComPtr<ID3D11RenderTargetView> rtv;
    static int ring_frame[4] = {-1, -1, -1, -1};
    static float last_dark = 0;
    static std::vector<u8> prev_px;
    auto drain = [&](int slot) {  // the slot's frame to the writer
        if (ring_frame[slot] < 0) return;
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(g_ctx->Map(ring[slot].Get(), 0, D3D11_MAP_READ, 0, &m))) {
            std::vector<u8> px((size_t)kRecordW * kRecordH * 4);
            for (u32 y = 0; y < kRecordH; y++)
                memcpy(&px[(size_t)y * kRecordW * 4], (const u8*)m.pData + (size_t)y * m.RowPitch, kRecordW * 4);
            g_ctx->Unmap(ring[slot].Get(), 0);
            if (g_glitch_trap) {  // only frames much darker than the one before, with that one
                u32 dark = 0, total = 0;
                for (size_t i = 0; i < px.size(); i += 4 * 7, total++)
                    if (px[i] + px[i + 1] + px[i + 2] < 18) dark++;
                const float frac = total ? (float)dark / total : 0, before = last_dark;
                last_dark = frac;
                std::vector<u8> prev = std::move(prev_px);
                prev_px = px;
                if (frac - before <= 0.08f) {
                    ring_frame[slot] = -1;
                    return;
                }
                LOG("glitch trap: recording %d %.0f%% dark after %.0f%%", ring_frame[slot], frac * 100, before * 100);
                if (!prev.empty()) {
                    char name[32];
                    snprintf(name, sizeof(name), "frame_%04d.bmp", ring_frame[slot] - 1);
                    std::lock_guard lk(g_record_mutex);
                    g_record_queue.emplace_back((dir / name).string(), std::move(prev));
                }
            }
            char name[32];
            snprintf(name, sizeof(name), "frame_%04d.bmp", ring_frame[slot]);
            std::lock_guard lk(g_record_mutex);
            g_record_queue.emplace_back((dir / name).string(), std::move(px));
            g_record_cv.notify_one();
        }
        ring_frame[slot] = -1;
    };
    static const u64 record_at = [] {  // test hook: CTR_RECORD_AT=<frame> (as F12 there)
        char v[32] = {};
        return GetEnvironmentVariableA("CTR_RECORD_AT", v, sizeof(v)) ? strtoull(v, nullptr, 10) : 0ull;
    }();
    static bool record_fired = false;
    if (record_at && g_frame >= record_at && !record_fired) {
        record_fired = true;
        char v[32] = {};
        int frames = 180;
        if (GetEnvironmentVariableA("CTR_RECORD_FRAMES", v, sizeof(v))) frames = atoi(v);
        if (GetEnvironmentVariableA("CTR_RECORD_SIZE", v, sizeof(v))) sscanf(v, "%ux%u", &kRecordW, &kRecordH);
        g_record_request = frames;
    }
    if (int frames = g_record_request.exchange(0)) {
        if (!scaled) {
            D3D11_TEXTURE2D_DESC td{};
            td.Width = kRecordW, td.Height = kRecordH, td.MipLevels = td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R8G8B8A8_UNORM, td.SampleDesc.Count = 1;
            td.BindFlags = D3D11_BIND_RENDER_TARGET;
            g_dev->CreateTexture2D(&td, nullptr, &scaled);
            g_dev->CreateRenderTargetView(scaled.Get(), nullptr, &rtv);
            td.BindFlags = 0, td.Usage = D3D11_USAGE_STAGING, td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            for (auto& r : ring) g_dev->CreateTexture2D(&td, nullptr, &r);
            std::thread(RecordWriter).detach();
        }
        SYSTEMTIME t;
        GetLocalTime(&t);
        char stamp[32];
        snprintf(stamp, sizeof(stamp), "%04d%02d%02d_%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
                 t.wSecond);
        dir = UserRoot() / "capture" / stamp;
        std::error_code ec;
        fs::create_directories(dir, ec);
        left = frames, n = 0;
        LOG("gfx: recording %d frames into %s", frames, dir.string().c_str());
    }
    if (left <= 0) {
        for (int s = 0; s < 4; s++) drain(s);  // the last few after the recording
        return;
    }
    const int slot = n % 4;
    drain(slot);  // four frames old: the GPU is done with it
    ID3D11ShaderResourceView* none[1] = {};
    g_ctx->PSSetShaderResources(0, 1, none);
    g_ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
    D3D11_VIEWPORT v{0, 0, (float)kRecordW, (float)kRecordH, 0, 1};
    g_ctx->RSSetViewports(1, &v);
    g_ctx->RSSetState(GetRaster(0, false));
    g_ctx->PSSetShaderResources(0, 1, &picture);
    g_ctx->Draw(3, 0);
    g_ctx->CopyResource(ring[slot].Get(), scaled.Get());
    ring_frame[slot] = n++;
    left--;
}
}  // namespace

// Copies of VideoOut buffers made when the game flips them (GfxSnapshot). This renderer draws as
// the engine records, not when the PS4's GPU would run the command buffer, so by the time the swap
// thread presents a flip the game may already be drawing a later frame into the same buffer (its
// wait for the flip is a GPU wait, which nothing here holds it to): presented as it was then, the
// picture flickered between frames half drawn. The native Vulkan renderer copies at the flip too.
// sceGnmDrawIndexOffset (v0.8.8): an indexed draw from an offset into the index buffer the engine
// has bound (its context's index buffer object: size at +0, base at +8). The vegetation draws this
// way; only DrawIndex and DrawIndexAuto came here before, so they were left out.
void GfxDrawOffset(u32 count, u32 index_offset) {
    const u64 dev = g_image_base ? Q(g_image_base + kDeviceGlobal) : 0;
    const u64 ctx = dev ? Q(dev + DEV_CONTEXT) : 0;
    const u64 ib = ctx ? Q(ctx + CTX_INDEX_BUFFER) : 0;
    const u64 base = ib && Readable(ib, 16) ? Q(ib + 8) : 0;
    const u32 isize = ctx ? R<u32>(ctx + CTX_INDEX_SIZE) : 0;
    if (!base || (isize != 2 && isize != 4)) {
        GfxDrawUnsupported("sceGnmDrawIndexOffset (no index buffer)");
        return;
    }
    GfxDraw(count, base + (u64)index_offset * isize, true);
}

void GfxDrawUnsupported(const char* kind) {
    std::lock_guard lk(g_mutex);
    static std::unordered_map<std::string, u64> seen;
    if (seen[kind]++ == 0) LOG("gfx: the game draws with %s, which this renderer does not make yet", kind);
    if (!g_capture || !g_image_base) return;
    const u64 dev = Q(g_image_base + kDeviceGlobal);
    const u64 ctx = dev ? Q(dev + DEV_CONTEXT) : 0;
    const ShaderInfo* vs = ctx ? GetShader(Q(ctx + CTX_VS), true) : nullptr;
    const u64 psh = ctx ? Q(ctx + CTX_PS) : 0;
    const ShaderInfo* ps = psh ? GetShader(psh, false) : nullptr;
    LOG("cap draw skipped: %s vs %s ps %s", kind, vs ? vs->name.c_str() : "?", ps ? ps->name.c_str() : "-");
}

// Frame captures. CTR_CAPTURE=<frame>: every render target into the game's folder; F11: the game's
// next frame, from one flip to the next, into the user folder's capture/frame_<n>/ (the draws in
// draws.txt there too, and only the larger colour targets: a frame has some 80).

void SaveCapture(const fs::path& dir) {
    int n = 0;
    for (auto& [base, surf] : g_surfaces) {
        if (!surf.rendered) continue;
        if (!dir.empty() && (surf.dsv || surf.w < 480 || n >= 24)) continue;
        char name[96];
        snprintf(name, sizeof(name), "cap_%05llu_%llx_%ux%u.bmp", (unsigned long long)g_frame - 1,
                 (unsigned long long)base, surf.w, surf.h);
        const std::string path = dir.empty() ? std::string(name) : (dir / name).string();
        if (surf.dsv) SaveDepth(surf, path.c_str());
        else SaveSurface(surf, path.c_str());
        for (auto& [p, copy] : g_after_shots) SaveSurface(copy, p.c_str());
        g_after_shots.clear();
        static const std::pair<u32, u32> probe = [] {
            char v[32] = {};
            u32 x = 960, y = 720;
            if (GetEnvironmentVariableA("CTR_PROBE", v, sizeof(v))) sscanf(v, "%u,%u", &x, &y);
            return std::make_pair(x, y);
        }();
        // probe coordinates are given for 1920x1080; scale to the surface
        ProbePixel(base, surf, probe.first * surf.w / 1920, probe.second * surf.h / 1080);
        n++;
    }
    LOG("cap: frame %llu captured, %d render targets saved%s%s", (unsigned long long)g_frame - 1, n,
        dir.empty() ? "" : " in ", dir.empty() ? "" : dir.string().c_str());
}

struct Snapshot {
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11ShaderResourceView> srv;
    u64 addr = 0;
};
constexpr u32 kSnapshots = 6;  // flips the game may have submitted ahead of the screen
Snapshot g_snapshots[kSnapshots];
u32 g_next_snapshot = 0;

u32 GfxSnapshot(u64 addr) {
    std::lock_guard lk(g_mutex);
    if (g_capture_by_flip) {  // F11: the frame drawn since the last flip, done
        g_capture = g_capture_by_flip = false;
        if (RENDERDOC_API_1_1_2* rd = RenderDoc()) {
            rd->EndFrameCapture(nullptr, nullptr);
            LOG("cap: RenderDoc capture saved in %s", g_capture_dir.string().c_str());
        }
        SaveCapture(g_capture_dir);
        if (g_log_mirror) std::fclose(std::exchange(g_log_mirror, nullptr));
    } else if (g_capture_armed) {
        g_capture_armed = false;
        std::error_code ec;
        g_capture_dir = UserRoot() / "capture" / ("frame_" + std::to_string(g_frame));
        fs::create_directories(g_capture_dir, ec);
        g_log_mirror = _wfopen((g_capture_dir / "draws.txt").c_str(), L"w");
        g_capture = g_capture_by_flip = true;
        LOG("cap: from this flip to the next, into %s", g_capture_dir.string().c_str());
        if (RENDERDOC_API_1_1_2* rd = RenderDoc()) {
            rd->SetCaptureFilePathTemplate((g_capture_dir / "renderdoc").string().c_str());
            rd->StartFrameCapture(nullptr, nullptr);
        }
    }
    if (!g_dev || !addr) return 0;
    auto it = g_surfaces.find(addr);
    if (it == g_surfaces.end() || !it->second.tex || !it->second.srv) return 0;
    D3D11_TEXTURE2D_DESC want;
    it->second.tex->GetDesc(&want);
    const u32 slot = g_next_snapshot++ % kSnapshots;
    Snapshot& s = g_snapshots[slot];
    D3D11_TEXTURE2D_DESC have{};
    if (s.tex) s.tex->GetDesc(&have);
    if (!s.tex || have.Width != want.Width || have.Height != want.Height || have.Format != want.Format ||
        have.MipLevels != want.MipLevels || have.ArraySize != want.ArraySize ||
        have.SampleDesc.Count != want.SampleDesc.Count) {
        s = Snapshot{};
        D3D11_TEXTURE2D_DESC d = want;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        d.CPUAccessFlags = 0;
        d.MiscFlags = 0;
        D3D11_SHADER_RESOURCE_VIEW_DESC view;
        it->second.srv->GetDesc(&view);
        if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &s.tex)) ||
            FAILED(g_dev->CreateShaderResourceView(s.tex.Get(), &view, &s.srv))) {
            static int warned = 0;
            if (!warned++) LOG("gfx: no copy of the flipped picture (format %d): shown as drawn", (int)want.Format);
            s = Snapshot{};
            return 0;
        }
    }
    g_ctx->CopyResource(s.tex.Get(), it->second.tex.Get());
    s.addr = addr;
    return slot + 1;
}

void GfxPresent(u64 addr, ID3D11RenderTargetView* target, u32 snapshot) {
    if (!Init()) return;
    g_swap_rtv = target;
    static const bool trace_init = [] {
        char v[64] = {};
        if (GetEnvironmentVariableA("CTR_TRACE_DEPTH", v, sizeof(v)))
            sscanf(v, "%llx,%llu", (unsigned long long*)&g_trace_depth, (unsigned long long*)&g_trace_frame);
        return true;
    }();
    (void)trace_init;
    if (!g_trace.empty()) {  // compress "d0,2048 d0,2048 ..." runs
        std::string out, last;
        int n = 0;
        std::istringstream in(g_trace);
        for (std::string w; in >> w;) {
            if (w == last) { n++; continue; }
            if (!last.empty()) out += " " + last + "x" + std::to_string(n);
            last = w, n = 1;
        }
        if (!last.empty()) out += " " + last + "x" + std::to_string(n);
        static std::string prev;
        if (out != prev) LOG("trace frame %llu:%s", (unsigned long long)g_frame, out.c_str());
        prev = out;
        g_trace.clear();
    }
    g_frame++;
    static bool removed_logged = false;
    if (HRESULT rr = g_dev->GetDeviceRemovedReason(); rr != S_OK && !removed_logged) {
        removed_logged = true;
        LOG("gfx: D3D11 DEVICE REMOVED at frame %llu, reason 0x%08lx", (unsigned long long)g_frame, rr);
    }
    static double last = NowMs();
    static u64 last_frame = 0, last_draws = 0;
    double now = NowMs();
    if (now - last > 2000) {
        double secs = (now - last) / 1000.0;
        u64 nf = std::max<u64>(1, g_frame - last_frame);
        LOG("gfx: %.1f fps, %.0f draws/frame, %.1f ms/frame in GfxDraw (cb %.1f, vb/ib map %.1f, issue %.1f), "
            "present %.1f",
            (g_frame - last_frame) / secs, double(g_stats.draws - last_draws) / nf, g_draw_ms / nf,
            g_cb_ms / nf, g_map_ms / nf, g_issue_ms / nf, g_present_us.exchange(0) / 1000.0 / nf);
        LOG("gfx: sections ms/frame: shader %.1f targets %.1f state %.1f layout %.1f geometry %.1f vb %.1f setshader %.1f cb+tex %.1f constants %.1f",
            g_sec[0] / nf, g_sec[1] / nf, g_sec[2] / nf, g_sec[3] / nf, g_sec[4] / nf, g_sec[5] / nf,
            g_sec[6] / nf, g_sec[7] / nf, g_sec[8] / nf);
        memset(g_sec, 0, sizeof(g_sec));
        LOG("gfx: streams per frame: %.0f vertex on the GPU, %.0f uploaded (%.0f KB), %.0f index on the GPU, %.0f uploaded (%.0f KB)",
            g_vb_cached / (double)std::max<u64>(g_frame - last_frame, 1), g_vb_uploads / (double)std::max<u64>(g_frame - last_frame, 1),
            g_vb_ring_bytes / 1024.0 / std::max<u64>(g_frame - last_frame, 1), g_ib_cached / (double)std::max<u64>(g_frame - last_frame, 1),
            g_ib_uploads / (double)std::max<u64>(g_frame - last_frame, 1), g_ib_ring_bytes / 1024.0 / std::max<u64>(g_frame - last_frame, 1));
        g_vb_cached = g_vb_uploads = g_vb_ring_bytes = g_ib_cached = g_ib_uploads = g_ib_ring_bytes = 0;
        LOG("gfx: stalls over %.1f s: lock %.1f ms, shaders %.1f, %llu textures built %.1f ms (%llu KB), refreshed %.1f ms (%llu KB), buffers %.1f; worst draw %.1f ms",
            secs, g_lock_ms, g_shader_ms, (unsigned long long)g_builds, g_build_ms, (unsigned long long)(g_build_bytes >> 10),
            g_refresh_ms, (unsigned long long)(g_refresh_bytes >> 10), g_buffer_ms, g_worst_draw_ms);
        g_lock_ms = g_shader_ms = g_build_ms = g_refresh_ms = g_buffer_ms = g_worst_draw_ms = 0;
        g_builds = g_build_bytes = g_refresh_bytes = 0;
        g_cb_ms = g_map_ms = g_issue_ms = 0;
        last = now;
        last_frame = g_frame;
        last_draws = g_stats.draws;
        g_draw_ms = 0;
    }
    if ((g_frame & 127) == 1) {
        LOG("gfx: frame %llu draws %llu drawn %llu no_shader %llu no_target %llu no_layout %llu clears %llu "
            "copies %llu texture refreshes %llu buffers created %llu refreshed %llu (%zu cached)",
            (unsigned long long)g_frame, (unsigned long long)g_stats.draws, (unsigned long long)g_stats.drawn,
            (unsigned long long)g_stats.no_shader, (unsigned long long)g_stats.no_target,
            (unsigned long long)g_stats.no_layout, (unsigned long long)g_stats.clears,
            (unsigned long long)g_stats.copies, (unsigned long long)g_stats.texture_refresh,
            (unsigned long long)g_stats.buffer_create, (unsigned long long)g_stats.buffer_refresh, g_gpu_buffers.size());
        EvictBuffers();
        EvictTextures();
    }
    if ((g_frame & 127) == 1) DrainDebugMessages();
    if (g_capture && !g_capture_by_flip) {  // CTR_CAPTURE=<frame>: from this picture to the next
        g_capture = false;
        SaveCapture(fs::path());
    }
    // F11 with the game's window in front: the game's next frame is captured, from one flip to the
    // next (GfxSnapshot). A lab aid only: under RenderDoc (BB_RENDERDOC) or with BB_D3D11_CAPTURE=1;
    // a player's F11 would stop the game a moment and write some 100 MB of pictures.
    static const bool capture_key = [] {
        const char* v = std::getenv("BB_D3D11_CAPTURE");
        return (v && v[0] == '1') || RenderDoc() != nullptr;
    }();
    if (capture_key) {
        static bool held = false;
        DWORD pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        const bool down = pid == GetCurrentProcessId() && (GetAsyncKeyState(VK_F11) & 0x8000);
        if (down && !held && !g_capture) {
            g_capture_armed = true;
            LOG("cap: F11 pressed");
        }
        held = down;
    }
    if (g_capture_frame && g_frame == g_capture_frame) {
        g_capture = true;
        LOG("cap: capturing frame %llu", (unsigned long long)g_frame);
    }
    auto it = g_surfaces.find(addr);
    // the copy made at the flip, while it still holds this buffer (not taken over by a later flip)
    ID3D11ShaderResourceView* picture = it != g_surfaces.end() ? it->second.srv.Get() : nullptr;
    if (snapshot >= 1 && snapshot <= kSnapshots && g_snapshots[snapshot - 1].addr == addr && g_snapshots[snapshot - 1].srv)
        picture = g_snapshots[snapshot - 1].srv.Get();
    float black[4] = {0, 0, 0, 1};
    g_ctx->OMSetRenderTargets(1, g_swap_rtv.GetAddressOf(), nullptr);
    g_ctx->ClearRenderTargetView(g_swap_rtv.Get(), black);
    if (picture && g_blit_vs) {
        // 16:9 picture, letterboxed into the back buffer
        DXGI_SWAP_CHAIN_DESC sd;
        g_swap->GetDesc(&sd);
        float bw = (float)sd.BufferDesc.Width, bh = (float)sd.BufferDesc.Height;
        float vw = std::min(bw, bh * 16.0f / 9.0f), vh = vw * 9.0f / 16.0f;
        D3D11_VIEWPORT v{(bw - vw) * 0.5f, (bh - vh) * 0.5f, vw, vh, 0, 1};
        g_picture[0] = v.TopLeftX / bw, g_picture[1] = v.TopLeftY / bh;
        g_picture[2] = (v.TopLeftX + vw) / bw, g_picture[3] = (v.TopLeftY + vh) / bh;
        g_ctx->RSSetViewports(1, &v);
        g_ctx->RSSetState(GetRaster(0, false));
        g_ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
        g_ctx->OMSetDepthStencilState(g_depth_off.Get(), 0);
        g_ctx->IASetInputLayout(nullptr);
        g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_ctx->VSSetShader(g_blit_vs.Get(), nullptr, 0);
        g_ctx->PSSetShader(g_blit_ps.Get(), nullptr, 0);
        {
            const float constants[4] = {g_game_gamma.load(std::memory_order_relaxed), 0, 0, 0};
            D3D11_MAPPED_SUBRESOURCE m;
            if (SUCCEEDED(g_ctx->Map(g_copy_cb.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
                memcpy(m.pData, constants, sizeof(constants));
                g_ctx->Unmap(g_copy_cb.Get(), 0);
            }
            g_ctx->PSSetConstantBuffers(0, 1, g_copy_cb.GetAddressOf());
        }
        g_ctx->PSSetShaderResources(0, 1, &picture);
        g_ctx->PSSetSamplers(0, 1, g_samp_linear.GetAddressOf());
        g_ctx->Draw(3, 0);
        RecordFrame(picture);
    }
    // CTR_SCREENSHOT=<n>: every n-th frame; =<first>+<count>: that many frames in a row (flicker)
    static u64 shot_frame = 0, shot_count = 0;
    static const bool shot_init = [] {
        char v[32] = {};
        if (GetEnvironmentVariableA("CTR_SCREENSHOT", v, sizeof(v))) {
            char* end = nullptr;
            shot_frame = strtoull(v, &end, 10);
            if (end && *end == '+') shot_count = strtoull(end + 1, nullptr, 10);
        }
        return true;
    }();
    (void)shot_init;
    const bool shot = shot_count ? g_frame >= shot_frame && g_frame < shot_frame + shot_count
                                 : shot_frame && (g_frame == shot_frame || g_frame % shot_frame == 0);
    if (shot) {
        char path[64];
        snprintf(path, sizeof(path), "shot_%05llu.bmp", (unsigned long long)g_frame);
        SaveScreenshot(path);
    }
    g_swap_rtv.Reset();
}

