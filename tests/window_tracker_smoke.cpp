#include "window_tracker.hpp"
#include <dwmapi.h>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace {
constexpr wchar_t HelperClass[] = L"Snow.WindowTracker.Smoke.Helper";
constexpr wchar_t ControllerClass[] = L"Snow.WindowTracker.Smoke.Controller";
constexpr UINT DirtyMessage = WM_APP + 73;
constexpr ULONG_PTR CommandTag = 0x534E4F57;
enum class Action : unsigned { Move, Minimize, Restore, Occlude, Unocclude, Recreate, Fullscreen };
struct Command { Action action{}; RECT rect{}; };
struct HelperState {
    HINSTANCE instance{};
    std::wstring title;
    HWND primary{}, occluder{};
    RECT normal{80, 160, 680, 480};
};
HelperState* helperState{};

HWND createPrimary(HelperState& state) {
    return CreateWindowExW(WS_EX_TOPMOST, HelperClass, state.title.c_str(),
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, state.normal.left, state.normal.top,
        state.normal.right - state.normal.left, state.normal.bottom - state.normal.top,
        nullptr, nullptr, state.instance, nullptr);
}

LRESULT CALLBACK helperProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_COPYDATA && helperState) {
        const auto* data = reinterpret_cast<const COPYDATASTRUCT*>(lp);
        if (data && data->dwData == CommandTag && data->cbData == sizeof(Command) && data->lpData) {
            const Command command = *static_cast<const Command*>(data->lpData);
            HelperState& state = *helperState;
            switch (command.action) {
            case Action::Move:
                state.normal = command.rect;
                SetWindowPos(window, HWND_TOPMOST, command.rect.left, command.rect.top,
                    command.rect.right - command.rect.left, command.rect.bottom - command.rect.top,
                    SWP_NOACTIVATE | SWP_SHOWWINDOW);
                break;
            case Action::Minimize: ShowWindow(window, SW_MINIMIZE); break;
            case Action::Restore: ShowWindow(window, SW_RESTORE); break;
            case Action::Occlude:
                if (state.occluder) DestroyWindow(state.occluder);
                state.occluder = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                    HelperClass, L"Snow tracker smoke occluder", WS_POPUP | WS_VISIBLE,
                    command.rect.left, command.rect.top,
                    command.rect.right - command.rect.left, command.rect.bottom - command.rect.top,
                    nullptr, nullptr, state.instance, nullptr);
                break;
            case Action::Unocclude:
                if (state.occluder) DestroyWindow(state.occluder);
                state.occluder = nullptr;
                break;
            case Action::Recreate:
                DestroyWindow(window);
                state.primary = createPrimary(state);
                break;
            case Action::Fullscreen:
                SetWindowLongPtrW(window, GWL_STYLE, static_cast<LONG_PTR>(WS_POPUP | WS_VISIBLE));
                SetWindowPos(window, HWND_TOPMOST, command.rect.left, command.rect.top,
                    command.rect.right - command.rect.left, command.rect.bottom - command.rect.top,
                    SWP_FRAMECHANGED | SWP_SHOWWINDOW);
                SetForegroundWindow(window);
                break;
            }
            return TRUE;
        }
    }
    if (message == WM_CLOSE && helperState && window == helperState->primary) {
        if (helperState->occluder) DestroyWindow(helperState->occluder);
        DestroyWindow(window);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}

int helperMain(const wchar_t* title) {
    HelperState state;
    state.instance = GetModuleHandleW(nullptr);
    state.title = title;
    helperState = &state;
    WNDCLASSW klass{};
    klass.hInstance = state.instance;
    klass.lpfnWndProc = helperProc;
    klass.lpszClassName = HelperClass;
    if (!RegisterClassW(&klass) || !(state.primary = createPrimary(state))) return 2;
    MSG message{};
    BOOL result{};
    while ((result = GetMessageW(&message, nullptr, 0, 0)) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    helperState = nullptr;
    return result == -1 ? 3 : 0;
}

void require(bool condition, const char* description) {
    if (!condition) throw std::runtime_error(description);
}

template<class Predicate>
bool pumpUntil(Predicate predicate, std::chrono::milliseconds timeout = std::chrono::seconds(8)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) return false;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (predicate()) return true;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

struct Controller {
    HWND window{};
    unsigned notifications{};
    Controller() {
        WNDCLASSW klass{};
        klass.hInstance = GetModuleHandleW(nullptr);
        klass.lpszClassName = ControllerClass;
        klass.lpfnWndProc = [](HWND window, UINT message, WPARAM wp, LPARAM lp) -> LRESULT {
            if (message == WM_NCCREATE) {
                const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lp);
                SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
            }
            auto* self = reinterpret_cast<Controller*>(GetWindowLongPtrW(window, GWLP_USERDATA));
            if (message == DirtyMessage && self) { ++self->notifications; return 0; }
            return DefWindowProcW(window, message, wp, lp);
        };
        require(RegisterClassW(&klass) != 0, "register notification window");
        window = CreateWindowExW(0, ControllerClass, L"Snow tracker smoke", 0,
            0, 0, 0, 0, HWND_MESSAGE, nullptr, klass.hInstance, this);
        require(window != nullptr, "create notification window");
    }
    ~Controller() { if (window) DestroyWindow(window); }
};

