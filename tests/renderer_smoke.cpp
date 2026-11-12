#include "renderer.hpp"

#include <dwmapi.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <iostream>
#include <limits>
#include <span>
#include <vector>

namespace {
constexpr wchar_t OverlayClass[] = L"SnowRendererSmokeOverlay";
constexpr wchar_t BackdropClass[] = L"SnowRendererSmokeBackdrop";

bool expect(bool condition, const char* message) {
    if (!condition) std::cerr << "FAIL: " << message << '\n';
    return condition;
}

LRESULT CALLBACK overlayProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_NCHITTEST) return HTTRANSPARENT;
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (message == WM_ERASEBKGND) return 1;
    return DefWindowProcW(hwnd, message, wp, lp);
}

LRESULT CALLBACK backdropProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(hwnd, &paint);
        SetDCBrushColor(dc, RGB(0, 0, 255));
        FillRect(dc, &paint.rcPaint, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        EndPaint(hwnd, &paint);
        return 0;
    }
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(hwnd, message, wp, lp);
}

struct Window {
    HWND hwnd{};
    ~Window() { if (hwnd != nullptr) DestroyWindow(hwnd); }
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    explicit Window(DWORD extendedStyle, const wchar_t* className = OverlayClass) {
        hwnd = CreateWindowExW(extendedStyle, className, L"Snow renderer smoke", WS_POPUP,
                              0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    }
};

void pumpMessages() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

bool registerClasses() {
    WNDCLASSW overlay{};
    overlay.hInstance = GetModuleHandleW(nullptr);
    overlay.lpfnWndProc = overlayProc;
    overlay.lpszClassName = OverlayClass;
    WNDCLASSW backdrop = overlay;
    backdrop.lpfnWndProc = backdropProc;
    backdrop.lpszClassName = BackdropClass;
    return RegisterClassW(&overlay) != 0 && RegisterClassW(&backdrop) != 0;
}

// Catches dropped alpha/premultiplication, stale clears, wrong desktop origin,
// leaked DIB/DC handles, unbounded per-frame resources, and oversized input.
bool exerciseRenderer(snow::RendererKind kind, bool optionalCapability, bool& skipped) {
    skipped = false;
    const DWORD before = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    const DWORD required = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE;
    bool ok = expect((snow::Renderer::windowStyles(kind) & required) == required,
                     "normal overlays must be layered, click-through and nonactivating");
    {
        Window window(snow::Renderer::windowStyles(kind));
        if (!window.hwnd) return expect(false, "create overlay");
        snow::Renderer renderer;
        if (!renderer.initialize(window.hwnd, {24, 24, 152, 120}, kind)) {
            if (optionalCapability) {
                skipped = true;
                std::cout << "SKIP: D3D11 initialization unavailable on this desktop/device; "
                             "D3D11 rendering was not tested\n";
                return ok;
            }
            return expect(false, "initialize GDI renderer");
        }
        const std::array<snow::DrawItem, 5> items{{
            {32, 32, 32, 32, 1, 0, 0, 0.5F, 0},
            {48, 48, 32, 32, 0, 1, 0, 0.5F, 1},
            {90, 32, 24, 24, 1, 1, 1, 0.75F, 2},
            {-1000, -1000, 1005, 1005, 1, 1, 1, 1, 1},
            {std::numeric_limits<float>::quiet_NaN(), 0, 1, 1, 1, 1, 1, 1, 0}
        }};
        ok = expect(renderer.render(items), "render alpha rectangle, ellipse and six-arm crystal") && ok;
        const auto cpuBytes = renderer.cpuBufferBytes();
        ok = expect(cpuBytes > 0 && cpuBytes <= 4ULL * 1024ULL * 1024ULL,
                    "small target CPU buffers remain bounded") && ok;
        if (kind == snow::RendererKind::Gdi) {
            ok = expect(cpuBytes == 49152, "128 by 96 GDI target owns one 32-bit image") && ok;
        }
        const DWORD steadyGdi = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
        const std::uint64_t firstPresent = renderer.presentCount();
        for (int frame = 0; frame < 120; ++frame) {
            if (!expect(renderer.render(items), "repeated present")) { ok = false; break; }
        }
        ok = expect(renderer.presentCount() >= firstPresent + 120,
                    "successful presents are counted") && ok;
        ok = expect(renderer.cpuBufferBytes() == cpuBytes, "frames do not grow CPU buffers") && ok;
        ok = expect(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) <= steadyGdi + 1,
                    "frames do not grow GDI handles") && ok;
        for (int iteration = 0; iteration < 24; ++iteration) {
            const snow::Rect monitor = (iteration % 2 == 0)
                ? snow::Rect{-96, -48, 64, 72} : snow::Rect{24, 24, 152, 120};
            if (!expect(renderer.resize(monitor), "resize with negative desktop origins")) {
                ok = false;
                break;
            }
            RECT positioned{};
            ok = expect(window.hwnd != nullptr && GetWindowRect(window.hwnd, &positioned) != FALSE &&
                        positioned.left == monitor.left && positioned.top == monitor.top &&
                        positioned.right == monitor.right && positioned.bottom == monitor.bottom,
                        "resized window preserves absolute negative monitor coordinates") && ok;
            const snow::DrawItem clipped{-100, -60, 150, 100, 0.5F, 0.25F, 1, 0.75F,
                                         iteration % 2 == 0 ? 2.0F : 1.0F};
            if (!expect(renderer.render(std::span(&clipped, 1)), "render clipped resized target")) {
                ok = false;
                break;
            }
        }
        ok = expect(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) <= steadyGdi + 1,
                    "resize replaces old GDI handles") && ok;
        std::vector<snow::DrawItem> excessive(snow::MaxDrawItems + 1);
        const auto presentsBeforeRejected = renderer.presentCount();
        ok = expect(!renderer.render(excessive), "reject over-capacity scene") && ok;
        ok = expect(renderer.presentCount() == presentsBeforeRejected,
                    "rejected scene is not presented") && ok;
        ok = expect(renderer.render({}), "empty scene clears previous content") && ok;
        ok = expect(!renderer.resize({0, 0, 0, 10}), "reject empty monitor") && ok;
        renderer.release();
        renderer.release();
        ok = expect(renderer.cpuBufferBytes() == 0, "release returns owned CPU buffers") && ok;
        ok = expect(IsWindow(window.hwnd) != FALSE, "renderer does not destroy caller-owned HWND") && ok;
        ok = expect(!renderer.render({}), "released renderer rejects rendering") && ok;
        ok = expect(!renderer.initialize(window.hwnd, {std::numeric_limits<int>::min(), 0,
                                                      std::numeric_limits<int>::max(), 10}, kind),
                    "reject overflowing monitor extent") && ok;
        ok = expect(!renderer.initialize(window.hwnd, {0, 0, 16384, 16384}, kind),
                    "reject monitor exceeding bounded pixel budget") && ok;
        if (kind == snow::RendererKind::Gdi) {
            ok = expect(renderer.initialize(window.hwnd, {24, 24, 152, 120}, kind),
                        "renderer can initialize after release/rejected dimensions") && ok;
            const HWND destroyedWindow = window.hwnd;
            window.hwnd = nullptr;
            ok = expect(DestroyWindow(destroyedWindow) != FALSE, "destroy native render target") && ok;
            ok = expect(!renderer.render(items), "native present failure propagates") && ok;
            ok = expect(renderer.cpuBufferBytes() == 0, "native present failure releases resources") && ok;
        }
    }
    pumpMessages();
    ok = expect(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) <= before + 1,
                "renderer/window lifetime returns GDI handles") && ok;
    if (ok) std::cout << "PASS: renderer lifecycle/resource smoke ("
                      << (kind == snow::RendererKind::Gdi ? "GDI" : "D3D11") << ")\n";
    return ok;
}

