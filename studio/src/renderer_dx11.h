#pragma once
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include "car_model.h"
#include "math_types.h"
#include "vinyl_types.h"

#include <string>
#include <unordered_map>
#include <vector>

namespace cpm {

using Microsoft::WRL::ComPtr;

struct OrbitCamera {
    Vec3 target{0.0f, 0.62f, 0.0f};
    float yaw = 0.65f;      // radians around Y
    float pitch = 0.30f;    // radians up/down
    float distance = 4.6f;
    float fovDeg = 42.0f;

    Vec3 eyePosition() const {
        float cp = std::cos(pitch), sp = std::sin(pitch);
        float cy = std::cos(yaw), sy = std::sin(yaw);
        return target + Vec3(sy * cp, sp, cy * cp) * distance;
    }

    Mat4 viewMatrix() const {
        return Mat4::LookAtLH(eyePosition(), target, Vec3(0, 1, 0));
    }

    Mat4 projMatrix(float aspect) const {
        return Mat4::PerspectiveLH(fovDeg * kDeg2Rad, aspect, 0.05f, 80.0f);
    }

    Ray screenPointToRay(float nx, float ny, float aspect) const {
        // nx, ny in [-1, +1] NDC (+Y up)
        Mat4 invVP = (projMatrix(aspect) * viewMatrix()).inverse();
        Vec4 pNear = invVP.mulVec4({nx, ny, 0.0f, 1.0f});
        Vec4 pFar  = invVP.mulVec4({nx, ny, 1.0f, 1.0f});
        Vec3 nearW = {pNear.x / pNear.w, pNear.y / pNear.w, pNear.z / pNear.w};
        Vec3 farW  = {pFar.x / pFar.w, pFar.y / pFar.w, pFar.z / pFar.w};
        return {nearW, (farW - nearW).normalized()};
    }
};

struct ViewportOptions {
    bool showUVHeatmap = false;
    bool showWireframe = false;
    bool showWindows = true;
    bool showInteriorStatic = true;
    bool showWheels = true;
    bool showFloorGrid = true;
    bool showDecalBoxes = true;
    bool showTileGrid = true;
    int uvViewSurface = SURF_BODY; // SURF_BODY / SURF_WINDOWS / SURF_PLATE
    bool uvShowWireframe = true;
};

struct GpuSubmesh {
    ComPtr<ID3D11Buffer> vb;
    ComPtr<ID3D11Buffer> ib;
    uint32_t indexCount = 0;
    SurfaceType surface = SURF_STATIC;
    int32_t kitGroupIdx = -1;
    int32_t kitVariantIdx = -1;
    float baseColor[4] = {};
    float roughness = 0.5f;
    float metallic = 0.1f;
};

struct LineVertex {
    float x, y, z;
    float r, g, b, a;
};

class RendererDX11 {
public:
    bool init(HWND hwnd, int width, int height);
    void shutdown();
    void resizeSwapChain(int width, int height);

    ID3D11Device* device() const { return m_device.Get(); }
    ID3D11DeviceContext* context() const { return m_ctx.Get(); }

    // Upload 20 high-res 256x256 sprites into GPU Texture2DArray + initialize system Unicode font
    bool uploadSpriteCatalog(const SpriteCatalog& catalog);

    // Upload loaded CarModel meshes into GPU vertex/index buffers
    void uploadCarModel(const CarModel& car);

    // Composite Body/Window/Plate vinyl layers onto offscreen GPU RenderTextures (< 0.3ms)
    void compositeVinyls(const ProjectedLivery& livery, const SpriteCatalog& catalog, const LiveryProject& project);

    // Render 3D viewport into offscreen SRV for ImGui::Image
    void render3DViewport(
        int width, int height,
        const OrbitCamera& cam,
        const CarModel& car,
        const LiveryProject& project,
        int selectedDecalIdx,
        const ViewportOptions& opts
    );

    // Render 2D UV / Planar Projection view (vinyl RT + active UV wireframe) into offscreen SRV
    void renderUVViewport(
        int width, int height,
        const CarModel& car,
        const LiveryProject& project,
        const ViewportOptions& opts
    );

    // Create/update an RGBA texture SRV (for decal original / preprocessed previews in Inspector)
    bool updatePreviewTexture(const std::string& key, const uint8_t* rgba, int w, int h);
    ID3D11ShaderResourceView* getPreviewSRV(const std::string& key) const;

    ID3D11ShaderResourceView* viewport3DSRV() const { return m_vp3DSRV.Get(); }
    ID3D11ShaderResourceView* viewportUVSRV() const { return m_vpUVSRV.Get(); }

    void beginFrame(const float clearRGBA[4]);
    void endFrame(bool vsync = true);
    bool saveBackbufferPNG(const std::string& pngPath);

private:
    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_ctx;
    ComPtr<IDXGISwapChain> m_swapChain;
    ComPtr<ID3D11RenderTargetView> m_mainRTV;

