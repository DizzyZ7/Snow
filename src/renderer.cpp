#include "renderer.hpp"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <dcomp.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iterator>
#include <limits>
#include <new>
#include <utility>

namespace snow {
namespace {
using Microsoft::WRL::ComPtr;

// Accept an 8K UHD monitor while rejecting accidental virtual-desktop/gap
// allocations or hostile extents. The app also bounds its monitor count.
constexpr std::uint64_t MaxTargetPixels = 32ULL * 1024ULL * 1024ULL;
constexpr int MaxTargetDimension = 16384;

bool dimensions(Rect monitor, int& width, int& height) noexcept {
    const auto w = static_cast<std::int64_t>(monitor.right) - monitor.left;
    const auto h = static_cast<std::int64_t>(monitor.bottom) - monitor.top;
    if (w <= 0 || h <= 0 || w > MaxTargetDimension || h > MaxTargetDimension ||
        static_cast<std::uint64_t>(w) * static_cast<std::uint64_t>(h) > MaxTargetPixels)
        return false;
    width = static_cast<int>(w);
    height = static_cast<int>(h);
    return true;
}

enum class Primitive : unsigned char { Rectangle = 0, Ellipse = 1, Crystal = 2 };

struct PreparedItem {
    double left{}, top{}, right{}, bottom{};
    double clippedLeft{}, clippedTop{}, clippedRight{}, clippedBottom{};
    float r{}, g{}, b{}, a{};
    Primitive primitive{Primitive::Rectangle};
};

bool prepare(const DrawItem& item, Rect monitor, PreparedItem& out) noexcept {
    if (!std::isfinite(item.x) || !std::isfinite(item.y) ||
        !std::isfinite(item.w) || !std::isfinite(item.h) ||
        !std::isfinite(item.r) || !std::isfinite(item.g) ||
        !std::isfinite(item.b) || !std::isfinite(item.a) ||
        !std::isfinite(item.shape) || item.w <= 0 || item.h <= 0 || item.a <= 0)
        return false;
    if (item.shape == 0.0F) out.primitive = Primitive::Rectangle;
    else if (item.shape == 1.0F) out.primitive = Primitive::Ellipse;
    else if (item.shape == 2.0F) out.primitive = Primitive::Crystal;
    else return false;
    out.left = static_cast<double>(item.x) - monitor.left;
    out.top = static_cast<double>(item.y) - monitor.top;
    out.right = out.left + static_cast<double>(item.w);
    out.bottom = out.top + static_cast<double>(item.h);
    out.clippedLeft = (std::max)(out.left, 0.0);
    out.clippedTop = (std::max)(out.top, 0.0);
    const auto width = static_cast<double>(static_cast<std::int64_t>(monitor.right) - monitor.left);
    const auto height = static_cast<double>(static_cast<std::int64_t>(monitor.bottom) - monitor.top);
    out.clippedRight = (std::min)(out.right, width);
    out.clippedBottom = (std::min)(out.bottom, height);
    if (out.clippedRight <= out.clippedLeft || out.clippedBottom <= out.clippedTop) return false;
    out.r = std::clamp(item.r, 0.0F, 1.0F);
    out.g = std::clamp(item.g, 0.0F, 1.0F);
    out.b = std::clamp(item.b, 0.0F, 1.0F);
    out.a = std::clamp(item.a, 0.0F, 1.0F);
    return true;
}

double branchDistance(double along, double across, double start, double end, double reach) noexcept {
    const double length = end - start;
    const double projection = std::clamp(((along - start) * length + across * reach) /
                                        (length * length + reach * reach), 0.0, 1.0);
    const double dy = along - start - projection * length;
    const double dx = across - projection * reach;
    return std::sqrt(dx * dx + dy * dy);
}

// Original procedural crystal: six fine spines, paired branch tiers and a
// faceted central hub. Fold into the nearest arm's 60-degree sector using dot
// products; no trigonometry, lookup image or per-pixel allocation is needed.
// Coordinates and stroke distances are normalized to the quad's half-extents.
double crystalDistance(double x, double y) noexcept {
    constexpr double cos30 = 0.8660254037844386;
    x = std::abs(x);
    y = std::abs(y);
    const double diagonal = cos30 * x + 0.5 * y;
    const double along = (std::max)(y, diagonal);
    const double across = diagonal > y ? std::abs(0.5 * x - cos30 * y) : x;
    const double tip = (std::max)(along - 0.90, 0.0);
    const double spine = std::sqrt(across * across + tip * tip) - 0.070;
    const double innerBranch = branchDistance(along, across, 0.38, 0.59, 0.18) - 0.045;
    const double outerBranch = branchDistance(along, across, 0.63, 0.80, 0.13) - 0.032;
    const double hub = along - 0.12;
    return (std::min)((std::min)(spine, hub), (std::min)(innerBranch, outerBranch));
}

struct GdiSurface {
    HDC dc{};
    HBITMAP bitmap{};
    HGDIOBJ previous{};
    std::uint32_t* pixels{};
    std::uint64_t bytes{};
    int width{}, height{};