// Requires an unlocked interactive desktop. This reads the composited display,
// rather than adding a readback path or test-only pixel getter to the renderer.
bool visualAlpha(snow::RendererKind kind) {
    MONITORINFO info{};
    info.cbSize = static_cast<DWORD>(sizeof(info));
    if (!expect(GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &info) != FALSE,
                "query visual-test monitor")) return false;
    const int x = info.rcMonitor.left + 32;
    const int y = info.rcMonitor.top + 32;
    Window backdrop(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, BackdropClass);
    Window overlay(snow::Renderer::windowStyles(kind));
    if (!backdrop.hwnd || !overlay.hwnd) return expect(false, "create visual test windows");
    if (!expect(backdrop.hwnd != nullptr && SetWindowPos(backdrop.hwnd, HWND_TOPMOST, x, y, 96, 96,
                             SWP_NOACTIVATE | SWP_SHOWWINDOW) != FALSE, "show blue backdrop"))
        return false;
    if (backdrop.hwnd) UpdateWindow(backdrop.hwnd);
    pumpMessages();
    snow::Renderer renderer;
    if (!expect(renderer.initialize(overlay.hwnd, {x, y, x + 96, y + 96}, kind),
                "initialize visual renderer")) return false;
    const snow::DrawItem red{static_cast<float>(x + 16), static_cast<float>(y + 16),
                             48, 48, 1, 0, 0, 0.5F, 0};
    if (!expect(renderer.render(std::span(&red, 1)), "present visual alpha fixture")) return false;
    ShowWindow(overlay.hwnd, SW_SHOWNOACTIVATE);
    pumpMessages();
    if (!expect(SUCCEEDED(DwmFlush()), "wait for composited visual fixture")) return false;
    HDC screen = GetDC(nullptr);
    if (!screen) return expect(false, "capture composited display");
    const COLORREF blended = GetPixel(screen, x + 32, y + 32);
    const COLORREF clear = GetPixel(screen, x + 80, y + 80);
    ReleaseDC(nullptr, screen);
    bool ok = expect(blended != CLR_INVALID && clear != CLR_INVALID, "read display pixels");
    ok = expect(std::abs(static_cast<int>(GetRValue(blended)) - 128) <= 6 &&
                GetGValue(blended) <= 6 &&
                std::abs(static_cast<int>(GetBValue(blended)) - 127) <= 6,
                "half-alpha red is premultiplied over blue") && ok;
    ok = expect(GetRValue(clear) <= 6 && GetGValue(clear) <= 6 && GetBValue(clear) >= 249,
                "transparent pixels preserve blue backdrop") && ok;
    // Catches treating shape 2 as an ellipse, a solid star, an opaque mask or a
    // new clipped quad whose UVs incorrectly restart at the monitor boundary.
    // A half-pixel origin places the crystal center exactly on a pixel center.
    const std::array<snow::DrawItem, 7> crystalFixture{{
        {static_cast<float>(x) + 16.5F, static_cast<float>(y) + 16.5F,
         48, 48, 1, 1, 1, 0.5F, 2},
        {static_cast<float>(x) + 72.5F, static_cast<float>(y) + 64.5F,
         6, 6, 1, 1, 1, 0.5F, 2},
        {static_cast<float>(x + 72), static_cast<float>(y + 12), 8, 8, 1, 1, 1, 1, -1},
        {static_cast<float>(x + 72), static_cast<float>(y + 12), 8, 8, 1, 1, 1, 1, 0.5F},
        {static_cast<float>(x + 72), static_cast<float>(y + 12), 8, 8, 1, 1, 1, 1, 1.5F},
        {static_cast<float>(x + 72), static_cast<float>(y + 12), 8, 8, 1, 1, 1, 1, 3},
        {static_cast<float>(x + 72), static_cast<float>(y + 12), 8, 8, 1, 1, 1, 1,
         std::numeric_limits<float>::quiet_NaN()}
    }};
    if (!expect(renderer.render(crystalFixture), "present six-arm crystal fixture")) return false;
    if (!expect(SUCCEEDED(DwmFlush()), "wait for composited crystal fixture")) return false;
    screen = GetDC(nullptr);
    if (!screen) return expect(false, "capture composited crystal pixels");
    const auto whiteOverBlue = [](COLORREF pixel) {
        return pixel != CLR_INVALID && std::abs(static_cast<int>(GetRValue(pixel)) - 128) <= 6 &&
               std::abs(static_cast<int>(GetGValue(pixel)) - 128) <= 6 && GetBValue(pixel) >= 249;
    };
    const auto transparentBlue = [](COLORREF pixel) {
        return pixel != CLR_INVALID && GetRValue(pixel) <= 6 &&
               GetGValue(pixel) <= 6 && GetBValue(pixel) >= 249;
    };
    ok = expect(whiteOverBlue(GetPixel(screen, x + 40, y + 40)),
                "crystal center retains half-alpha white coverage") && ok;
    constexpr std::array<POINT, 6> armOffsets{{{0, -16}, {14, -8}, {14, 8},
                                              {0, 16}, {-14, 8}, {-14, -8}}};
    for (const POINT offset : armOffsets) {
        ok = expect(whiteOverBlue(GetPixel(screen, x + 40 + offset.x, y + 40 + offset.y)),
                    "each of six crystal arms has opaque geometric coverage") && ok;
    }
    constexpr std::array<POINT, 4> branchOffsets{{{4, -14}, {-4, -14}, {3, -19}, {-3, -19}}};
    for (const POINT offset : branchOffsets) {
        ok = expect(whiteOverBlue(GetPixel(screen, x + 40 + offset.x, y + 40 + offset.y)),
                    "both fine branch tiers extend beyond a plain six-spoke star") && ok;
    }
    constexpr std::array<POINT, 6> gapOffsets{{{9, -16}, {18, 0}, {9, 16},
                                              {-9, 16}, {-18, 0}, {-9, -16}}};
    for (const POINT offset : gapOffsets) {
        ok = expect(transparentBlue(GetPixel(screen, x + 40 + offset.x, y + 40 + offset.y)),
                    "gaps between crystal arms remain transparent") && ok;
    }
    const COLORREF antialiased = GetPixel(screen, x + 42, y + 24);
    ok = expect(antialiased != CLR_INVALID && GetRValue(antialiased) >= 4 &&
                GetRValue(antialiased) <= 100 &&
                std::abs(static_cast<int>(GetRValue(antialiased)) - GetGValue(antialiased)) <= 2 &&
                GetBValue(antialiased) >= 249,
                "crystal edge has partial premultiplied alpha coverage") && ok;
    ok = expect(transparentBlue(GetPixel(screen, x + 76, y + 16)),
                "unsupported and nonfinite primitive codes are ignored") && ok;
    const COLORREF smallCenter = GetPixel(screen, x + 75, y + 67);
    const COLORREF smallArm = GetPixel(screen, x + 75, y + 65);
    ok = expect(smallCenter != CLR_INVALID && GetRValue(smallCenter) >= 64 &&
                GetRValue(smallCenter) <= 134 && GetBValue(smallCenter) >= 249 &&
                smallArm != CLR_INVALID && GetRValue(smallArm) >= 40 &&
                GetRValue(smallArm) <= 134 && GetBValue(smallArm) >= 249,
                "six-pixel crystal retains a visible center and fine arm") && ok;
    const COLORREF smallGap = GetPixel(screen, x + 77, y + 65);
    ok = expect(smallGap != CLR_INVALID && GetRValue(smallGap) <= 32 &&
                GetGValue(smallGap) <= 32 && GetBValue(smallGap) >= 249,
                "six-pixel crystal gaps remain mostly transparent through antialiasing") && ok;
    ReleaseDC(nullptr, screen);
    if (!expect(renderer.resize({x + 40, y, x + 96, y + 96}), "clip crystal at monitor edge") ||
        !expect(renderer.render(crystalFixture), "render clipped crystal without restarting its UVs") ||
        !expect(SUCCEEDED(DwmFlush()), "wait for clipped crystal fixture")) return false;
    screen = GetDC(nullptr);
    if (!screen) return expect(false, "capture clipped crystal pixels");
    ok = expect(whiteOverBlue(GetPixel(screen, x + 54, y + 32)),
                "clipped crystal preserves its original diagonal arm") && ok;
    ok = expect(transparentBlue(GetPixel(screen, x + 58, y + 40)),
                "clipped crystal preserves its original transparent gap") && ok;
    ReleaseDC(nullptr, screen);
    if (ok) std::cout << "PASS: interactive alpha/crystal pixel check\n";
    return ok;
}
}

