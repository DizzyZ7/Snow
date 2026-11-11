#include <windows.h>
#include <shellapi.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <chrono>
#include <random>
#include <vector>

namespace {
constexpr UINT WM_TRAY = WM_APP + 1;
constexpr UINT ID_PAUSE = 1001;
constexpr UINT ID_EXIT = 1002;
constexpr UINT ID_LIGHT = 1003;
constexpr UINT ID_NORMAL = 1004;
constexpr UINT ID_BLIZZARD = 1005;
constexpr UINT_PTR TIMER_ID = 1;
struct Flake { float x, y, speed, drift, radius; };
HWND overlay = nullptr;
HWND controller = nullptr;
NOTIFYICONDATAW tray{};
RECT desktop{};
std::vector<Flake> flakes;
std::mt19937 rng{std::random_device{}()};
bool paused = false;
UINT taskbarCreatedMessage = 0;
unsigned tickCount = 0;
int weatherMode = 1;
std::chrono::steady_clock::time_point lastFrame = std::chrono::steady_clock::now();
int width = 0, height = 0;
HDC bufferDC = nullptr;
HBITMAP bufferBitmap = nullptr;
HGDIOBJ previousBitmap = nullptr;
void releaseBuffer() {
    if (bufferDC) {
        if (previousBitmap) SelectObject(bufferDC, previousBitmap);
        if (bufferBitmap) DeleteObject(bufferBitmap);
        DeleteDC(bufferDC);
    }
    bufferDC = nullptr;
    bufferBitmap = nullptr;
    previousBitmap = nullptr;
}
void resizeDesktop() {
    desktop.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    desktop.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    desktop.right = desktop.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    desktop.bottom = desktop.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    width = desktop.right - desktop.left;
    height = desktop.bottom - desktop.top;
    releaseBuffer();
    if (width <= 0 || height <= 0) return;
    HDC screen = GetDC(nullptr);
    bufferDC = CreateCompatibleDC(screen);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    bufferBitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!bufferDC || !bufferBitmap) { releaseBuffer(); return; }
    previousBitmap = SelectObject(bufferDC, bufferBitmap);
    SetWindowPos(overlay, HWND_TOPMOST, desktop.left, desktop.top, width, height,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    std::uniform_real_distribution<float> px(0.f, static_cast<float>(width));
    std::uniform_real_distribution<float> py(0.f, static_cast<float>(height));
    flakes.clear();
    const int divisor = weatherMode == 0 ? 15000 : weatherMode == 2 ? 2800 : 5500;
    const int count = std::clamp(width * height / divisor, 60, 1800);
    for (int i = 0; i < count; ++i)
        flakes.push_back({px(rng), py(rng), 0, 0, 0});
    std::uniform_real_distribution<float> speed(25.f, 85.f);
    std::uniform_real_distribution<float> drift(-18.f, 18.f);
    std::uniform_real_distribution<float> radius(1.f, 3.f);
    for (auto& f : flakes) { f.speed = speed(rng); f.drift = drift(rng); f.radius = radius(rng); }
}
void render() {
    if (!bufferDC || !bufferBitmap || width <= 0 || height <= 0) return;
    RECT bounds{0, 0, width, height};
    HBRUSH black = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    FillRect(bufferDC, &bounds, black);
    // Color-key transparency: black is transparent; white flakes remain visible.
    HBRUSH snow = CreateSolidBrush(RGB(245, 248, 255));
    if (!snow) return;
    HGDIOBJ oldBrush = SelectObject(bufferDC, snow);
    HGDIOBJ oldPen = SelectObject(bufferDC, GetStockObject(NULL_PEN));
    for (const auto& f : flakes) {
        const int x = static_cast<int>(f.x), y = static_cast<int>(f.y);
        const int r = static_cast<int>(std::ceil(f.radius));
        Ellipse(bufferDC, x-r, y-r, x+r+1, y+r+1);
    }
    SelectObject(bufferDC, oldPen);
    SelectObject(bufferDC, oldBrush);
    DeleteObject(snow);
    POINT source{0,0};
    SIZE size{width,height};
    POINT destination{desktop.left,desktop.top};
    HDC screen = GetDC(nullptr);
    // ULW_COLORKEY keeps the overlay transparent to view, while WS_EX_TRANSPARENT
    // makes it click-through.
    UpdateLayeredWindow(overlay, screen, &destination, &size, bufferDC,
                        &source, RGB(0,0,0), nullptr, ULW_COLORKEY);
    ReleaseDC(nullptr, screen);
}
void tick() {
    if (paused || width <= 0 || height <= 0) return;
    const auto now = std::chrono::steady_clock::now();
    const float dt = std::clamp(std::chrono::duration<float>(now - lastFrame).count(), 0.0f, 0.05f);
    lastFrame = now;
    const float wind = weatherMode == 2 ? 70.0f : 0.0f;
    for (auto& f : flakes) {
        f.y += f.speed * dt;
        f.x += (f.drift + wind) * dt;
        if (f.y > height + 5) f.y = -5;
        if (f.x < -5) f.x = static_cast<float>(width + 4);
        if (f.x > width + 5) f.x = -4;
    }
    render();
}
void menu() {
    POINT p{};
    GetCursorPos(&p);
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | (paused ? MF_CHECKED : 0), ID_PAUSE, L"Пауза");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | (weatherMode == 0 ? MF_CHECKED : 0), ID_LIGHT, L"Легкий снег");
    AppendMenuW(m, MF_STRING | (weatherMode == 1 ? MF_CHECKED : 0), ID_NORMAL, L"Обычный снег");
    AppendMenuW(m, MF_STRING | (weatherMode == 2 ? MF_CHECKED : 0), ID_BLIZZARD, L"Метель");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_EXIT, L"Выход");
    SetForegroundWindow(controller);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, p.x, p.y, 0, controller, nullptr);
    DestroyMenu(m);
}
LRESULT CALLBACK controllerProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (taskbarCreatedMessage && msg == taskbarCreatedMessage) {
        Shell_NotifyIconW(NIM_ADD, &tray);
        return 0;
    }
    switch (msg) {
    case WM_TRAY:
        if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU || lp == WM_LBUTTONUP) menu();
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == ID_PAUSE) { paused = !paused; lastFrame = std::chrono::steady_clock::now(); return 0; }
        if (LOWORD(wp) >= ID_LIGHT && LOWORD(wp) <= ID_BLIZZARD) {
            weatherMode = static_cast<int>(LOWORD(wp) - ID_LIGHT);
            resizeDesktop();
            render();
            return 0;
        }
        if (LOWORD(wp) == ID_EXIT) { DestroyWindow(hwnd); return 0; }
        break;
    case WM_DISPLAYCHANGE:
    case WM_SETTINGCHANGE:
        resizeDesktop();
        return 0;
    case WM_TIMER:
        if (wp == TIMER_ID) {
            // Message-only windows do not reliably receive desktop broadcast messages.
            // Reconcile virtual desktop geometry periodically for monitor changes.
            if (++tickCount % 60 == 0) {
                const int left = GetSystemMetrics(SM_XVIRTUALSCREEN);
                const int top = GetSystemMetrics(SM_YVIRTUALSCREEN);
                const int w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
                const int h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
                if (left != desktop.left || top != desktop.top || w != width || h != height)
                    resizeDesktop();
            }
            tick();
        }
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, TIMER_ID);
        Shell_NotifyIconW(NIM_DELETE, &tray);
        releaseBuffer();
        if (overlay) DestroyWindow(overlay);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