    // States
    ComPtr<ID3D11RasterizerState> m_rsSolid;
    ComPtr<ID3D11RasterizerState> m_rsNoCull;
    ComPtr<ID3D11RasterizerState> m_rsWire;
    ComPtr<ID3D11DepthStencilState> m_dsDefault;
    ComPtr<ID3D11DepthStencilState> m_dsReadOnly;
    ComPtr<ID3D11DepthStencilState> m_dsOff;
    ComPtr<ID3D11BlendState> m_bsOpaque;
    ComPtr<ID3D11BlendState> m_bsAlpha;
    ComPtr<ID3D11SamplerState> m_sampLinearClamp;

    // Shaders & layouts
    ComPtr<ID3D11VertexShader> m_carVS;
    ComPtr<ID3D11PixelShader> m_carPS;
    ComPtr<ID3D11InputLayout> m_carIL;
    ComPtr<ID3D11Buffer> m_carCB;

    ComPtr<ID3D11VertexShader> m_vinylVS;
    ComPtr<ID3D11PixelShader> m_vinylPS;
    ComPtr<ID3D11InputLayout> m_vinylIL;
    ComPtr<ID3D11Buffer> m_vinylInstanceVB;
    ComPtr<ID3D11Buffer> m_vinylQuadVB;
    ComPtr<ID3D11Buffer> m_vinylCB;

    ComPtr<ID3D11VertexShader> m_lineVS;
    ComPtr<ID3D11PixelShader> m_linePS;
    ComPtr<ID3D11InputLayout> m_lineIL;
    ComPtr<ID3D11Buffer> m_lineCB;
    ComPtr<ID3D11Buffer> m_lineVB;
    uint32_t m_lineVBCap = 0;

    ComPtr<ID3D11VertexShader> m_uvWireVS;
    ComPtr<ID3D11PixelShader> m_uvWirePS;
    ComPtr<ID3D11VertexShader> m_fsQuadVS;
    ComPtr<ID3D11PixelShader> m_fsQuadPS;

    // Sprite Texture2DArray (20 x 256 x 256 R8)
    ComPtr<ID3D11Texture2D> m_spriteArrayTex;
    ComPtr<ID3D11ShaderResourceView> m_spriteArraySRV;

    // Text glyph texture cache (rendered on demand with stb_truetype)
    struct CachedGlyphTex {
        ComPtr<ID3D11Texture2D> tex;
        ComPtr<ID3D11ShaderResourceView> srv;
        int w = 1, h = 1;
    };
    std::vector<uint8_t> m_fontDataPrimary;
    std::vector<uint8_t> m_fontDataSymbol;
    std::unordered_map<std::string, CachedGlyphTex> m_textCache;
    const CachedGlyphTex* getOrCreateTextTex(const std::string& utf8, bool bold);

    // Offscreen Vinyl RenderTextures
    ComPtr<ID3D11Texture2D> m_bodyRT, m_winRT, m_plateRT;
    ComPtr<ID3D11RenderTargetView> m_bodyRTV, m_winRTV, m_plateRTV;
    ComPtr<ID3D11ShaderResourceView> m_bodySRV, m_winSRV, m_plateSRV;
    int m_vinylRTSize = 2048;

    // 3D Viewport RT
    int m_vp3DW = 0, m_vp3DH = 0;
    ComPtr<ID3D11Texture2D> m_vp3DTex, m_vp3DDepth;
    ComPtr<ID3D11RenderTargetView> m_vp3DRTV;
    ComPtr<ID3D11DepthStencilView> m_vp3DDSV;
    ComPtr<ID3D11ShaderResourceView> m_vp3DSRV;

    // 2D UV Viewport RT
    int m_vpUVW = 0, m_vpUVH = 0;
    ComPtr<ID3D11Texture2D> m_vpUVTex;
    ComPtr<ID3D11RenderTargetView> m_vpUVRTV;
    ComPtr<ID3D11ShaderResourceView> m_vpUVSRV;

    // Uploaded car GPU meshes
    std::vector<GpuSubmesh> m_carSubmeshes;
    std::vector<GpuSubmesh> m_wheelSubmeshes;

    // Preview textures for ImGui
    struct PreviewTex {
        ComPtr<ID3D11Texture2D> tex;
        ComPtr<ID3D11ShaderResourceView> srv;
        int w = 0, h = 0;
    };
    std::unordered_map<std::string, PreviewTex> m_previewTexs;

    bool createMainRTV();
    bool createShaders();
    bool createVinylRTs();
    void ensure3DViewport(int w, int h);
    void ensureUVViewport(int w, int h);
    void compositeLayerList(
        ID3D11RenderTargetView* rtv, int rtW, int rtH,
        const std::vector<VinylLayer>& layers,
        const SpriteCatalog& catalog
    );
    void drawLines(const std::vector<LineVertex>& verts, const Mat4& vp);
};

} // namespace cpm