    GdiSurface() = default;
    GdiSurface(const GdiSurface&) = delete;
    GdiSurface& operator=(const GdiSurface&) = delete;
    ~GdiSurface() { reset(); }

    void reset() noexcept {
        if (dc != nullptr && previous != nullptr) SelectObject(dc, previous);
        if (bitmap != nullptr) DeleteObject(bitmap);
        if (dc != nullptr) DeleteDC(dc);
        dc = nullptr;
        bitmap = nullptr;
        previous = nullptr;
        pixels = nullptr;
        bytes = 0;
        width = 0;
        height = 0;
    }

    bool create(int targetWidth, int targetHeight) noexcept {
        reset();
        dc = CreateCompatibleDC(nullptr);
        if (dc == nullptr) return false;
        if (SetLayout(dc, 0) == GDI_ERROR) { reset(); return false; }
        BITMAPINFO info{};
        info.bmiHeader.biSize = static_cast<DWORD>(sizeof(BITMAPINFOHEADER));
        info.bmiHeader.biWidth = targetWidth;
        info.bmiHeader.biHeight = -targetHeight;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (bitmap == nullptr || bits == nullptr) { reset(); return false; }
        const HGDIOBJ selected = SelectObject(dc, bitmap);
        if (selected == nullptr || selected == HGDI_ERROR) { reset(); return false; }
        previous = selected;
        pixels = static_cast<std::uint32_t*>(bits);
        width = targetWidth;
        height = targetHeight;
        bytes = static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height) * 4ULL;
        std::memset(pixels, 0, static_cast<std::size_t>(bytes));
        return true;
    }
};

void blendPixel(std::uint32_t& destination, const PreparedItem& item, double coverage) noexcept {
    const auto alpha = static_cast<std::uint32_t>(
        std::clamp(static_cast<double>(item.a) * coverage, 0.0, 1.0) * 255.0 + 0.5);
    if (alpha == 0) return;
    const std::uint32_t inverse = 255U - alpha;
    const auto channel = [alpha, inverse](float source, std::uint32_t dest) noexcept {
        const auto premultiplied = static_cast<std::uint32_t>(
            static_cast<double>(source) * static_cast<double>(alpha) + 0.5);
        return premultiplied + (dest * inverse + 127U) / 255U;
    };
    const std::uint32_t blue = channel(item.b, destination & 255U);
    const std::uint32_t green = channel(item.g, (destination >> 8U) & 255U);
    const std::uint32_t red = channel(item.r, (destination >> 16U) & 255U);
    const std::uint32_t finalAlpha = alpha + (((destination >> 24U) * inverse + 127U) / 255U);
    destination = blue | (green << 8U) | (red << 16U) | (finalAlpha << 24U);
}

void rasterize(GdiSurface& target, const PreparedItem& item) noexcept {
    const int left = static_cast<int>(std::floor(item.clippedLeft));
    const int top = static_cast<int>(std::floor(item.clippedTop));
    const int right = static_cast<int>(std::ceil(item.clippedRight));
    const int bottom = static_cast<int>(std::ceil(item.clippedBottom));
    const double radiusX = (item.right - item.left) * 0.5;
    const double radiusY = (item.bottom - item.top) * 0.5;
    const double centerX = item.left + radiusX;
    const double centerY = item.top + radiusY;
    const double crystalPixelScale = (std::min)(radiusX, radiusY);
    for (int y = top; y < bottom; ++y) {
        auto* row = target.pixels + static_cast<std::size_t>(y) * static_cast<std::size_t>(target.width);
        const double vertical = (std::min)(static_cast<double>(y + 1), item.bottom) -
                                (std::max)(static_cast<double>(y), item.top);
        for (int x = left; x < right; ++x) {
            const double horizontal = (std::min)(static_cast<double>(x + 1), item.right) -
                                      (std::max)(static_cast<double>(x), item.left);
            double coverage = horizontal * vertical;
            if (item.primitive == Primitive::Ellipse) {
                const double qx = (static_cast<double>(x) + 0.5 - centerX) / radiusX;
                const double qy = (static_cast<double>(y) + 0.5 - centerY) / radiusY;
                const double distance = std::sqrt(qx * qx + qy * qy);
                const double gradient = std::sqrt((qx / radiusX) * (qx / radiusX) +
                                                  (qy / radiusY) * (qy / radiusY));
                const double ellipseCoverage = gradient > 0
                    ? std::clamp((1.0 - distance) * distance / gradient + 0.5, 0.0, 1.0)
                    : 1.0;
                coverage = (std::min)(coverage, ellipseCoverage);
            } else if (item.primitive == Primitive::Crystal) {
                const double qx = (static_cast<double>(x) + 0.5 - centerX) / radiusX;
                const double qy = (static_cast<double>(y) + 0.5 - centerY) / radiusY;
                const double crystalCoverage = std::clamp(
                    0.5 - crystalDistance(qx, qy) * crystalPixelScale, 0.0, 1.0);
                coverage = (std::min)(coverage, crystalCoverage);
            }
            blendPixel(row[x], item, coverage);
        }
    }
}

struct Instance {
    std::array<float, 4> rect{};
    std::array<float, 4> uv{};
    std::array<float, 4> color{};
    float shape{};
    std::array<float, 3> padding{};
};
static_assert(sizeof(Instance) == 64);
static_assert(MaxDrawItems * sizeof(Instance) <= (std::numeric_limits<UINT>::max)());

constexpr char ShaderSource[] = R"hlsl(
cbuffer Target : register(b0) { float2 targetSize; float2 targetPadding; };
struct VertexInput { float4 rect : RECT; float4 uv : TEXCOORD0;
                     float4 color : COLOR0; float shape : SHAPE; };
struct PixelInput { float4 position : SV_POSITION; float2 uv : TEXCOORD0;
                    float4 color : COLOR0; nointerpolation float shape : SHAPE; };
PixelInput VSMain(VertexInput input, uint vertex : SV_VertexID) {
    const float2 corners[6] = { float2(0,0), float2(1,0), float2(0,1),
                               float2(0,1), float2(1,0), float2(1,1) };
    float2 corner = corners[vertex];
    float2 pixel = input.rect.xy + corner * input.rect.zw;
    PixelInput output;
    output.position = float4(pixel.x * 2.0 / targetSize.x - 1.0,
                             1.0 - pixel.y * 2.0 / targetSize.y, 0.0, 1.0);
    output.uv = lerp(input.uv.xy, input.uv.zw, corner);
    output.color = input.color;
    output.shape = input.shape;
    return output;
}
float branchDistance(float2 p, float start, float end, float reach) {
    float2 branch = float2(end - start, reach);
    float2 local = p - float2(start, 0.0);
    float projection = saturate(dot(local, branch) / dot(branch, branch));
    return length(local - projection * branch);
}
float crystalDistance(float2 p) {
    const float cos30 = 0.8660254037844386;
    p = abs(p);
    float diagonal = dot(p, float2(cos30, 0.5));
    float along = max(p.y, diagonal);
    float across = diagonal > p.y ? abs(0.5 * p.x - cos30 * p.y) : p.x;
    float tip = max(along - 0.90, 0.0);
    float spine = length(float2(across, tip)) - 0.070;
    float innerBranch = branchDistance(float2(along, across), 0.38, 0.59, 0.18) - 0.045;
    float outerBranch = branchDistance(float2(along, across), 0.63, 0.80, 0.13) - 0.032;
    return min(min(spine, along - 0.12), min(innerBranch, outerBranch));
}
float4 PSMain(PixelInput input) : SV_TARGET {
    float coverage = 1.0;
    if (input.shape == 1.0) {
        float distance = length(input.uv * 2.0 - 1.0);
        float antialiasWidth = max(fwidth(distance), 0.00001);
        coverage = saturate((1.0 - distance) / antialiasWidth + 0.5);
    } else if (input.shape == 2.0) {
        float2 p = input.uv * 2.0 - 1.0;
        float2 pixelSpan = fwidth(p);
        // Original UV derivatives survive monitor clipping; use the shorter
        // half-extent as in the CPU path, preserving a one-pixel AA transition.
        float normalizedPixel = max(max(pixelSpan.x, pixelSpan.y), 1.0e-30);
        coverage = saturate(0.5 - crystalDistance(p) / normalizedPixel);
    }
    float alpha = input.color.a * coverage;
    return float4(input.color.rgb * alpha, alpha);
}
)hlsl";