struct HelperProcess {
    PROCESS_INFORMATION process{};
    std::wstring title;
    HWND window() const { return FindWindowW(HelperClass, title.c_str()); }
    HelperProcess() {
        wchar_t executable[32768]{};
        const DWORD length = GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
        require(length != 0 && length < std::size(executable), "find smoke executable");
        title = L"Snow tracker smoke " + std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64());
        std::wstring command = L"\"" + std::wstring(executable) + L"\" --helper \"" + title + L"\"";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        require(CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, 0,
            nullptr, nullptr, &startup, &process) != FALSE, "start helper process");
    }
    ~HelperProcess() {
        if (HWND target = window()) {
            DWORD_PTR ignored{};
            SendMessageTimeoutW(target, WM_CLOSE, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 1000, &ignored);
        }
        if (process.hProcess) {
            if (WaitForSingleObject(process.hProcess, 3000) == WAIT_TIMEOUT) TerminateProcess(process.hProcess, 4);
            CloseHandle(process.hProcess);
        }
        if (process.hThread) CloseHandle(process.hThread);
    }
    void send(Action action, RECT rect = {}) const {
        Command command{action, rect};
        COPYDATASTRUCT data{CommandTag, static_cast<DWORD>(sizeof(command)), &command};
        DWORD_PTR result{};
        require(SendMessageTimeoutW(window(), WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data),
            SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &result) != 0 && result != 0, "helper command");
    }
};

struct OwnWindow {
    HWND window{};
    ~OwnWindow() { if (window) DestroyWindow(window); }
};

snow::Rect physicalBounds(HWND window) {
    RECT bounds{};
    require(SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds,
        static_cast<DWORD>(sizeof(bounds)))), "read real DWM frame bounds");
    return {static_cast<int>(bounds.left), static_cast<int>(bounds.top),
        static_cast<int>(bounds.right), static_cast<int>(bounds.bottom)};
}

const snow::SurfaceSpec* findBounds(const std::vector<snow::SurfaceSpec>& surfaces, snow::Rect bounds) {
    const auto found = std::find_if(surfaces.begin(), surfaces.end(), [bounds](const auto& surface) {
        return surface.bounds == bounds;
    });
    return found == surfaces.end() ? nullptr : &*found;
}

