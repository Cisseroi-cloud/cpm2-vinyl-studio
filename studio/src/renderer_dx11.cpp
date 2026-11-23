#include "renderer_dx11.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

#define STB_TRUETYPE_IMPLEMENTATION
#include "../../third_party/stb/stb_truetype.h"
#include "../../third_party/stb/stb_image_write.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

namespace cpm {

// ================================ HLSL SHADERS ================================

static const char* kCarShaderHLSL = R"(
cbuffer CarCB : register(b0) {
    float4x4 uViewProj;
    float4 uCamPos;
    float4 uBaseColor;
    float4 uParams; // x=roughness, y=metallic, z=surfaceType(0=body,1=win,2=plate,3=static), w=heatmapMode
};

Texture2D uVinylTex : register(t0);
SamplerState uSamp : register(s0);

struct VSIn {
    float3 pos : POSITION;
    float3 norm : NORMAL;
    float2 uv : TEXCOORD0;
    float dist : TEXCOORD1;
};

struct PSIn {
    float4 svPos : SV_POSITION;
    float3 worldPos : TEXCOORD0;
    float3 worldNorm : TEXCOORD1;
    float2 uv : TEXCOORD2;
    float dist : TEXCOORD3;
};

PSIn VSMain(VSIn i) {
    PSIn o;
    o.svPos = mul(uViewProj, float4(i.pos, 1.0));
    o.worldPos = i.pos;
    o.worldNorm = i.norm;
    o.uv = i.uv;
    o.dist = i.dist;
    return o;
}

float3 heatmapColor(float ratio) {
    // ratio 1.0 = isotropic green, 1.8 = yellow, >=3.0 = red
    float t = saturate((ratio - 1.0) / 2.2);
    float3 c0 = float3(0.10, 0.85, 0.30);
    float3 c1 = float3(0.98, 0.82, 0.12);
    float3 c2 = float3(0.95, 0.16, 0.14);
    return t < 0.5 ? lerp(c0, c1, t * 2.0) : lerp(c1, c2, (t - 0.5) * 2.0);
}

float4 PSMain(PSIn i) : SV_TARGET {
    float3 N = normalize(i.worldNorm);
    float3 V = normalize(uCamPos.xyz - i.worldPos);
    if (dot(N, V) < 0.0) N = -N;

    float rough = clamp(uParams.x, 0.04, 0.95);
    float metal = saturate(uParams.y);
    int surfType = (int)(uParams.z + 0.5);
    bool showHeatmap = (uParams.w > 0.5);

    // Sample vinyl RT (DX11 v=0 is top, Unity v=1 is top -> flip V)
    float4 vinyl = float4(0, 0, 0, 0);
    if (surfType <= 2) {
        bool inUV = (i.uv.x >= 0.0 && i.uv.x <= 1.0 && i.uv.y >= 0.0 && i.uv.y <= 1.0);
        if (inUV) {
            vinyl = uVinylTex.Sample(uSamp, float2(i.uv.x, 1.0 - i.uv.y));
        }
    }

    float3 albedo = uBaseColor.rgb;
    float outAlpha = uBaseColor.a;

    if (surfType == 0 || surfType == 2) {
        albedo = lerp(albedo, vinyl.rgb, vinyl.a);
        outAlpha = 1.0;
    } else if (surfType == 1) {
        // Semi-transparent tinted glass with opaque vinyls on top!
        albedo = lerp(albedo, vinyl.rgb, vinyl.a);
        outAlpha = saturate(lerp(uBaseColor.a, 1.0, vinyl.a));
    }

    if (showHeatmap && surfType <= 2) {
        albedo = lerp(albedo, heatmapColor(i.dist), 0.72);
    }

    // Studio 3-point + hemisphere automotive lighting
    float3 L1 = normalize(float3(0.55, 0.85, 0.65));
    float3 L2 = normalize(float3(-0.70, 0.45, -0.55));
    float ndl1 = saturate(dot(N, L1));
    float ndl2 = saturate(dot(N, L2));

    float3 hemi = lerp(float3(0.14, 0.15, 0.18), float3(0.42, 0.46, 0.54), N.y * 0.5 + 0.5);
    float3 diffuse = albedo * (hemi + ndl1 * float3(0.95, 0.93, 0.90) + ndl2 * float3(0.32, 0.38, 0.48));

    // Specular + clearcoat Fresnel
    float3 H1 = normalize(L1 + V);
    float3 H2 = normalize(L2 + V);
    float shin = lerp(128.0, 12.0, rough);
    float spec1 = pow(saturate(dot(N, H1)), shin);
    float spec2 = pow(saturate(dot(N, H2)), shin * 0.6);
    float fresnel = pow(1.0 - saturate(dot(N, V)), 4.0);

    float3 specCol = lerp(float3(0.08, 0.08, 0.08), albedo, metal);
    float3 color = diffuse * (1.0 - metal * 0.45) + specCol * (spec1 * 1.1 + spec2 * 0.45);

    if (surfType == 0 || surfType == 1) {
        // Automotive clearcoat / glass reflection
        float coat = pow(saturate(dot(N, H1)), 96.0) * 0.65 + fresnel * (surfType == 1 ? 0.55 : 0.35);
        color += float3(0.92, 0.95, 1.0) * coat;
    }

    // Subtle Reinhard tone mapping
    color = color / (1.0 + color * 0.25);
    return float4(saturate(color), outAlpha);
}
)";

static const char* kVinylShaderHLSL = R"(
cbuffer VinylCB : register(b0) {
    float4 uMode; // x=0 -> Texture2DArray sprite, x=1 -> single Texture2D text glyph
};

Texture2DArray<float> uSpriteArray : register(t0);
Texture2D<float> uTextTex : register(t1);
SamplerState uSamp : register(s0);

struct VSIn {
    float2 quadPos : POSITION;   // [-0.5, +0.5]
    float2 quadUV  : TEXCOORD0;  // [0, 1] (top-left = 0,0)
    // Per-instance attributes
    float4 instPosScale : INST0; // px, py, wUV (signed for flipX), hUV
    float4 instRotSlice : INST1; // rotRad, sliceIdx, 0, 0
    float4 instColor    : INST2; // straight RGBA in [0, 1]
};

struct PSIn {
    float4 svPos : SV_POSITION;
    float2 uv    : TEXCOORD0;
    float slice  : TEXCOORD1;
    float4 color : COLOR0;
};

PSIn VSMain(VSIn i) {
    PSIn o;
    float px = i.instPosScale.x;
    float py = i.instPosScale.y;
    float wUV = i.instPosScale.z; // may be negative for horizontal mirror!
    float hUV = i.instPosScale.w;
    float rad = i.instRotSlice.x;

    float lx = i.quadPos.x * wUV;
    float ly = i.quadPos.y * hUV;
    float c = cos(rad), s = sin(rad);
    float rx = lx * c - ly * s;
    float ry = lx * s + ly * c;

    // CPM2 Vinyl plane [-0.5, +0.5] -> DX11 NDC [-1, +1]
    float ndcX = (px + rx) * 2.0;
    float ndcY = (py + ry) * 2.0;
    o.svPos = float4(ndcX, ndcY, 0.5, 1.0);
    o.uv = i.quadUV;
    o.slice = i.instRotSlice.y;
    o.color = i.instColor;
    return o;
}