bool compileShader(const char* entry, const char* target, ComPtr<ID3DBlob>& bytecode) {
    ComPtr<ID3DBlob> errors;
    return SUCCEEDED(D3DCompile(ShaderSource, sizeof(ShaderSource) - 1, "Snow procedural shapes",
                               nullptr, nullptr, entry, target,
                               D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
                               0, bytecode.GetAddressOf(), errors.GetAddressOf()));
}

struct D3dResources {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain1> swapChain;
    ComPtr<ID3D11RenderTargetView> target;
    ComPtr<ID3D11VertexShader> vertexShader;
    ComPtr<ID3D11PixelShader> pixelShader;
    ComPtr<ID3D11InputLayout> inputLayout;
    ComPtr<ID3D11Buffer> instances;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11BlendState> blend;
    ComPtr<ID3D11RasterizerState> rasterizer;
    ComPtr<IDCompositionDevice> composition;
    ComPtr<IDCompositionTarget> compositionTarget;
    ComPtr<IDCompositionVisual> visual;

    D3dResources() = default;
    D3dResources(const D3dResources&) = delete;
    D3dResources& operator=(const D3dResources&) = delete;
    ~D3dResources() {
        if (compositionTarget != nullptr) compositionTarget->SetRoot(nullptr);
        if (composition != nullptr) composition->Commit();
        if (context != nullptr) { context->ClearState(); context->Flush(); }
    }