int wmain(int argc, wchar_t** argv) {
    bool d3d = false;
    bool visual = false;
    for (int index = 1; index < argc; ++index) {
        if (std::wcscmp(argv[index], L"--d3d") == 0) d3d = true;
        else if (std::wcscmp(argv[index], L"--visual") == 0) visual = true;
        else { std::cerr << "usage: renderer_smoke [--d3d] [--visual]\n"; return 2; }
    }
    if (!expect(registerClasses(), "register smoke window classes")) return 1;
    bool gdiSkipped = false;
    bool d3dSkipped = false;
    bool ok = exerciseRenderer(snow::RendererKind::Gdi, false, gdiSkipped);
    if (d3d) ok = exerciseRenderer(snow::RendererKind::D3D11, true, d3dSkipped) && ok;
    else std::cout << "NOT RUN: D3D11 candidate (request --d3d)\n";
    if (visual) {
        ok = visualAlpha(snow::RendererKind::Gdi) && ok;
        // --visual makes unavailable/incorrect D3D a failure when explicitly requested.
        if (d3d) ok = visualAlpha(snow::RendererKind::D3D11) && ok;
    } else std::cout << "NOT RUN: composited alpha pixel check (requires --visual and unlocked desktop)\n";
    if (!ok) return 1;
    // Configure CTest SKIP_RETURN_CODE 77 for a separately opted-in D3D test.
    return d3dSkipped ? 77 : 0;
}