float4 PSMain(PSIn i) : SV_TARGET {
    float maskA = (uMode.x < 0.5)
        ? uSpriteArray.Sample(uSamp, float3(i.uv, i.slice))
        : uTextTex.Sample(uSamp, i.uv);
    float a = maskA * i.color.a;
    if (a < 0.002) discard;
    return float4(i.color.rgb, a);
}
)";

static const char* kLineShaderHLSL = R"(
cbuffer LineCB : register(b0) {
    float4x4 uViewProj;
};
struct VSIn {
    float3 pos : POSITION;
    float4 col : COLOR0;
};
struct PSIn {
    float4 svPos : SV_POSITION;
    float4 col   : COLOR0;
};
PSIn VSMain(VSIn i) {
    PSIn o;
    o.svPos = mul(uViewProj, float4(i.pos, 1.0));
    o.col = i.col;
    return o;
}
float4 PSMain(PSIn i) : SV_TARGET {
    return i.col;
}
)";

static const char* kUVWireShaderHLSL = R"(
cbuffer CarCB : register(b0) {
    float4x4 uUnused;
    float4 uUnused2;
    float4 uWireColor;
    float4 uUnused3;
};
struct VSIn {
    float3 pos : POSITION;
    float3 norm : NORMAL;
    float2 uv : TEXCOORD0;
    float dist : TEXCOORD1;
};
struct PSIn {
    float4 svPos : SV_POSITION;
    float4 col : COLOR0;
};
PSIn VSMain(VSIn i) {
    PSIn o;
    o.svPos = float4(i.uv.x * 2.0 - 1.0, i.uv.y * 2.0 - 1.0, 0.5, 1.0);
    o.col = uWireColor;
    return o;
}
float4 PSMain(PSIn i) : SV_TARGET {
    return i.col;
}
)";

static const char* kFSQuadShaderHLSL = R"(
Texture2D uTex : register(t0);
SamplerState uSamp : register(s0);
struct PSIn {
    float4 svPos : SV_POSITION;
    float2 uv : TEXCOORD0;
};
PSIn VSMain(uint id : SV_VertexID) {
    PSIn o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.svPos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.5, 1.0);
    return o;
}
float4 PSMain(PSIn i) : SV_TARGET {
    // Subtle dark checkerboard behind transparent vinyl RT
    float2 cell = floor(i.uv * 40.0);
    float chk = fmod(cell.x + cell.y, 2.0);
    float3 bg = lerp(float3(0.08, 0.09, 0.11), float3(0.12, 0.13, 0.16), chk);
    float4 v = uTex.Sample(uSamp, i.uv);
    float3 col = lerp(bg, v.rgb, v.a);
    // Subtle 0.1 UV grid lines
    float2 g = abs(frac(i.uv * 10.0 + 0.5) - 0.5) / max(fwidth(i.uv * 10.0), 0.001);
    float gl = 1.0 - saturate(min(g.x, g.y));
    col = lerp(col, float3(0.35, 0.42, 0.55), gl * 0.35);
    return float4(col, 1.0);
}
)";

// ================================ STRUCTS ================================

struct CarCBData {
    float viewProj[16];
    float camPos[4];
    float baseColor[4];
    float params[4];
};

struct VinylInstance {
    float px, py, wUV, hUV;
    float rotRad, sliceIdx, pad0, pad1;
    float r, g, b, a;
};

struct QuadVertex {
    float x, y, u, v;
};

static bool compileShader(const char* src, const char* entry, const char* target, ComPtr<ID3DBlob>& outBlob) {
    ComPtr<ID3DBlob> err;
    HRESULT hr = D3DCompile(src, std::strlen(src), nullptr, nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &outBlob, &err);
    if (FAILED(hr)) {
        if (err) OutputDebugStringA((const char*)err->GetBufferPointer());
        return false;
    }
    return true;
}

// ================================ IMPLEMENTATION ================================

bool RendererDX11::init(HWND hwnd, int width, int height) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = (UINT)std::max(1, width);
    sd.BufferDesc.Height = (UINT)std::max(1, height);
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL flOut;
    const D3D_FEATURE_LEVEL fls[2] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        fls, 2, D3D11_SDK_VERSION, &sd, &m_swapChain, &m_device, &flOut, &m_ctx
    );
    if (FAILED(hr)) return false;

    if (!createMainRTV()) return false;
    if (!createShaders()) return false;
    if (!createVinylRTs()) return false;

    // Load system fonts for live Unicode / JDM text rendering
    auto loadFileBytes = [](const char* path, std::vector<uint8_t>& out) {
        std::ifstream f(path, std::ios::binary);
        if (!f) return;
        f.seekg(0, std::ios::end);
        auto sz = f.tellg();
        if (sz <= 0) return;
        f.seekg(0, std::ios::beg);
        out.resize((size_t)sz);
        f.read(reinterpret_cast<char*>(out.data()), sz);
    };
    loadFileBytes("C:\\Windows\\Fonts\\arial.ttf", m_fontDataPrimary);
    loadFileBytes("C:\\Windows\\Fonts\\seguisym.ttf", m_fontDataSymbol);

    return true;
}

void RendererDX11::shutdown() {
    m_previewTexs.clear();
    m_textCache.clear();
    m_carSubmeshes.clear();
    m_wheelSubmeshes.clear();
    m_mainRTV.Reset();
    m_swapChain.Reset();
    m_ctx.Reset();
    m_device.Reset();
}

bool RendererDX11::createMainRTV() {
    m_mainRTV.Reset();
    ComPtr<ID3D11Texture2D> backBuf;
    if (FAILED(m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuf)))) return false;
    return SUCCEEDED(m_device->CreateRenderTargetView(backBuf.Get(), nullptr, &m_mainRTV));
}

void RendererDX11::resizeSwapChain(int width, int height) {
    if (!m_swapChain || width <= 0 || height <= 0) return;
    m_ctx->OMSetRenderTargets(0, nullptr, nullptr);
    m_mainRTV.Reset();
    m_swapChain->ResizeBuffers(0, (UINT)width, (UINT)height, DXGI_FORMAT_UNKNOWN, 0);
    createMainRTV();
}