    bool createTarget() {
        ComPtr<ID3D11Texture2D> backBuffer;
        if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(backBuffer.GetAddressOf())))) return false;
        return SUCCEEDED(device->CreateRenderTargetView(backBuffer.Get(), nullptr, target.GetAddressOf()));
    }

    bool create(HWND hwnd, int width, int height) {
        constexpr D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_0};
        D3D_FEATURE_LEVEL level{};
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                    D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels,
                                    static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
                                    device.GetAddressOf(), &level, context.GetAddressOf()))) return false;
        ComPtr<IDXGIDevice1> dxgiDevice;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<IDXGIFactory2> factory;
        if (FAILED(device.As(&dxgiDevice)) || FAILED(dxgiDevice->SetMaximumFrameLatency(1)) ||
            FAILED(dxgiDevice->GetAdapter(adapter.GetAddressOf())) ||
            FAILED(adapter->GetParent(IID_PPV_ARGS(factory.GetAddressOf())))) return false;
        DXGI_SWAP_CHAIN_DESC1 description{};
        description.Width = static_cast<UINT>(width);
        description.Height = static_cast<UINT>(height);
        description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        description.BufferCount = 2;
        description.Scaling = DXGI_SCALING_STRETCH;
        description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        description.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        if (FAILED(factory->CreateSwapChainForComposition(device.Get(), &description, nullptr,
                                                         swapChain.GetAddressOf())) || !createTarget())
            return false;
        ComPtr<ID3DBlob> vertexCode;
        ComPtr<ID3DBlob> pixelCode;
        if (!compileShader("VSMain", "vs_5_0", vertexCode) ||
            !compileShader("PSMain", "ps_5_0", pixelCode) ||
            FAILED(device->CreateVertexShader(vertexCode->GetBufferPointer(), vertexCode->GetBufferSize(),
                                               nullptr, vertexShader.GetAddressOf())) ||
            FAILED(device->CreatePixelShader(pixelCode->GetBufferPointer(), pixelCode->GetBufferSize(),
                                              nullptr, pixelShader.GetAddressOf()))) return false;
        constexpr D3D11_INPUT_ELEMENT_DESC elements[]{
            {"RECT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_INSTANCE_DATA, 1},
            {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D11_INPUT_PER_INSTANCE_DATA, 1},
            {"SHAPE", 0, DXGI_FORMAT_R32_FLOAT, 0, 48, D3D11_INPUT_PER_INSTANCE_DATA, 1}
        };
        if (FAILED(device->CreateInputLayout(elements, static_cast<UINT>(std::size(elements)),
                                             vertexCode->GetBufferPointer(), vertexCode->GetBufferSize(),
                                             inputLayout.GetAddressOf()))) return false;
        D3D11_BUFFER_DESC instanceDescription{};
        instanceDescription.ByteWidth = static_cast<UINT>(MaxDrawItems * sizeof(Instance));
        instanceDescription.Usage = D3D11_USAGE_DYNAMIC;
        instanceDescription.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        instanceDescription.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device->CreateBuffer(&instanceDescription, nullptr, instances.GetAddressOf()))) return false;
        D3D11_BUFFER_DESC constantDescription{};
        constantDescription.ByteWidth = 16;
        constantDescription.Usage = D3D11_USAGE_DEFAULT;
        constantDescription.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        if (FAILED(device->CreateBuffer(&constantDescription, nullptr, constants.GetAddressOf()))) return false;
        D3D11_BLEND_DESC blendDescription{};
        auto& renderTarget = blendDescription.RenderTarget[0];
        renderTarget.BlendEnable = TRUE;
        renderTarget.SrcBlend = D3D11_BLEND_ONE;
        renderTarget.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        renderTarget.BlendOp = D3D11_BLEND_OP_ADD;
        renderTarget.SrcBlendAlpha = D3D11_BLEND_ONE;
        renderTarget.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        renderTarget.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        renderTarget.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(device->CreateBlendState(&blendDescription, blend.GetAddressOf()))) return false;
        D3D11_RASTERIZER_DESC rasterDescription{};
        rasterDescription.FillMode = D3D11_FILL_SOLID;
        rasterDescription.CullMode = D3D11_CULL_NONE;
        rasterDescription.DepthClipEnable = TRUE;
        if (FAILED(device->CreateRasterizerState(&rasterDescription, rasterizer.GetAddressOf()))) return false;
        if (FAILED(DCompositionCreateDevice(dxgiDevice.Get(), IID_PPV_ARGS(composition.GetAddressOf()))) ||
            FAILED(composition->CreateTargetForHwnd(hwnd, TRUE, compositionTarget.GetAddressOf())) ||
            FAILED(composition->CreateVisual(visual.GetAddressOf())) ||
            FAILED(visual->SetContent(swapChain.Get())) ||
            FAILED(compositionTarget->SetRoot(visual.Get())) ||
            FAILED(composition->Commit())) return false;
        // Initialize layered-window visibility/hit-testing without a CPU image.
        // NOREDIRECTIONBITMAP avoids an opaque GDI backing surface beneath DComp.
        return SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA) != FALSE;
    }

    bool resize(int width, int height) {
        context->OMSetRenderTargets(0, nullptr, nullptr);
        target.Reset();
        context->Flush();
        if (FAILED(swapChain->ResizeBuffers(2, static_cast<UINT>(width), static_cast<UINT>(height),
                                            DXGI_FORMAT_B8G8R8A8_UNORM, 0))) return false;
        return createTarget();
    }

    bool render(std::span<const DrawItem> items, Rect monitor, int width, int height) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(instances.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return false;
        auto* output = static_cast<Instance*>(mapped.pData);
        UINT count = 0;
        for (const DrawItem& item : items) {
            PreparedItem prepared;
            if (!prepare(item, monitor, prepared)) continue;
            Instance instance{};
            instance.rect = {static_cast<float>(prepared.clippedLeft),
                             static_cast<float>(prepared.clippedTop),
                             static_cast<float>(prepared.clippedRight - prepared.clippedLeft),
                             static_cast<float>(prepared.clippedBottom - prepared.clippedTop)};
            const double itemWidth = prepared.right - prepared.left;
            const double itemHeight = prepared.bottom - prepared.top;
            instance.uv = {static_cast<float>((prepared.clippedLeft - prepared.left) / itemWidth),
                           static_cast<float>((prepared.clippedTop - prepared.top) / itemHeight),
                           static_cast<float>((prepared.clippedRight - prepared.left) / itemWidth),
                           static_cast<float>((prepared.clippedBottom - prepared.top) / itemHeight)};
            instance.color = {prepared.r, prepared.g, prepared.b, prepared.a};
            instance.shape = static_cast<float>(prepared.primitive);
            output[count++] = instance;
        }
        context->Unmap(instances.Get(), 0);
        constexpr float transparent[4]{};
        context->ClearRenderTargetView(target.Get(), transparent);
        ID3D11RenderTargetView* targetView = target.Get();
        context->OMSetRenderTargets(1, &targetView, nullptr);
        context->OMSetBlendState(blend.Get(), nullptr, 0xFFFFFFFFU);
        const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(width), static_cast<float>(height), 0, 1};
        context->RSSetViewports(1, &viewport);
        context->RSSetState(rasterizer.Get());
        const std::array<float, 4> targetSize{static_cast<float>(width), static_cast<float>(height), 0, 0};
        context->UpdateSubresource(constants.Get(), 0, nullptr, targetSize.data(), 0, 0);
        ID3D11Buffer* constantBuffer = constants.Get();
        context->VSSetConstantBuffers(0, 1, &constantBuffer);
        context->VSSetShader(vertexShader.Get(), nullptr, 0);
        context->PSSetShader(pixelShader.Get(), nullptr, 0);
        context->IASetInputLayout(inputLayout.Get());
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11Buffer* instanceBuffer = instances.Get();
        constexpr UINT stride = static_cast<UINT>(sizeof(Instance));
        constexpr UINT offset = 0;
        context->IASetVertexBuffers(0, 1, &instanceBuffer, &stride, &offset);
        if (count != 0) context->DrawInstanced(6, count, 0, 0);
        // The UI thread submits once; no retry loop on removed/reset devices.
        return SUCCEEDED(swapChain->Present(1, 0));
    }
};
}

