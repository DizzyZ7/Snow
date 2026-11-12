#include "renderer.hpp"
#include <windowsx.h>
#include <chrono>
#include <cwchar>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
constexpr wchar_t HelperClass[] = L"Snow.InputSmoke.Foreign";
constexpr wchar_t OverlayClass[] = L"Snow.InputSmoke.Overlay";
constexpr wchar_t ClickProperty[] = L"Snow.InputSmoke.Clicks";
unsigned overlayClicks{};
bool interactive{};
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }

LRESULT CALLBACK helperProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (message == WM_LBUTTONDOWN) {
        const auto count = reinterpret_cast<UINT_PTR>(GetPropW(hwnd, ClickProperty));
        SetPropW(hwnd, ClickProperty, reinterpret_cast<HANDLE>(count + 1));
        return 0;
    }
    if (message == WM_CLOSE) { RemovePropW(hwnd, ClickProperty); DestroyWindow(hwnd); PostQuitMessage(0); return 0; }
    return DefWindowProcW(hwnd, message, wp, lp);
}
LRESULT CALLBACK overlayProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (message == WM_NCHITTEST) return interactive ? HTCLIENT : HTTRANSPARENT;
    if (message == WM_LBUTTONDOWN) { ++overlayClicks; return 0; }
    if (message == WM_ERASEBKGND) return 1;
    return DefWindowProcW(hwnd, message, wp, lp);
}
int helperMain(const wchar_t* title) {
    WNDCLASSW cls{}; cls.hInstance = GetModuleHandleW(nullptr); cls.lpfnWndProc = helperProc;
    cls.lpszClassName = HelperClass; cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    if (!RegisterClassW(&cls)) return 1;
    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, HelperClass, title,
        WS_POPUP, 0, 0, 240, 180, nullptr, nullptr, cls.hInstance, nullptr);
    if (!window) return 1;
    MSG message{}; BOOL next = 0;
    while ((next = GetMessageW(&message, nullptr, 0, 0)) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
    return next == -1 ? 1 : 0;
}
template<class Predicate> bool waitFor(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
        if (predicate()) return true;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}