bool RendererDX11::createShaders() {
    // Rasterizer states
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_BACK;
    rd.FrontCounterClockwise = FALSE; // Unity LH CW front faces match DX11 default!
    rd.DepthClipEnable = TRUE;
    m_device->CreateRasterizerState(&rd, &m_rsSolid);

    rd.CullMode = D3D11_CULL_NONE;
    m_device->CreateRasterizerState(&rd, &m_rsNoCull);

    rd.FillMode = D3D11_FILL_WIREFRAME;
    rd.CullMode = D3D11_CULL_NONE;
    m_device->CreateRasterizerState(&rd, &m_rsWire);

    // Depth stencil states
    D3D11_DEPTH_STENCIL_DESC dsd{};
    dsd.DepthEnable = TRUE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dsd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    m_device->CreateDepthStencilState(&dsd, &m_dsDefault);

    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    m_device->CreateDepthStencilState(&dsd, &m_dsReadOnly);

    dsd.DepthEnable = FALSE;
    m_device->CreateDepthStencilState(&dsd, &m_dsOff);

    // Blend states
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable = FALSE;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    m_device->CreateBlendState(&bd, &m_bsOpaque);

    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    m_device->CreateBlendState(&bd, &m_bsAlpha);

    // Sampler
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    m_device->CreateSamplerState(&sd, &m_sampLinearClamp);

    // 1. Car shader
    ComPtr<ID3DBlob> vsB, psB;
    if (!compileShader(kCarShaderHLSL, "VSMain", "vs_5_0", vsB)) return false;
    if (!compileShader(kCarShaderHLSL, "PSMain", "ps_5_0", psB)) return false;
    m_device->CreateVertexShader(vsB->GetBufferPointer(), vsB->GetBufferSize(), nullptr, &m_carVS);
    m_device->CreatePixelShader(psB->GetBufferPointer(), psB->GetBufferSize(), nullptr, &m_carPS);

    D3D11_INPUT_ELEMENT_DESC carElems[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32_FLOAT,       0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    m_device->CreateInputLayout(carElems, 4, vsB->GetBufferPointer(), vsB->GetBufferSize(), &m_carIL);

    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth = sizeof(CarCBData);
    cbd.Usage = D3D11_USAGE_DYNAMIC;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    m_device->CreateBuffer(&cbd, nullptr, &m_carCB);

    // 2. Vinyl compositor instanced shader
    if (!compileShader(kVinylShaderHLSL, "VSMain", "vs_5_0", vsB)) return false;
    if (!compileShader(kVinylShaderHLSL, "PSMain", "ps_5_0", psB)) return false;
    m_device->CreateVertexShader(vsB->GetBufferPointer(), vsB->GetBufferSize(), nullptr, &m_vinylVS);
    m_device->CreatePixelShader(psB->GetBufferPointer(), psB->GetBufferSize(), nullptr, &m_vinylPS);

    D3D11_INPUT_ELEMENT_DESC vinElems[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 8,  D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"INST",     0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0,  D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"INST",     1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"INST",     2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 32, D3D11_INPUT_PER_INSTANCE_DATA, 1},
    };
    m_device->CreateInputLayout(vinElems, 5, vsB->GetBufferPointer(), vsB->GetBufferSize(), &m_vinylIL);

    const QuadVertex quad[6] = {
        {-0.5f, +0.5f, 0.0f, 0.0f},
        {+0.5f, +0.5f, 1.0f, 0.0f},
        {+0.5f, -0.5f, 1.0f, 1.0f},
        {-0.5f, +0.5f, 0.0f, 0.0f},
        {+0.5f, -0.5f, 1.0f, 1.0f},
        {-0.5f, -0.5f, 0.0f, 1.0f},
    };
    D3D11_BUFFER_DESC qbd{};
    qbd.ByteWidth = sizeof(quad);
    qbd.Usage = D3D11_USAGE_IMMUTABLE;
    qbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA qsd{quad, 0, 0};
    m_device->CreateBuffer(&qbd, &qsd, &m_vinylQuadVB);

    D3D11_BUFFER_DESC ibd{};
    ibd.ByteWidth = sizeof(VinylInstance) * 512;
    ibd.Usage = D3D11_USAGE_DYNAMIC;
    ibd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    ibd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    m_device->CreateBuffer(&ibd, nullptr, &m_vinylInstanceVB);

    cbd.ByteWidth = 16;
    m_device->CreateBuffer(&cbd, nullptr, &m_vinylCB);

    // 3. 3D Line shader
    if (!compileShader(kLineShaderHLSL, "VSMain", "vs_5_0", vsB)) return false;
    if (!compileShader(kLineShaderHLSL, "PSMain", "ps_5_0", psB)) return false;
    m_device->CreateVertexShader(vsB->GetBufferPointer(), vsB->GetBufferSize(), nullptr, &m_lineVS);
    m_device->CreatePixelShader(psB->GetBufferPointer(), psB->GetBufferSize(), nullptr, &m_linePS);

    D3D11_INPUT_ELEMENT_DESC lineElems[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    m_device->CreateInputLayout(lineElems, 2, vsB->GetBufferPointer(), vsB->GetBufferSize(), &m_lineIL);
    cbd.ByteWidth = 64;
    m_device->CreateBuffer(&cbd, nullptr, &m_lineCB);

    // 4. UV Wireframe & Fullscreen Quad shaders
    if (!compileShader(kUVWireShaderHLSL, "VSMain", "vs_5_0", vsB)) return false;
    if (!compileShader(kUVWireShaderHLSL, "PSMain", "ps_5_0", psB)) return false;
    m_device->CreateVertexShader(vsB->GetBufferPointer(), vsB->GetBufferSize(), nullptr, &m_uvWireVS);
    m_device->CreatePixelShader(psB->GetBufferPointer(), psB->GetBufferSize(), nullptr, &m_uvWirePS);

    if (!compileShader(kFSQuadShaderHLSL, "VSMain", "vs_5_0", vsB)) return false;
    if (!compileShader(kFSQuadShaderHLSL, "PSMain", "ps_5_0", psB)) return false;
    m_device->CreateVertexShader(vsB->GetBufferPointer(), vsB->GetBufferSize(), nullptr, &m_fsQuadVS);
    m_device->CreatePixelShader(psB->GetBufferPointer(), psB->GetBufferSize(), nullptr, &m_fsQuadPS);

    return true;
}

bool RendererDX11::createVinylRTs() {
    auto makeRT = [&](int w, int h, ComPtr<ID3D11Texture2D>& tex, ComPtr<ID3D11RenderTargetView>& rtv, ComPtr<ID3D11ShaderResourceView>& srv) {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = (UINT)w;
        td.Height = (UINT)h;
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(m_device->CreateTexture2D(&td, nullptr, &tex))) return false;
        if (FAILED(m_device->CreateRenderTargetView(tex.Get(), nullptr, &rtv))) return false;
        if (FAILED(m_device->CreateShaderResourceView(tex.Get(), nullptr, &srv))) return false;
        return true;
    };
    return makeRT(m_vinylRTSize, m_vinylRTSize, m_bodyRT, m_bodyRTV, m_bodySRV) &&
           makeRT(m_vinylRTSize, m_vinylRTSize, m_winRT, m_winRTV, m_winSRV) &&
           makeRT(1024, 512, m_plateRT, m_plateRTV, m_plateSRV);
}