struct Renderer::Impl {
    HWND hwnd{};
    Rect monitor{};
    RendererKind kind{RendererKind::Gdi};
    std::uint64_t presents{};
    int width{}, height{};
    GdiSurface gdi;
    std::unique_ptr<D3dResources> d3d;
};

Renderer::Renderer() noexcept = default;
Renderer::~Renderer() = default;
Renderer::Renderer(Renderer&&) noexcept = default;
Renderer& Renderer::operator=(Renderer&&) noexcept = default;

DWORD Renderer::windowStyles(RendererKind kind) noexcept {
    const DWORD common = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE |
                         WS_EX_TOOLWINDOW | WS_EX_TOPMOST;
    return kind == RendererKind::D3D11 ? common | WS_EX_NOREDIRECTIONBITMAP : common;
}

bool Renderer::initialize(HWND hwnd, Rect monitor, RendererKind kind) {
    release();
    int width = 0;
    int height = 0;
    if ((kind != RendererKind::Gdi && kind != RendererKind::D3D11) ||
        !dimensions(monitor, width, height) || hwnd == nullptr || IsWindow(hwnd) == FALSE ||
        GetWindowThreadProcessId(hwnd, nullptr) != GetCurrentThreadId()) return false;
    const auto styles = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
    const DWORD expectedStyles = windowStyles(kind);
    if ((styles & expectedStyles) != expectedStyles) return false;
    const auto normalStyles = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
    if ((normalStyles & WS_POPUP) == 0 || (normalStyles & WS_CHILD) != 0) return false;
    std::unique_ptr<Impl> candidate(new (std::nothrow) Impl);
    if (candidate == nullptr) return false;
    candidate->hwnd = hwnd;
    candidate->monitor = monitor;
    candidate->kind = kind;
    candidate->width = width;
    candidate->height = height;
    if (kind == RendererKind::Gdi) {
        if (!candidate->gdi.create(width, height)) return false;
    } else {
        candidate->d3d.reset(new (std::nothrow) D3dResources);
        if (candidate->d3d == nullptr || !candidate->d3d->create(hwnd, width, height)) return false;
    }
    if (SetWindowPos(hwnd, HWND_TOPMOST, monitor.left, monitor.top, width, height,
                     SWP_NOACTIVATE) == FALSE) return false;
    impl_ = std::move(candidate);
    return true;
}