LRESULT CALLBACK overlayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
    if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(hwnd, msg, wp, lp);
}
}
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
        SetProcessDPIAware();
    taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");
    WNDCLASSW ctl{};
    ctl.hInstance = instance;
    ctl.lpfnWndProc = controllerProc;
    ctl.lpszClassName = L"SnowController";
    if (!RegisterClassW(&ctl)) return 1;
    WNDCLASSW ov{};
    ov.hInstance = instance;
    ov.lpfnWndProc = overlayProc;
    ov.lpszClassName = L"SnowOverlay";
    if (!RegisterClassW(&ov)) return 1;
    // A hidden top-level window receives Explorer's TaskbarCreated broadcast.
    controller = CreateWindowExW(WS_EX_TOOLWINDOW, ctl.lpszClassName, L"Snow", WS_POPUP,
                                 0,0,0,0, nullptr, nullptr, instance, nullptr);
    if (!controller) return 1;
    overlay = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE |
                              WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
                              ov.lpszClassName, L"Snow Overlay", WS_POPUP,
                              0,0,1,1,nullptr,nullptr,instance,nullptr);
    if (!overlay) { DestroyWindow(controller); return 1; }
    tray.cbSize = sizeof(tray);
    tray.hWnd = controller;
    tray.uID = 1;
    tray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    tray.uCallbackMessage = WM_TRAY;
    tray.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(tray.szTip, L"Snow — снег на рабочем столе");
    if (!Shell_NotifyIconW(NIM_ADD, &tray)) {
        DestroyWindow(controller);
        return 1;
    }
    resizeDesktop();
    render();
    if (!SetTimer(controller, TIMER_ID, 33, nullptr)) {
        DestroyWindow(controller);
        return 1;
    }
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}