struct Fixture {
    PROCESS_INFORMATION helper{};
    HWND target{}, overlay{};
    POINT oldCursor{};
    bool cursorSaved{};
    Fixture() { cursorSaved = GetCursorPos(&oldCursor) != FALSE; }
    ~Fixture() {
        if (overlay) DestroyWindow(overlay);
        if (target && IsWindow(target)) PostMessageW(target, WM_CLOSE, 0, 0);
        if (helper.hProcess) {
            if (WaitForSingleObject(helper.hProcess, 2000) == WAIT_TIMEOUT) TerminateProcess(helper.hProcess, 2);
            CloseHandle(helper.hProcess);
        }
        if (helper.hThread) CloseHandle(helper.hThread);
        if (cursorSaved) SetCursorPos(oldCursor.x, oldCursor.y);
    }
    void start() {
        std::wstring exe(32768, L'\0');
        const DWORD count = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
        require(count > 0 && count < exe.size(), "find input test executable"); exe.resize(count);
        const std::wstring title = L"Snow input fixture " + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
        std::wstring command = L"\"" + exe + L"\" --helper \"" + title + L"\"";
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        require(CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &helper) != FALSE,
            "start separate input recipient process");
        require(waitFor([&] { target = FindWindowW(HelperClass, title.c_str()); return target != nullptr; }), "foreign input fixture appears");
    }
};
void click(POINT point) {
    const int x = GetSystemMetrics(SM_XVIRTUALSCREEN), y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN), height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    require(width > 1 && height > 1, "input desktop has valid physical extent");
    INPUT input[3]{};
    input[0].type = INPUT_MOUSE;
    input[0].mi.dx = MulDiv(point.x - x, 65535, width - 1);
    input[0].mi.dy = MulDiv(point.y - y, 65535, height - 1);
    input[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    input[1].type = INPUT_MOUSE; input[1].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    input[2].type = INPUT_MOUSE; input[2].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    require(SendInput(3, input, sizeof(INPUT)) == 3, "send real mouse input in test desktop");
}
unsigned foreignClicks(HWND target) { return static_cast<unsigned>(reinterpret_cast<UINT_PTR>(GetPropW(target, ClickProperty))); }
int run(snow::RendererKind kind) {
    Fixture fixture; fixture.start();
    MONITORINFO monitor{}; monitor.cbSize = sizeof(monitor);
    require(GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &monitor) != FALSE, "read input monitor");
    const int x = static_cast<int>(monitor.rcMonitor.left) + 40, y = static_cast<int>(monitor.rcMonitor.top) + 140;
    const HWND target = fixture.target;
    if (!target) throw std::runtime_error("input recipient disappeared");
    require(SetWindowPos(target, HWND_TOPMOST, x, y, 240, 180, SWP_SHOWWINDOW | SWP_NOACTIVATE) != FALSE, "show foreign input recipient");
    WNDCLASSW cls{}; cls.hInstance = GetModuleHandleW(nullptr); cls.lpfnWndProc = overlayProc; cls.lpszClassName = OverlayClass;
    require(RegisterClassW(&cls) != 0, "register input overlay");
    fixture.overlay = CreateWindowExW(snow::Renderer::windowStyles(kind), OverlayClass, L"Snow input overlay", WS_POPUP,
        x, y, 240, 180, nullptr, nullptr, cls.hInstance, nullptr);
    const HWND overlay = fixture.overlay;
    if (!overlay) throw std::runtime_error("create overlay for input test");
    snow::Renderer renderer;
    if (!renderer.initialize(overlay, {x, y, x + 240, y + 180}, kind)) {
        if (kind == snow::RendererKind::D3D11) { std::cout << "SKIP: GPU input test device unavailable\n"; return 77; }
        throw std::runtime_error("initialize GDI input fixture");
    }
    const snow::DrawItem visible{static_cast<float>(x + 20), static_cast<float>(y + 20), 100, 100, 1, 1, 1, .8f, 1};
    require(renderer.render(std::span(&visible, 1)), "render visible snow over foreign window");
    ShowWindow(overlay, SW_SHOWNOACTIVATE);
    const HWND foreground = GetForegroundWindow();
    click({x + 60, y + 60});
    require(waitFor([&] { return foreignClicks(target) == 1; }), "click crosses visible snow into another process");
    require(overlayClicks == 0 && GetForegroundWindow() == foreground, "normal snow neither captures mouse nor steals focus");

    auto style = GetWindowLongPtrW(overlay, GWL_EXSTYLE);
    SetWindowLongPtrW(overlay, GWL_EXSTYLE, style & ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT));
    interactive = true;
    SetWindowPos(overlay, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    const snow::DrawItem plane{static_cast<float>(x), static_cast<float>(y), 240, 180, 0, 0, 0, 1.f / 255.f, 0};
    require(renderer.render(std::span(&plane, 1)), "present explicit input plane");
    click({x + 180, y + 140});
    require(waitFor([] { return overlayClicks == 1; }), "explicit tool captures a gap between snowflakes");
    require(foreignClicks(target) == 1 && GetForegroundWindow() == foreground, "tool input does not leak to underlying app or steal keyboard focus");

    interactive = false;
    SetWindowLongPtrW(overlay, GWL_EXSTYLE, style);
    SetWindowPos(overlay, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    require(renderer.render({}), "clear input plane on exit");
    click({x + 60, y + 60});
    require(waitFor([&] { return foreignClicks(target) == 2; }), "leaving tool mode restores cross-process click-through");
    require(overlayClicks == 1, "overlay stops receiving mouse after tool exit");
    std::cout << "PASS: real cross-process mouse routing and no-focus overlay lifecycle\n";
    return 0;
}
}
int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (argc == 3 && std::wcscmp(argv[1], L"--helper") == 0) return helperMain(argv[2]);
    try { return run(argc == 2 && std::wcscmp(argv[1], L"--d3d") == 0 ? snow::RendererKind::D3D11 : snow::RendererKind::Gdi); }
    catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