int smokeMain() {
    Controller controller;
    snow::WindowTracker tracker;
    require(!tracker.start(nullptr, DirtyMessage), "reject invalid notification window");
    require(tracker.start(controller.window, DirtyMessage), "install out-of-context tracking");
    const auto* surfaces = &tracker.refresh();
    require(!tracker.dirty(), "refresh acknowledges initial dirty state");
    {
        OwnWindow own;
        own.window = CreateWindowExW(WS_EX_TOPMOST, ControllerClass, L"Snow own-process exclusion check",
            WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 140, 420, 250,
            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        require(own.window != nullptr, "create real own-process window");
        const snow::Rect ownBounds = physicalBounds(own.window);
        require(findBounds(tracker.refresh(), ownBounds) == nullptr, "own-process windows never support snow");
    }
    HelperProcess helper;
    require(pumpUntil([&] { return helper.window() != nullptr; }), "helper creates a real foreign window");
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    require(GetMonitorInfoW(MonitorFromWindow(helper.window(), MONITOR_DEFAULTTOPRIMARY), &monitor) != FALSE,
        "query physical monitor");
    const LONG originX = monitor.rcMonitor.left;
    const LONG originY = monitor.rcMonitor.top;
    helper.send(Action::Move, {originX + 40, originY + 180, originX + 640, originY + 480});
    auto lastRefresh = std::chrono::steady_clock::now();
    auto waitFor = [&](auto predicate) {
        return pumpUntil([&] {
            const auto now = std::chrono::steady_clock::now();
            if (tracker.dirty() || now - lastRefresh >= std::chrono::seconds(2)) {
                surfaces = &tracker.refresh();
                lastRefresh = now;
            }
            require(surfaces->size() <= snow::MaxSurfaces, "bounded surface count");
            for (const auto& surface : *surfaces) {
                require(surface.visible.size() <= 32, "bounded visible intervals");
                require(surface.bounds.width() <= static_cast<int>(snow::MaxSurfaceCells) * snow::CellWidth,
                    "bounded surface width");
            }
            return predicate();
        });
    };
    snow::Rect expected = physicalBounds(helper.window());
    require(waitFor([&] { return findBounds(*surfaces, expected) != nullptr; }), "new window becomes a surface");
    require(controller.notifications > 0, "real WinEvents notify the UI window");
    std::uint64_t identity = findBounds(*surfaces, expected)->id;
    require(identity != 0, "surface receives a generation identity");

    const snow::Rect initialBounds = expected;
    helper.send(Action::Move, {originX + 70, originY + 190, originX + 690, originY + 510});
    require(waitFor([&] {
        expected = physicalBounds(helper.window());
        const auto* surface = findBounds(*surfaces, expected);
        return expected != initialBounds && surface && surface->id == identity;
    }),
        "move and resize preserve generation and follow physical DWM bounds");

    const RECT occluder{expected.left + 120, expected.top - 70, expected.left + 320, expected.top + 70};
    helper.send(Action::Occlude, occluder);
    require(waitFor([&] {
        const auto* surface = findBounds(*surfaces, expected);
        if (!surface || surface->visible.empty()) return false;
        return std::none_of(surface->visible.begin(), surface->visible.end(), [&](snow::Span span) {
            return span.left < occluder.right && span.right > occluder.left;
        });
    }), "higher tool windows clip the supporting snow band");
    helper.send(Action::Unocclude);
    require(waitFor([&] {
        const auto* surface = findBounds(*surfaces, expected);
        return surface && surface->visible.size() == 1 && surface->visible.front() == snow::Span{expected.left, expected.right};
    }), "uncovering restores the complete visible edge");

    helper.send(Action::Minimize);
    require(waitFor([&] {
        return std::none_of(surfaces->begin(), surfaces->end(), [identity](const auto& surface) { return surface.id == identity; });
    }), "minimized window stops supporting snow");
    helper.send(Action::Restore);
    expected = physicalBounds(helper.window());
    require(waitFor([&] { return findBounds(*surfaces, expected) != nullptr; }), "restored window is rediscovered");
    identity = findBounds(*surfaces, expected)->id;

    const snow::Rect restoredBounds = expected;
    helper.send(Action::Move, {originX - 120, originY + 180, originX + 480, originY + 480});
    require(waitFor([&] {
        expected = physicalBounds(helper.window());
        const auto* surface = findBounds(*surfaces, expected);
        return expected != restoredBounds && surface && surface->id == identity;
    }),
        "partially off-monitor move retains signed desktop geometry");
    const auto* partiallyVisible = findBounds(*surfaces, expected);
    require(partiallyVisible && !partiallyVisible->visible.empty(), "off-monitor edge retains its visible section");

    // Leaving refresh unacknowledged must not accumulate one message per event.
    tracker.refresh();
    pumpUntil([] { return false; }, std::chrono::milliseconds(100));
    tracker.refresh();
    const unsigned beforeStorm = controller.notifications;
    for (LONG offset = 0; offset < 20; ++offset)
        helper.send(Action::Move, {originX + 40 + offset, originY + 180, originX + 640 + offset, originY + 480});
    require(pumpUntil([&] { return controller.notifications > beforeStorm; }), "event storm sends notification");
    pumpUntil([] { return false; }, std::chrono::milliseconds(200));
    require(controller.notifications == beforeStorm + 1 && tracker.dirty(), "event storm coalesces one pending notification");
    surfaces = &tracker.refresh();
    expected = physicalBounds(helper.window());
    const auto* beforeRecreate = findBounds(*surfaces, expected);
    require(beforeRecreate != nullptr, "storm reconciliation observes final geometry");
    identity = beforeRecreate->id;
    helper.send(Action::Recreate);
    require(waitFor([&] {
        const auto* surface = findBounds(*surfaces, expected);
        return surface && surface->id != identity;
    }), "destroy and immediate recreate cannot inherit old snow identity");

    identity = findBounds(*surfaces, expected)->id;
    helper.send(Action::Fullscreen, monitor.rcMonitor);
    expected = physicalBounds(helper.window());
    require(waitFor([&] {
        return findBounds(*surfaces, expected) == nullptr &&
            std::none_of(surfaces->begin(), surfaces->end(), [identity](const auto& surface) { return surface.id == identity; });
    }), "fullscreen window has no supporting surface");
    if (GetForegroundWindow() == helper.window()) {
        tracker.refresh();
        require(tracker.foregroundFullscreen(), "foreground fullscreen policy follows physical monitor bounds");
    } else {
        std::cout << "Foreground policy check skipped: desktop denied foreground activation.\n";
    }
    tracker.stop();
    require(!tracker.dirty() && tracker.refresh().empty() && !tracker.foregroundFullscreen(), "stop releases all tracking state");
    std::cout << "Window tracking lifecycle smoke passed.\n";
    return 0;
}
}

int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (argc == 3 && std::wstring(argv[1]) == L"--helper") return helperMain(argv[2]);
    try { return smokeMain(); }
    catch (const std::exception& error) { std::cerr << "Window tracking smoke failed: " << error.what() << '\n'; return 1; }
}