bool RendererDX11::uploadSpriteCatalog(const SpriteCatalog& catalog) {
    if (catalog.entries.empty()) return false;
    UINT nSlices = (UINT)catalog.entries.size();

    D3D11_TEXTURE2D_DESC td{};
    td.Width = 256;
    td.Height = 256;
    td.MipLevels = 1;
    td.ArraySize = nSlices;
    td.Format = DXGI_FORMAT_R8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    std::vector<D3D11_SUBRESOURCE_DATA> init(nSlices);
    for (UINT i = 0; i < nSlices; ++i) {
        init[i].pSysMem = catalog.entries[i].mask256.data();
        init[i].SysMemPitch = 256;
        init[i].SysMemSlicePitch = 256 * 256;
    }

    m_spriteArrayTex.Reset();
    m_spriteArraySRV.Reset();
    if (FAILED(m_device->CreateTexture2D(&td, init.data(), &m_spriteArrayTex))) return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC srvd{};
    srvd.Format = DXGI_FORMAT_R8_UNORM;
    srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    srvd.Texture2DArray.MipLevels = 1;
    srvd.Texture2DArray.ArraySize = nSlices;
    return SUCCEEDED(m_device->CreateShaderResourceView(m_spriteArrayTex.Get(), &srvd, &m_spriteArraySRV));
}

void RendererDX11::uploadCarModel(const CarModel& car) {
    m_carSubmeshes.clear();
    m_wheelSubmeshes.clear();

    auto uploadList = [&](const std::vector<CarSubmesh>& src, std::vector<GpuSubmesh>& dst) {
        dst.reserve(src.size());
        for (const auto& sm : src) {
            if (sm.vertices.empty() || sm.indices.empty()) continue;
            GpuSubmesh g;
            g.surface = sm.surface;
            g.kitGroupIdx = sm.kitGroupIdx;
            g.kitVariantIdx = sm.kitVariantIdx;
            std::memcpy(g.baseColor, sm.baseColor, sizeof(g.baseColor));
            g.roughness = sm.roughness;
            g.metallic = sm.metallic;
            g.indexCount = (uint32_t)sm.indices.size();

            D3D11_BUFFER_DESC vbd{};
            vbd.ByteWidth = (UINT)(sm.vertices.size() * sizeof(CarVertex));
            vbd.Usage = D3D11_USAGE_IMMUTABLE;
            vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
            D3D11_SUBRESOURCE_DATA vsd{sm.vertices.data(), 0, 0};
            m_device->CreateBuffer(&vbd, &vsd, &g.vb);

            D3D11_BUFFER_DESC ibd{};
            ibd.ByteWidth = (UINT)(sm.indices.size() * sizeof(uint32_t));
            ibd.Usage = D3D11_USAGE_IMMUTABLE;
            ibd.BindFlags = D3D11_BIND_INDEX_BUFFER;
            D3D11_SUBRESOURCE_DATA isd{sm.indices.data(), 0, 0};
            m_device->CreateBuffer(&ibd, &isd, &g.ib);

            dst.push_back(std::move(g));
        }
    };

    uploadList(car.submeshes, m_carSubmeshes);
    uploadList(car.proceduralWheels, m_wheelSubmeshes);
}

static uint32_t decodeUTF8(const std::string& s, size_t& pos) {
    uint8_t c = (uint8_t)s[pos++];
    if (c < 0x80) return c;
    if ((c & 0xE0) == 0xC0 && pos < s.size()) {
        uint32_t r = ((c & 0x1F) << 6) | ((uint8_t)s[pos++] & 0x3F);
        return r;
    }
    if ((c & 0xF0) == 0xE0 && pos + 1 < s.size()) {
        uint32_t r = ((c & 0x0F) << 12) | (((uint8_t)s[pos] & 0x3F) << 6) | ((uint8_t)s[pos + 1] & 0x3F);
        pos += 2;
        return r;
    }
    if ((c & 0xF8) == 0xF0 && pos + 2 < s.size()) {
        uint32_t r = ((c & 0x07) << 18) | (((uint8_t)s[pos] & 0x3F) << 12) | (((uint8_t)s[pos + 1] & 0x3F) << 6) | ((uint8_t)s[pos + 2] & 0x3F);
        pos += 3;
        return r;
    }
    return '?';
}

const RendererDX11::CachedGlyphTex* RendererDX11::getOrCreateTextTex(const std::string& utf8, bool bold) {
    std::string key = (bold ? "B|" : "R|") + utf8;
    auto it = m_textCache.find(key);
    if (it != m_textCache.end()) return &it->second;

    if (m_fontDataPrimary.empty()) return nullptr;
    stbtt_fontinfo fontPrimary{}, fontSym{};
    if (!stbtt_InitFont(&fontPrimary, m_fontDataPrimary.data(), 0)) return nullptr;
    bool hasSym = (!m_fontDataSymbol.empty() && stbtt_InitFont(&fontSym, m_fontDataSymbol.data(), 0));

    std::vector<uint32_t> cps;
    for (size_t p = 0; p < utf8.size();) cps.push_back(decodeUTF8(utf8, p));
    if (cps.empty()) return nullptr;

    const float pxH = 96.0f;
    float scP = stbtt_ScaleForPixelHeight(&fontPrimary, pxH);
    float scS = hasSym ? stbtt_ScaleForPixelHeight(&fontSym, pxH) : scP;

    int totalW = 8;
    for (uint32_t cp : cps) {
        bool useSym = (hasSym && stbtt_FindGlyphIndex(&fontPrimary, (int)cp) == 0 && stbtt_FindGlyphIndex(&fontSym, (int)cp) != 0);
        int adv = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(useSym ? &fontSym : &fontPrimary, (int)cp, &adv, &lsb);
        totalW += (int)std::ceil(adv * (useSym ? scS : scP)) + 2;
    }
    int bmpW = std::clamp(totalW, 16, 1024);
    int bmpH = 120;
    std::vector<uint8_t> bmp((size_t)bmpW * (size_t)bmpH, 0);

    int ascent = 0, descent = 0, lineGap = 0;
    stbtt_GetFontVMetrics(&fontPrimary, &ascent, &descent, &lineGap);
    int baseline = (int)(ascent * scP) + 8;

    int penX = 4;
    for (uint32_t cp : cps) {
        bool useSym = (hasSym && stbtt_FindGlyphIndex(&fontPrimary, (int)cp) == 0 && stbtt_FindGlyphIndex(&fontSym, (int)cp) != 0);
        stbtt_fontinfo* fi = useSym ? &fontSym : &fontPrimary;
        float sc = useSym ? scS : scP;
        int gw = 0, gh = 0, xoff = 0, yoff = 0;
        unsigned char* g = stbtt_GetCodepointBitmap(fi, 0, sc, (int)cp, &gw, &gh, &xoff, &yoff);
        if (g) {
            for (int y = 0; y < gh; ++y) {
                int dy = baseline + yoff + y;
                if (dy < 0 || dy >= bmpH) continue;
                for (int x = 0; x < gw; ++x) {
                    int dx = penX + xoff + x;
                    if (dx < 0 || dx >= bmpW) continue;
                    bmp[(size_t)dy * (size_t)bmpW + (size_t)dx] = std::max(bmp[(size_t)dy * (size_t)bmpW + (size_t)dx], g[y * gw + x]);
                }
            }
            stbtt_FreeBitmap(g, nullptr);
        }
        int adv = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(fi, (int)cp, &adv, &lsb);
        penX += (int)std::ceil(adv * sc);
    }

    CachedGlyphTex ct;
    ct.w = bmpW; ct.h = bmpH;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = (UINT)bmpW; td.Height = (UINT)bmpH;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{bmp.data(), (UINT)bmpW, 0};
    if (FAILED(m_device->CreateTexture2D(&td, &sd, &ct.tex))) return nullptr;
    if (FAILED(m_device->CreateShaderResourceView(ct.tex.Get(), nullptr, &ct.srv))) return nullptr;

    auto res = m_textCache.emplace(key, std::move(ct));
    return &res.first->second;
}