bool Renderer::resize(Rect monitor) {
    if (impl_ == nullptr) return false;
    int width = 0;
    int height = 0;
    if (!dimensions(monitor, width, height)) return false;
    if (width != impl_->width || height != impl_->height) {
        const bool resized = impl_->kind == RendererKind::Gdi
            ? impl_->gdi.create(width, height) : impl_->d3d->resize(width, height);
        if (!resized) { release(); return false; }
    }
    if (SetWindowPos(impl_->hwnd, HWND_TOPMOST, monitor.left, monitor.top, width, height,
                     SWP_NOACTIVATE) == FALSE) { release(); return false; }
    impl_->monitor = monitor;
    impl_->width = width;
    impl_->height = height;
    return true;
}

bool Renderer::render(std::span<const DrawItem> items) {
    if (impl_ == nullptr || items.size() > MaxDrawItems) return false;
    bool presented = false;
    if (impl_->kind == RendererKind::Gdi) {
        auto& target = impl_->gdi;
        std::memset(target.pixels, 0, static_cast<std::size_t>(target.bytes));
        for (const DrawItem& item : items) {
            PreparedItem prepared;
            if (prepare(item, impl_->monitor, prepared)) rasterize(target, prepared);
        }
        POINT destination{impl_->monitor.left, impl_->monitor.top};
        POINT source{};
        SIZE size{impl_->width, impl_->height};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        presented = UpdateLayeredWindow(impl_->hwnd, nullptr, &destination, &size, target.dc,
                                        &source, 0, &blend, ULW_ALPHA) != FALSE;
    } else {
        presented = impl_->d3d->render(items, impl_->monitor, impl_->width, impl_->height);
    }
    if (!presented) { release(); return false; }
    ++impl_->presents;
    return true;
}

void Renderer::release() noexcept { impl_.reset(); }

const wchar_t* Renderer::name() const noexcept {
    if (impl_ == nullptr) return L"uninitialized";
    return impl_->kind == RendererKind::Gdi ? L"GDI alpha" : L"D3D11/DirectComposition (experimental)";
}

std::uint64_t Renderer::cpuBufferBytes() const noexcept {
    if (impl_ == nullptr) return 0;
    return impl_->kind == RendererKind::Gdi
        ? impl_->gdi.bytes : static_cast<std::uint64_t>(MaxDrawItems * sizeof(Instance) + 16);
}

std::uint64_t Renderer::presentCount() const noexcept {
    return impl_ == nullptr ? 0 : impl_->presents;
}
}