void RendererDX11::compositeLayerList(
    ID3D11RenderTargetView* rtv, int rtW, int rtH,
    const std::vector<VinylLayer>& layers,
    const SpriteCatalog& catalog
) {
    const float clear0[4] = {0, 0, 0, 0};
    m_ctx->ClearRenderTargetView(rtv, clear0);
    if (layers.empty() || !m_spriteArraySRV) return;

    D3D11_VIEWPORT vp{0, 0, (float)rtW, (float)rtH, 0.0f, 1.0f};
    m_ctx->RSSetViewports(1, &vp);
    m_ctx->OMSetRenderTargets(1, &rtv, nullptr);
    m_ctx->RSSetState(m_rsNoCull.Get());
    m_ctx->OMSetDepthStencilState(m_dsOff.Get(), 0);
    const float bf[4] = {0, 0, 0, 0};
    m_ctx->OMSetBlendState(m_bsAlpha.Get(), bf, 0xFFFFFFFF);

    m_ctx->IASetInputLayout(m_vinylIL.Get());
    m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_ctx->VSSetShader(m_vinylVS.Get(), nullptr, 0);
    m_ctx->PSSetShader(m_vinylPS.Get(), nullptr, 0);
    m_ctx->PSSetSamplers(0, 1, m_sampLinearClamp.GetAddressOf());
    m_ctx->PSSetShaderResources(0, 1, m_spriteArraySRV.GetAddressOf());

    ID3D11Buffer* vbs[2] = {m_vinylQuadVB.Get(), m_vinylInstanceVB.Get()};
    UINT strides[2] = {sizeof(QuadVertex), sizeof(VinylInstance)};
    UINT offsets[2] = {0, 0};
    m_ctx->IASetVertexBuffers(0, 2, vbs, strides, offsets);

    auto setMode = [&](float m) {
        D3D11_MAPPED_SUBRESOURCE ms{};
        if (SUCCEEDED(m_ctx->Map(m_vinylCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
            float v[4] = {m, 0, 0, 0};
            std::memcpy(ms.pData, v, sizeof(v));
            m_ctx->Unmap(m_vinylCB.Get(), 0);
        }
        m_ctx->PSSetConstantBuffers(0, 1, m_vinylCB.GetAddressOf());
    };

    // Batch consecutive sprite layers into a single DrawInstanced call; flush before/after text layers!
    std::vector<VinylInstance> batch;
    batch.reserve(layers.size());

    auto flushSprites = [&]() {
        if (batch.empty()) return;
        setMode(0.0f);
        size_t start = 0;
        while (start < batch.size()) {
            size_t cnt = std::min<size_t>(512, batch.size() - start);
            D3D11_MAPPED_SUBRESOURCE ms{};
            if (SUCCEEDED(m_ctx->Map(m_vinylInstanceVB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
                std::memcpy(ms.pData, &batch[start], cnt * sizeof(VinylInstance));
                m_ctx->Unmap(m_vinylInstanceVB.Get(), 0);
                m_ctx->DrawInstanced(6, (UINT)cnt, 0, 0);
            }
            start += cnt;
        }
        batch.clear();
    };

    for (const auto& l : layers) {
        uint8_t r, g, b, a;
        unpackABGR(l.color, r, g, b, a);
        if (l.isText && !l.text.empty()) {
            flushSprites();
            const auto* gt = getOrCreateTextTex(l.text, l.bold != 0);
            if (!gt) continue;
            setMode(1.0f);
            m_ctx->PSSetShaderResources(1, 1, gt->srv.GetAddressOf());

            float aspect = (float)gt->w / (float)std::max(1, gt->h);
            float hUV = std::fabs(l.sy) * 0.025f;
            float wUV = std::fabs(l.sx) * 0.025f * std::max(0.4f, aspect * 0.65f);
            if (l.flipX || l.sx < 0.0f) wUV = -wUV;
            VinylInstance inst{l.px, l.py, wUV, hUV, l.rot * kDeg2Rad, 0.0f, 0, 0, r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f};
            D3D11_MAPPED_SUBRESOURCE ms{};
            if (SUCCEEDED(m_ctx->Map(m_vinylInstanceVB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
                std::memcpy(ms.pData, &inst, sizeof(inst));
                m_ctx->Unmap(m_vinylInstanceVB.Get(), 0);
                m_ctx->DrawInstanced(6, 1, 0, 0);
            }
        } else {
            float nw = 0.0245f, nh = 0.0244f;
            catalog.nativeSizeFor(l.stickerIndex, nw, nh);
            int slice = catalog.sliceForSticker(l.stickerIndex);
            float wUV = l.sx * nw;
            if (l.flipX) wUV = -wUV;
            float hUV = l.sy * nh;
            batch.push_back({l.px, l.py, wUV, hUV, l.rot * kDeg2Rad, (float)slice, 0, 0, r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f});
        }
    }
    flushSprites();

    ID3D11ShaderResourceView* nullSRVs[2] = {nullptr, nullptr};
    m_ctx->PSSetShaderResources(0, 2, nullSRVs);
    m_ctx->OMSetRenderTargets(0, nullptr, nullptr);
}

void RendererDX11::compositeVinyls(const ProjectedLivery& livery, const SpriteCatalog& catalog, const LiveryProject& /*project*/) {
    compositeLayerList(m_bodyRTV.Get(), m_vinylRTSize, m_vinylRTSize, livery.bodyLayers, catalog);
    compositeLayerList(m_winRTV.Get(), m_vinylRTSize, m_vinylRTSize, livery.windowLayers, catalog);
    compositeLayerList(m_plateRTV.Get(), 1024, 512, livery.plateLayers, catalog);
}

void RendererDX11::ensure3DViewport(int w, int h) {
    w = std::max(64, w); h = std::max(64, h);
    if (w == m_vp3DW && h == m_vp3DH && m_vp3DTex) return;
    m_vp3DW = w; m_vp3DH = h;
    m_vp3DTex.Reset(); m_vp3DRTV.Reset(); m_vp3DSRV.Reset();
    m_vp3DDepth.Reset(); m_vp3DDSV.Reset();

    D3D11_TEXTURE2D_DESC td{};
    td.Width = (UINT)w; td.Height = (UINT)h;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    m_device->CreateTexture2D(&td, nullptr, &m_vp3DTex);
    m_device->CreateRenderTargetView(m_vp3DTex.Get(), nullptr, &m_vp3DRTV);
    m_device->CreateShaderResourceView(m_vp3DTex.Get(), nullptr, &m_vp3DSRV);

    td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    m_device->CreateTexture2D(&td, nullptr, &m_vp3DDepth);
    m_device->CreateDepthStencilView(m_vp3DDepth.Get(), nullptr, &m_vp3DDSV);
}

void RendererDX11::ensureUVViewport(int w, int h) {
    w = std::max(64, w); h = std::max(64, h);
    if (w == m_vpUVW && h == m_vpUVH && m_vpUVTex) return;
    m_vpUVW = w; m_vpUVH = h;
    m_vpUVTex.Reset(); m_vpUVRTV.Reset(); m_vpUVSRV.Reset();

    D3D11_TEXTURE2D_DESC td{};
    td.Width = (UINT)w; td.Height = (UINT)h;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    m_device->CreateTexture2D(&td, nullptr, &m_vpUVTex);
    m_device->CreateRenderTargetView(m_vpUVTex.Get(), nullptr, &m_vpUVRTV);
    m_device->CreateShaderResourceView(m_vpUVTex.Get(), nullptr, &m_vpUVSRV);
}

void RendererDX11::drawLines(const std::vector<LineVertex>& verts, const Mat4& vp) {
    if (verts.empty()) return;
    if (verts.size() > m_lineVBCap) {
        m_lineVBCap = (uint32_t)std::max<size_t>(verts.size() * 2, 2048);
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = m_lineVBCap * sizeof(LineVertex);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        m_lineVB.Reset();
        m_device->CreateBuffer(&bd, nullptr, &m_lineVB);
    }

    D3D11_MAPPED_SUBRESOURCE ms{};
    if (SUCCEEDED(m_ctx->Map(m_lineVB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
        std::memcpy(ms.pData, verts.data(), verts.size() * sizeof(LineVertex));
        m_ctx->Unmap(m_lineVB.Get(), 0);
    }
    if (SUCCEEDED(m_ctx->Map(m_lineCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
        // Mat4 is already column-major, matching HLSL default column_major float4x4
        std::memcpy(ms.pData, vp.m, 64);
        m_ctx->Unmap(m_lineCB.Get(), 0);
    }

    m_ctx->IASetInputLayout(m_lineIL.Get());
    m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
    UINT stride = sizeof(LineVertex), off = 0;
    m_ctx->IASetVertexBuffers(0, 1, m_lineVB.GetAddressOf(), &stride, &off);
    m_ctx->VSSetShader(m_lineVS.Get(), nullptr, 0);
    m_ctx->PSSetShader(m_linePS.Get(), nullptr, 0);
    m_ctx->VSSetConstantBuffers(0, 1, m_lineCB.GetAddressOf());
    m_ctx->Draw((UINT)verts.size(), 0);
}

void RendererDX11::render3DViewport(
    int width, int height,
    const OrbitCamera& cam,
    const CarModel& car,
    const LiveryProject& project,
    int selectedDecalIdx,
    const ViewportOptions& opts
) {
    ensure3DViewport(width, height);
    const float bg[4] = {0.10f, 0.11f, 0.14f, 1.0f};
    m_ctx->ClearRenderTargetView(m_vp3DRTV.Get(), bg);
    m_ctx->ClearDepthStencilView(m_vp3DDSV.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);

    D3D11_VIEWPORT vp{0, 0, (float)m_vp3DW, (float)m_vp3DH, 0.0f, 1.0f};
    m_ctx->RSSetViewports(1, &vp);
    m_ctx->OMSetRenderTargets(1, m_vp3DRTV.GetAddressOf(), m_vp3DDSV.Get());

    float aspect = (float)m_vp3DW / (float)std::max(1, m_vp3DH);
    Mat4 vpMat = cam.projMatrix(aspect) * cam.viewMatrix();
    Vec3 eye = cam.eyePosition();
    const float bf[4] = {0, 0, 0, 0};

    // 1. Floor grid lines
    if (opts.showFloorGrid) {
        m_ctx->OMSetDepthStencilState(m_dsDefault.Get(), 0);
        m_ctx->OMSetBlendState(m_bsAlpha.Get(), bf, 0xFFFFFFFF);
        std::vector<LineVertex> grid;
        float floorY = car.bounds.bmin.y - 0.16f;
        for (int i = -6; i <= 6; ++i) {
            float c = (i == 0) ? 0.36f : 0.20f;
            float a = (i == 0) ? 0.70f : 0.40f;
            grid.push_back({(float)i * 0.5f, floorY, -3.0f, c, c, c + 0.05f, a});
            grid.push_back({(float)i * 0.5f, floorY, +3.0f, c, c, c + 0.05f, a});
            grid.push_back({-3.0f, floorY, (float)i * 0.5f, c, c, c + 0.05f, a});
            grid.push_back({+3.0f, floorY, (float)i * 0.5f, c, c, c + 0.05f, a});
        }
        drawLines(grid, vpMat);
    }

    // 2. Render car meshes
    m_ctx->IASetInputLayout(m_carIL.Get());
    m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_ctx->VSSetShader(m_carVS.Get(), nullptr, 0);
    m_ctx->PSSetShader(m_carPS.Get(), nullptr, 0);
    m_ctx->PSSetSamplers(0, 1, m_sampLinearClamp.GetAddressOf());

    auto drawSubmesh = [&](const GpuSubmesh& g) {
        CarCBData cb{};
        // Mat4 is already column-major, matching HLSL default column_major float4x4
        std::memcpy(cb.viewProj, vpMat.m, 64);
        cb.camPos[0] = eye.x; cb.camPos[1] = eye.y; cb.camPos[2] = eye.z; cb.camPos[3] = 1.0f;
        if (g.surface == SURF_BODY) {
            cb.baseColor[0] = project.paintColor[0];
            cb.baseColor[1] = project.paintColor[1];
            cb.baseColor[2] = project.paintColor[2];
            cb.baseColor[3] = 1.0f;
            cb.params[0] = project.paintRoughness;
            cb.params[1] = project.paintMetallic;
            m_ctx->PSSetShaderResources(0, 1, m_bodySRV.GetAddressOf());
        } else if (g.surface == SURF_WINDOWS) {
            std::memcpy(cb.baseColor, project.windowTint, 16);
            cb.params[0] = 0.06f;
            cb.params[1] = 0.10f;
            m_ctx->PSSetShaderResources(0, 1, m_winSRV.GetAddressOf());
        } else if (g.surface == SURF_PLATE) {
            cb.baseColor[0] = 0.94f; cb.baseColor[1] = 0.94f; cb.baseColor[2] = 0.95f; cb.baseColor[3] = 1.0f;
            cb.params[0] = 0.25f;
            cb.params[1] = 0.15f;
            m_ctx->PSSetShaderResources(0, 1, m_plateSRV.GetAddressOf());
        } else {
            std::memcpy(cb.baseColor, g.baseColor, 16);
            cb.params[0] = g.roughness;
            cb.params[1] = g.metallic;
            m_ctx->PSSetShaderResources(0, 1, m_bodySRV.GetAddressOf());
        }
        cb.params[2] = (float)g.surface;
        cb.params[3] = opts.showUVHeatmap ? 1.0f : 0.0f;

        D3D11_MAPPED_SUBRESOURCE ms{};
        if (SUCCEEDED(m_ctx->Map(m_carCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
            std::memcpy(ms.pData, &cb, sizeof(cb));
            m_ctx->Unmap(m_carCB.Get(), 0);
        }
        m_ctx->VSSetConstantBuffers(0, 1, m_carCB.GetAddressOf());
        m_ctx->PSSetConstantBuffers(0, 1, m_carCB.GetAddressOf());

        UINT stride = sizeof(CarVertex), off = 0;
        m_ctx->IASetVertexBuffers(0, 1, g.vb.GetAddressOf(), &stride, &off);
        m_ctx->IASetIndexBuffer(g.ib.Get(), DXGI_FORMAT_R32_UINT, 0);
        m_ctx->DrawIndexed(g.indexCount, 0, 0);
    };

    auto isActive = [&](const GpuSubmesh& g) {
        if (g.kitGroupIdx < 0) return true;
        if ((size_t)g.kitGroupIdx >= project.kitVariants.size()) return g.kitVariantIdx == 0;
        return g.kitVariantIdx == project.kitVariants[(size_t)g.kitGroupIdx];
    };

    // Pass A: Opaque (STATIC + WHEELS + BODY + PLATE)
    m_ctx->RSSetState(opts.showWireframe ? m_rsWire.Get() : m_rsNoCull.Get());
    m_ctx->OMSetDepthStencilState(m_dsDefault.Get(), 0);
    m_ctx->OMSetBlendState(m_bsOpaque.Get(), bf, 0xFFFFFFFF);

    if (opts.showWheels) {
        for (const auto& g : m_wheelSubmeshes) drawSubmesh(g);
    }
    for (const auto& g : m_carSubmeshes) {
        if (!isActive(g)) continue;
        if (g.surface == SURF_WINDOWS) continue;
        if (g.surface == SURF_STATIC && !opts.showInteriorStatic) continue;
        drawSubmesh(g);
    }

    // Pass B: Transparent Windows (with WindowVinyls_RT composited!)
    if (opts.showWindows) {
        m_ctx->OMSetDepthStencilState(m_dsReadOnly.Get(), 0);
        m_ctx->OMSetBlendState(m_bsAlpha.Get(), bf, 0xFFFFFFFF);
        for (const auto& g : m_carSubmeshes) {
            if (!isActive(g) || g.surface != SURF_WINDOWS) continue;
            drawSubmesh(g);
        }
    }

    // Pass C: 3D Decal Projection Boxes & Multi-Tile Grid lines
    if (opts.showDecalBoxes && !project.decals.empty()) {
        m_ctx->RSSetState(m_rsNoCull.Get());
        m_ctx->OMSetDepthStencilState(m_dsReadOnly.Get(), 0);
        m_ctx->OMSetBlendState(m_bsAlpha.Get(), bf, 0xFFFFFFFF);

        std::vector<LineVertex> lines;
        for (int i = 0; i < (int)project.decals.size(); ++i) {
            const auto& d = project.decals[(size_t)i];
            if (!d.enabled || d.xform.projectionMode != 0) continue;
            bool sel = (i == selectedDecalIdx);
            float cr = sel ? 0.15f : 0.55f, cg = sel ? 0.88f : 0.65f, cb = sel ? 1.0f : 0.80f, ca = sel ? 0.95f : 0.35f;

            Mat4 m = d.xform.matrix();
            auto pt = [&](float x, float y, float z) { return m.transformPoint({x, y, z}); };
            auto addEdge = [&](const Vec3& a, const Vec3& b, float r, float g, float bl, float al) {
                lines.push_back({a.x, a.y, a.z, r, g, bl, al});
                lines.push_back({b.x, b.y, b.z, r, g, bl, al});
            };

            // Front decal frame (z = +0.5) & projection depth back frame (z = -0.5)
            Vec3 f0 = pt(-0.5f, -0.5f, +0.5f), f1 = pt(+0.5f, -0.5f, +0.5f), f2 = pt(+0.5f, +0.5f, +0.5f), f3 = pt(-0.5f, +0.5f, +0.5f);
            Vec3 b0 = pt(-0.5f, -0.5f, -0.5f), b1 = pt(+0.5f, -0.5f, -0.5f), b2 = pt(+0.5f, +0.5f, -0.5f), b3 = pt(-0.5f, +0.5f, -0.5f);
            addEdge(f0, f1, cr, cg, cb, ca); addEdge(f1, f2, cr, cg, cb, ca);
            addEdge(f2, f3, cr, cg, cb, ca); addEdge(f3, f0, cr, cg, cb, ca);
            addEdge(b0, b1, cr, cg, cb, ca * 0.45f); addEdge(b1, b2, cr, cg, cb, ca * 0.45f);
            addEdge(b2, b3, cr, cg, cb, ca * 0.45f); addEdge(b3, b0, cr, cg, cb, ca * 0.45f);
            addEdge(f0, b0, cr, cg, cb, ca * 0.5f); addEdge(f1, b1, cr, cg, cb, ca * 0.5f);
            addEdge(f2, b2, cr, cg, cb, ca * 0.5f); addEdge(f3, b3, cr, cg, cb, ca * 0.5f);

            // Projection direction arrow (-Z)
            Vec3 cF = pt(0, 0, +0.5f), cB = pt(0, 0, -0.25f);
            addEdge(cF, cB, 1.0f, 0.85f, 0.2f, ca);

            // Multi-Tile Tiling Grid lines on the decal front face!
            if (sel && opts.showTileGrid && (d.tiling.cols > 1 || d.tiling.rows > 1)) {
                for (int c = 1; c < d.tiling.cols; ++c) {
                    float u = -0.5f + (float)c / (float)d.tiling.cols;
                    addEdge(pt(u, -0.5f, +0.5f), pt(u, +0.5f, +0.5f), 1.0f, 0.60f, 0.15f, 0.90f);
                }
                for (int r = 1; r < d.tiling.rows; ++r) {
                    float v = -0.5f + (float)r / (float)d.tiling.rows;
                    addEdge(pt(-0.5f, v, +0.5f), pt(+0.5f, v, +0.5f), 1.0f, 0.60f, 0.15f, 0.90f);
                }
            }
        }
        drawLines(lines, vpMat);
    }

    ID3D11ShaderResourceView* nullSRV = nullptr;
    m_ctx->PSSetShaderResources(0, 1, &nullSRV);
    m_ctx->OMSetRenderTargets(0, nullptr, nullptr);
}

void RendererDX11::renderUVViewport(
    int width, int height,
    const CarModel& /*car*/,
    const LiveryProject& project,
    const ViewportOptions& opts
) {
    ensureUVViewport(width, height);
    const float bg[4] = {0.07f, 0.08f, 0.10f, 1.0f};
    m_ctx->ClearRenderTargetView(m_vpUVRTV.Get(), bg);

    D3D11_VIEWPORT vp{0, 0, (float)m_vpUVW, (float)m_vpUVH, 0.0f, 1.0f};
    m_ctx->RSSetViewports(1, &vp);
    m_ctx->OMSetRenderTargets(1, m_vpUVRTV.GetAddressOf(), nullptr);
    m_ctx->RSSetState(m_rsNoCull.Get());
    m_ctx->OMSetDepthStencilState(m_dsOff.Get(), 0);
    const float bf[4] = {0, 0, 0, 0};
    m_ctx->OMSetBlendState(m_bsOpaque.Get(), bf, 0xFFFFFFFF);

    // 1. Draw chosen surface RenderTexture + checkerboard + 0.1 grid
    ID3D11ShaderResourceView* srv = (opts.uvViewSurface == SURF_WINDOWS) ? m_winSRV.Get()
                                  : (opts.uvViewSurface == SURF_PLATE) ? m_plateSRV.Get()
                                  : m_bodySRV.Get();
    m_ctx->IASetInputLayout(nullptr);
    m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_ctx->VSSetShader(m_fsQuadVS.Get(), nullptr, 0);
    m_ctx->PSSetShader(m_fsQuadPS.Get(), nullptr, 0);
    m_ctx->PSSetSamplers(0, 1, m_sampLinearClamp.GetAddressOf());
    m_ctx->PSSetShaderResources(0, 1, &srv);
    m_ctx->Draw(3, 0);

    // 2. Overlay active mesh UV wireframe
    if (opts.uvShowWireframe) {
        m_ctx->RSSetState(m_rsWire.Get());
        m_ctx->OMSetBlendState(m_bsAlpha.Get(), bf, 0xFFFFFFFF);
        m_ctx->IASetInputLayout(m_carIL.Get());
        m_ctx->VSSetShader(m_uvWireVS.Get(), nullptr, 0);
        m_ctx->PSSetShader(m_uvWirePS.Get(), nullptr, 0);

        CarCBData cb{};
        cb.baseColor[0] = 0.25f; cb.baseColor[1] = 0.85f; cb.baseColor[2] = 1.0f; cb.baseColor[3] = 0.45f;
        if (opts.uvViewSurface == SURF_WINDOWS) { cb.baseColor[0] = 1.0f; cb.baseColor[1] = 0.72f; cb.baseColor[2] = 0.25f; }
        D3D11_MAPPED_SUBRESOURCE ms{};
        if (SUCCEEDED(m_ctx->Map(m_carCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
            std::memcpy(ms.pData, &cb, sizeof(cb));
            m_ctx->Unmap(m_carCB.Get(), 0);
        }
        m_ctx->VSSetConstantBuffers(0, 1, m_carCB.GetAddressOf());

        for (const auto& g : m_carSubmeshes) {
            if ((int)g.surface != opts.uvViewSurface) continue;
            if (g.kitGroupIdx >= 0) {
                int sel = ((size_t)g.kitGroupIdx < project.kitVariants.size()) ? project.kitVariants[(size_t)g.kitGroupIdx] : 0;
                if (g.kitVariantIdx != sel) continue;
            }
            UINT stride = sizeof(CarVertex), off = 0;
            m_ctx->IASetVertexBuffers(0, 1, g.vb.GetAddressOf(), &stride, &off);
            m_ctx->IASetIndexBuffer(g.ib.Get(), DXGI_FORMAT_R32_UINT, 0);
            m_ctx->DrawIndexed(g.indexCount, 0, 0);
        }
    }

    ID3D11ShaderResourceView* nullSRV = nullptr;
    m_ctx->PSSetShaderResources(0, 1, &nullSRV);
    m_ctx->OMSetRenderTargets(0, nullptr, nullptr);
}

bool RendererDX11::updatePreviewTexture(const std::string& key, const uint8_t* rgba, int w, int h) {
    if (!rgba || w <= 0 || h <= 0) return false;
    auto& pt = m_previewTexs[key];
    pt.tex.Reset(); pt.srv.Reset();
    pt.w = w; pt.h = h;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = (UINT)w; td.Height = (UINT)h;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd{rgba, (UINT)(w * 4), 0};
    if (FAILED(m_device->CreateTexture2D(&td, &sd, &pt.tex))) return false;
    return SUCCEEDED(m_device->CreateShaderResourceView(pt.tex.Get(), nullptr, &pt.srv));
}

ID3D11ShaderResourceView* RendererDX11::getPreviewSRV(const std::string& key) const {
    auto it = m_previewTexs.find(key);
    return (it != m_previewTexs.end()) ? it->second.srv.Get() : nullptr;
}

void RendererDX11::beginFrame(const float clearRGBA[4]) {
    m_ctx->OMSetRenderTargets(1, m_mainRTV.GetAddressOf(), nullptr);
    m_ctx->ClearRenderTargetView(m_mainRTV.Get(), clearRGBA);
}

void RendererDX11::endFrame(bool vsync) {
    m_swapChain->Present(vsync ? 1 : 0, 0);
}

bool RendererDX11::saveBackbufferPNG(const std::string& pngPath) {
    ComPtr<ID3D11Texture2D> backBuf;
    if (FAILED(m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuf)))) return false;
    D3D11_TEXTURE2D_DESC td{};
    backBuf->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(m_device->CreateTexture2D(&td, nullptr, &staging))) return false;
    m_ctx->CopyResource(staging.Get(), backBuf.Get());
    D3D11_MAPPED_SUBRESOURCE ms{};
    if (FAILED(m_ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &ms))) return false;
    std::vector<uint8_t> pixels((size_t)td.Width * (size_t)td.Height * 4u);
    for (UINT y = 0; y < td.Height; ++y) {
        std::memcpy(&pixels[(size_t)y * td.Width * 4u], (const uint8_t*)ms.pData + (size_t)y * ms.RowPitch, td.Width * 4u);
    }
    m_ctx->Unmap(staging.Get(), 0);
    return stbi_write_png(pngPath.c_str(), (int)td.Width, (int)td.Height, 4, pixels.data(), (int)(td.Width * 4u)) != 0;
}

} // namespace cpm
