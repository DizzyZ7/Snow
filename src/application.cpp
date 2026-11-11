#include "application.hpp"
#include "core.hpp"
#include "renderer.hpp"
#include "window_tracker.hpp"
#include "performance_monitor.hpp"
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wtsapi32.h>
#include <windowsx.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace snow {
namespace {
constexpr UINT TrayMessage = WM_APP + 1, TrackerMessage = WM_APP + 2;
constexpr UINT_PTR FrameTimer = 1, ReconcileTimer = 2, SmokeTimer = 3, TrackerTimer = 4;
constexpr int PauseId = 1001, ExitId = 1002, SettingsId = 1003, ClearId = 1004;
constexpr int WeatherId = 1100, ProfileId = 1200, ToolId = 1300;
constexpr int PauseHotkey = 1, ExitHotkey = 2, EscapeHotkey = 3;
constexpr int WeatherControl = 2001, ProfileControl = 2002, AccumControl = 2003,
    AutorunControl = 2004, ApplyControl = 2005, CountControl = 2010, SpeedControl = 2011,
    WindControl = 2012, SizeControl = 2013, RateControl = 2014, DepthControl = 2015, InteractionControl = 2016;
using Clock = std::chrono::steady_clock;
constexpr wchar_t OverlayClass[] = L"SnowOverlayV2";
constexpr wchar_t ControllerClass[] = L"SnowControllerV2";
constexpr wchar_t SettingsClass[] = L"SnowSettingsV2";
constexpr wchar_t RunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
Rect fromNative(RECT r) { return {r.left, r.top, r.right, r.bottom}; }
std::wstring executablePath() {
    std::wstring path(32768, L'\0');
    const DWORD count = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (!count || count >= path.size()) return {};
    path.resize(count); return path;
}
std::filesystem::path configPath() {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &raw))) return {};
    std::filesystem::path folder(raw); CoTaskMemFree(raw);
    return folder / L"Snow" / L"settings.ini";
}
bool autostartEnabled() {
    DWORD type = 0, size = 0;
    return RegGetValueW(HKEY_CURRENT_USER, RunKey, L"Snow", RRF_RT_REG_SZ, &type, nullptr, &size) == ERROR_SUCCESS;
}
bool setAutostart(bool enabled) {
    if (!enabled) {
        const auto result = RegDeleteKeyValueW(HKEY_CURRENT_USER, RunKey, L"Snow");
        return result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND;
    }
    const auto exe = executablePath(); if (exe.empty()) return false;
    const std::wstring command = L"\"" + exe + L"\"";
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, RunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return false;
    const auto result = RegSetValueExW(key, L"Snow", 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()), static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key); return result == ERROR_SUCCESS;
}
}
struct Application::Impl {
    struct Overlay {
        Impl* owner{};
        HWND hwnd{};
        Rect rect{};
        Renderer renderer;
        ~Overlay() { renderer.release(); if (hwnd) DestroyWindow(hwnd); }
    };
    HINSTANCE instance{};
    HWND controller{}, settingsWindow{};
    HANDLE mutex{};
    NOTIFYICONDATAW tray{};
    UINT taskbarMessage{};
    std::vector<std::unique_ptr<Overlay>> overlays;
    std::vector<DrawItem> scene;
    WindowTracker tracker;
    PerformanceMonitor performance;
    std::uint64_t previousIdle{}, previousTotal{};
    double systemLoad{};
    HPOWERNOTIFY displayNotification{};
    SnowSimulation simulation;
    Settings settings;
    RendererKind rendererKind{RendererKind::Gdi};
    std::filesystem::path path;
    Clock::time_point lastFrame{Clock::now()}, lastInput{Clock::now()};
    Tool tool{Tool::None};
    bool smoke{}, manualPause{}, sessionLocked{}, displayOff{}, powerSuspended{}, autoPause{}, battery{}, saver{}, running{}, frameTimer{}, writable{true}, shuttingDown{};
    bool pauseHotkey{}, exitHotkey{}, escapeHotkey{}, sessionNotifications{}, policyPaused{true};
    Rect desktopBounds{};
    unsigned smokeStage{};
    std::uint64_t smokeSteps{};
    int result{}, interval{33};

    Impl(HINSTANCE h, bool test, bool gpu) : instance(h), rendererKind(gpu ? RendererKind::D3D11 : RendererKind::Gdi), smoke(test) { overlays.reserve(16); scene.reserve(MaxDrawItems); }
    ~Impl() {
        shutdown();
        if (controller) { SetWindowLongPtrW(controller, GWLP_USERDATA, 0); DestroyWindow(controller); }
        if (mutex) CloseHandle(mutex);
        if (smoke && !path.empty()) { std::error_code ec; std::filesystem::remove_all(path.parent_path(), ec); }
    }
    bool paused() const noexcept { return manualPause || sessionLocked || displayOff || powerSuspended || autoPause; }
    void shutdown() {
        if (shuttingDown) return;
        shuttingDown = true; running = false;
        if (controller) {
            KillTimer(controller, FrameTimer); KillTimer(controller, ReconcileTimer); KillTimer(controller, SmokeTimer); KillTimer(controller, TrackerTimer);
            UnregisterHotKey(controller, PauseHotkey); UnregisterHotKey(controller, ExitHotkey); UnregisterHotKey(controller, EscapeHotkey);
            if (sessionNotifications) WTSUnRegisterSessionNotification(controller);
            if (displayNotification) { UnregisterPowerSettingNotification(displayNotification); displayNotification = nullptr; }
        }
        tracker.stop();
        if (settingsWindow) { DestroyWindow(settingsWindow); settingsWindow = nullptr; }
        overlays.clear();
        if (tray.hWnd) { Shell_NotifyIconW(NIM_DELETE, &tray); tray.hWnd = nullptr; }
    }
    void fail() { result = 1; if (controller) PostMessageW(controller, WM_CLOSE, 0, 0); }
    void load() {
        if (smoke) { path = std::filesystem::temp_directory_path() / (L"SnowSmoke-" + std::to_wstring(GetCurrentProcessId())) / L"settings.ini"; return; }
        path = configPath();
        if (path.empty()) { writable = false; return; }
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) { writable = !ec; return; }
        const auto size = std::filesystem::file_size(path, ec);
        if (ec || size > 8192) { writable = false; return; }
        std::ifstream input(path, std::ios::binary);
        if (!input) { writable = false; return; }
        std::string text(static_cast<std::size_t>(size), '\0');
        input.read(text.data(), static_cast<std::streamsize>(text.size()));
        if (!input && !text.empty()) { writable = false; return; }
        if (const auto parsed = parseSettings(text)) settings = *parsed;
        else writable = false; // Preserve invalid/future configurations instead of silently overwriting.
    }
    bool save() {
        if (!writable || path.empty()) return false;
        std::error_code ec; std::filesystem::create_directories(path.parent_path(), ec); if (ec) return false;
        auto temp = path; temp += L".tmp";
        const auto text = serializeSettings(settings);
        const HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        DWORD written = 0;
        const bool okay = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) != 0 &&
            written == text.size() && FlushFileBuffers(file) != 0;
        CloseHandle(file);
        if (!okay) { DeleteFileW(temp.c_str()); return false; }
        if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { DeleteFileW(temp.c_str()); return false; }
        return true;
    }
    void persist() {
        if (!save() && !smoke) balloon(L"Настройки не сохранены", L"Проверьте файл %LOCALAPPDATA%\\Snow\\settings.ini и права доступа. Snow продолжает работать.");
    }
    void balloon(const wchar_t* title, const wchar_t* text) {
        if (smoke) return;
        tray.uFlags = NIF_INFO;
        wcscpy_s(tray.szInfoTitle, title); wcscpy_s(tray.szInfo, text); tray.dwInfoFlags = NIIF_INFO;
        Shell_NotifyIconW(NIM_MODIFY, &tray);
        tray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    }
    bool addTray() {
        tray.cbSize = sizeof(tray); tray.hWnd = controller; tray.uID = 1;
        tray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP; tray.uCallbackMessage = TrayMessage;
        tray.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wcscpy_s(tray.szTip, L"Snow — снег на рабочем столе");
        if (!Shell_NotifyIconW(NIM_ADD, &tray)) return false;
        tray.uVersion = NOTIFYICON_VERSION_4; Shell_NotifyIconW(NIM_SETVERSION, &tray); return true;
    }
    static BOOL CALLBACK enumMonitor(HMONITOR, HDC, LPRECT rect, LPARAM data) {
        auto* monitors = reinterpret_cast<std::vector<Rect>*>(data);
        if (monitors->size() < 16 && rect->right > rect->left && rect->bottom > rect->top) monitors->push_back(fromNative(*rect));
        return TRUE;
    }
    bool rebuildOverlays() {
        std::vector<Rect> monitors; monitors.reserve(16);
        if (!EnumDisplayMonitors(nullptr, nullptr, enumMonitor, reinterpret_cast<LPARAM>(&monitors)) || monitors.empty()) return false;
        if (monitors.size() == overlays.size()) {
            bool same = true;
            for (std::size_t i = 0; i < monitors.size(); ++i) if (monitors[i] != overlays[i]->rect) same = false;
            if (same) return true;
        }
        setTool(Tool::None);
        // Release old monitor surfaces before allocating replacements to avoid topology-change peaks.
        overlays.clear(); Rect desktop = monitors.front();
        for (const auto rect : monitors) {
            desktop.left = std::min(desktop.left, rect.left); desktop.top = std::min(desktop.top, rect.top);
            desktop.right = std::max(desktop.right, rect.right); desktop.bottom = std::max(desktop.bottom, rect.bottom);
            auto ov = std::make_unique<Overlay>(); ov->owner = this; ov->rect = rect;
            const auto create = [&](RendererKind kind) {
                ov->hwnd = CreateWindowExW(Renderer::windowStyles(kind), OverlayClass, L"Snow Overlay", WS_POPUP,
                    rect.left, rect.top, rect.width(), rect.height(), nullptr, nullptr, instance, ov.get());
                return ov->hwnd && ov->renderer.initialize(ov->hwnd, rect, kind);
            };
            if (!create(rendererKind)) {
                ov->renderer.release();
                if (ov->hwnd) { DestroyWindow(ov->hwnd); ov->hwnd = nullptr; }
                if (rendererKind == RendererKind::Gdi || !create(RendererKind::Gdi)) return false;
                balloon(L"Включен GDI renderer", L"GPU renderer недоступен. Snow использует совместимый GDI режим.");
            }
            if (!SetWindowPos(ov->hwnd, HWND_TOPMOST, rect.left, rect.top, rect.width(), rect.height(), SWP_NOACTIVATE | (paused() ? 0 : SWP_SHOWWINDOW))) return false;
            overlays.push_back(std::move(ov));
        }
        desktopBounds = desktop; simulation.setBounds(desktop);
        return true;
    }
    void updatePolicy() {
        SYSTEM_POWER_STATUS power{};
        if (GetSystemPowerStatus(&power)) { battery = power.ACLineStatus == 0; saver = power.SystemStatusFlag != 0; }
        if (!smoke) autoPause = tracker.foregroundFullscreen();
        const bool stop = paused();
        if (stop != policyPaused) { simulation.setPaused(stop); lastFrame = Clock::now(); policyPaused = stop; }
        const int desired = performance.interval(settings.profile, battery, saver, systemLoad);
        if (frameTimer && (stop || interval != desired)) { KillTimer(controller, FrameTimer); frameTimer = false; }
        interval = desired;
        if (!stop && !frameTimer) { frameTimer = SetTimer(controller, FrameTimer, static_cast<UINT>(interval), nullptr) != 0; if (!frameTimer) fail(); }
        for (const auto& ov : overlays) ShowWindow(ov->hwnd, stop ? SW_HIDE : SW_SHOWNOACTIVATE);
        if (stop && tool != Tool::None) setTool(Tool::None);
    }
    bool draw() {
        simulation.buildScene(scene);
        // A 1/255-alpha input plane gives layered windows a hit region between
        // individual flakes only while the user explicitly activates a tool.
        if (tool != Tool::None && scene.size() < MaxDrawItems) scene.push_back({
            static_cast<float>(desktopBounds.left), static_cast<float>(desktopBounds.top),
            static_cast<float>(desktopBounds.width()), static_cast<float>(desktopBounds.height()),
            0, 0, 0, 1.f / 255.f, 0});
        for (const auto& ov : overlays) if (!ov->renderer.render(scene)) return false;
        return true;
    }
    void frame() {
        if (paused()) return;
        const auto now = Clock::now(); const double elapsed = std::chrono::duration<double>(now - lastFrame).count(); lastFrame = now;
        simulation.advance(elapsed);
        if (!draw()) {
            // Device loss is handled once at the application boundary, never by a retry loop.
            overlays.clear(); rendererKind = RendererKind::Gdi;
            if (!rebuildOverlays() || !draw()) fail();
        }
        performance.recordFrame(std::chrono::duration<double>(Clock::now() - now).count());
        const int desired = performance.interval(settings.profile, battery, saver, systemLoad);
        if (frameTimer && desired != interval) {
            KillTimer(controller, FrameTimer); interval = desired;
            frameTimer = SetTimer(controller, FrameTimer, static_cast<UINT>(interval), nullptr) != 0;
            if (!frameTimer) fail();
        }
        if (tool != Tool::None && now - lastInput > std::chrono::seconds(30)) setTool(Tool::None);
    }
    void reconcile() {
        FILETIME idle{}, kernel{}, user{};
        if (GetSystemTimes(&idle, &kernel, &user)) {
            const auto ticks = [](FILETIME value) { return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32) | value.dwLowDateTime; };
            const auto idleTicks = ticks(idle), totalTicks = ticks(kernel) + ticks(user);
            if (previousTotal && totalTicks > previousTotal && idleTicks >= previousIdle)
                systemLoad = std::clamp(1.0 - static_cast<double>(idleTicks - previousIdle) / static_cast<double>(totalTicks - previousTotal), 0.0, 1.0);
            previousIdle = idleTicks; previousTotal = totalTicks;
        }
        simulation.accumulation().reconcile(tracker.refresh());
        if (!rebuildOverlays()) { fail(); return; }
        updatePolicy();
    }
    void setTool(Tool value) {
        if (value != Tool::None && paused()) return;
        if (value != Tool::None && !escapeHotkey) {
            escapeHotkey = RegisterHotKey(controller, EscapeHotkey, MOD_NOREPEAT, VK_ESCAPE) != 0;
            if (!escapeHotkey) { balloon(L"Инструмент недоступен", L"Клавиша Escape занята другим приложением. Освободите ее и повторите."); return; }
        }
        if (value == Tool::None && escapeHotkey) { UnregisterHotKey(controller, EscapeHotkey); escapeHotkey = false; ReleaseCapture(); }
        tool = value; lastInput = Clock::now();
        for (const auto& ov : overlays) {
            auto styles = GetWindowLongPtrW(ov->hwnd, GWL_EXSTYLE);
            if (value == Tool::None) styles |= WS_EX_TRANSPARENT; else styles &= ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT);
            SetWindowLongPtrW(ov->hwnd, GWL_EXSTYLE, styles);
            SetWindowPos(ov->hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        }
        if (value != Tool::None) balloon(L"Режим инструментов", L"Зажмите левую кнопку для очистки. Снежок/снеговик создается кликом. Escape — выйти; выход также через трей.");
    }
    void interact(Overlay& ov, LPARAM lp, bool down) {
        if (tool == Tool::None) return;
        lastInput = Clock::now();
        const float x = static_cast<float>(ov.rect.left + GET_X_LPARAM(lp)), y = static_cast<float>(ov.rect.top + GET_Y_LPARAM(lp));
        if (down || tool == Tool::Mitten || tool == Tool::Shovel) simulation.useTool(tool, x, y);
    }
    void command(UINT id) {
        if (id == PauseId) { manualPause = !manualPause; updatePolicy(); }
        else if (id == ExitId) PostMessageW(controller, WM_CLOSE, 0, 0);
        else if (id == SettingsId) showSettings();
        else if (id == ClearId) { simulation.accumulation().reconcile({}); simulation.accumulation().reconcile(tracker.refresh()); }
        else if (id >= WeatherId && id <= WeatherId + 4) { settings.weather = static_cast<Weather>(id - WeatherId); simulation.setSettings(settings); persist(); }
        else if (id >= ProfileId && id <= ProfileId + 2) { settings.profile = static_cast<Profile>(id - ProfileId); simulation.setSettings(settings); updatePolicy(); persist(); }
        else if (id >= ToolId && id <= ToolId + 4) setTool(static_cast<Tool>(id - ToolId));
    }
    void menu() {
        HMENU root = CreatePopupMenu(), weather = CreatePopupMenu(), profile = CreatePopupMenu(), tools = CreatePopupMenu();
        if (!root || !weather || !profile || !tools) { if (root) DestroyMenu(root); if (weather) DestroyMenu(weather); if (profile) DestroyMenu(profile); if (tools) DestroyMenu(tools); return; }
        AppendMenuW(root, MF_STRING | (manualPause ? MF_CHECKED : 0), PauseId, L"Пауза / продолжить\tCtrl+Alt+F11");
        constexpr const wchar_t* weatherNames[]{L"Легкий снег", L"Обычный снег", L"Сильный снег", L"Метель", L"Пользовательский"};
        for (unsigned i = 0; i < 5; ++i) AppendMenuW(weather, MF_STRING | (static_cast<unsigned>(settings.weather) == i ? MF_CHECKED : 0), WeatherId + i, weatherNames[i]);
        constexpr const wchar_t* profileNames[]{L"Eco — 15 FPS", L"Balanced — 30 FPS", L"Quality — 60 FPS"};
        for (unsigned i = 0; i < 3; ++i) AppendMenuW(profile, MF_STRING | (static_cast<unsigned>(settings.profile) == i ? MF_CHECKED : 0), ProfileId + i, profileNames[i]);
        constexpr const wchar_t* toolNames[]{L"Обычный режим (Escape)", L"Варежка", L"Лопата", L"Снежок", L"Снеговик"};
        for (unsigned i = 0; i < 5; ++i) AppendMenuW(tools, MF_STRING | (static_cast<unsigned>(tool) == i ? MF_CHECKED : 0), ToolId + i, toolNames[i]);
        AppendMenuW(root, MF_POPUP, reinterpret_cast<UINT_PTR>(weather), L"Погода");
        AppendMenuW(root, MF_POPUP, reinterpret_cast<UINT_PTR>(profile), L"Производительность");
        AppendMenuW(root, MF_POPUP, reinterpret_cast<UINT_PTR>(tools), L"Инструменты");
        AppendMenuW(root, MF_STRING, SettingsId, L"Настройки…");
        AppendMenuW(root, MF_STRING, ClearId, L"Очистить накопленный снег");
        AppendMenuW(root, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(root, MF_STRING, ExitId, L"Выход\tCtrl+Alt+F12");
        POINT cursor{}; GetCursorPos(&cursor); SetForegroundWindow(controller);
        const UINT selected = static_cast<UINT>(TrackPopupMenu(root, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, controller, nullptr));
        DestroyMenu(root); PostMessageW(controller, WM_NULL, 0, 0); if (selected) command(selected);
    }
    HWND control(const wchar_t* klass, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id) {
        const int dpi = static_cast<int>(GetDpiForWindow(settingsWindow));
        const HWND hwnd = CreateWindowExW(0, klass, text, WS_CHILD | WS_VISIBLE | style,
            MulDiv(x, dpi, 96), MulDiv(y, dpi, 96), MulDiv(w, dpi, 96), MulDiv(h, dpi, 96),
            settingsWindow, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
        if (hwnd) SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        return hwnd;
    }
    void slider(const wchar_t* label, int y, int id, int min, int max, int value) {
        control(L"STATIC", label, 0, 20, y, 380, 22, 0);
        const HWND hwnd = control(TRACKBAR_CLASSW, L"", WS_TABSTOP | TBS_AUTOTICKS | TBS_TOOLTIPS, 16, y + 22, 382, 28, id);
        SendMessageW(hwnd, TBM_SETRANGEMIN, FALSE, min); SendMessageW(hwnd, TBM_SETRANGEMAX, FALSE, max);
        SendMessageW(hwnd, TBM_SETPOS, TRUE, value);
    }
    int position(int id) const { return static_cast<int>(SendDlgItemMessageW(settingsWindow, id, TBM_GETPOS, 0, 0)); }
    void showSettings() {
        if (settingsWindow) { ShowWindow(settingsWindow, SW_SHOWNORMAL); SetForegroundWindow(settingsWindow); return; }
        setTool(Tool::None);
        const int dpi = static_cast<int>(GetDpiForSystem());
        settingsWindow = CreateWindowExW(WS_EX_CONTROLPARENT, SettingsClass, L"Snow — настройки", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
            CW_USEDEFAULT, CW_USEDEFAULT, MulDiv(430, dpi, 96), MulDiv(660, dpi, 96), nullptr, nullptr, instance, this);
        if (!settingsWindow) return;
        control(L"STATIC", L"Погода", 0, 20, 16, 170, 22, 0);
        const HWND weather = control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, 20, 40, 180, 180, WeatherControl);
        for (const wchar_t* name : {L"Легкий снег", L"Обычный снег", L"Сильный снег", L"Метель", L"Пользовательский"}) SendMessageW(weather, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name));
        SendMessageW(weather, CB_SETCURSEL, static_cast<WPARAM>(settings.weather), 0);
        control(L"STATIC", L"Производительность", 0, 220, 16, 180, 22, 0);
        const HWND profile = control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, 220, 40, 180, 180, ProfileControl);
        for (const wchar_t* name : {L"Eco", L"Balanced", L"Quality"}) SendMessageW(profile, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name));
        SendMessageW(profile, CB_SETCURSEL, static_cast<WPARAM>(settings.profile), 0);
        slider(L"Число снежинок (пользовательский режим): 0–4096", 80, CountControl, 0, 4096, settings.particles);
        slider(L"Скорость падения: 10–400%", 135, SpeedControl, 10, 400, static_cast<int>(settings.speed * 100));
        slider(L"Ветер: −250…250 пикселей/с", 190, WindControl, -250, 250, static_cast<int>(settings.wind));
        slider(L"Размер снежинок: 50–300%", 245, SizeControl, 50, 300, static_cast<int>(settings.size * 100));
        slider(L"Скорость накопления: 10–500%", 300, RateControl, 10, 500, static_cast<int>(settings.accumulationRate * 100));
        slider(L"Высота сугробов: 4–96 пикселей", 355, DepthControl, 4, 96, static_cast<int>(settings.maxDepth));
        slider(L"Сила инструментов: 25–400%", 410, InteractionControl, 25, 400, static_cast<int>(settings.interaction * 100));
        const HWND accumulation = control(L"BUTTON", L"Накопление на окнах и панели задач", BS_AUTOCHECKBOX | WS_TABSTOP, 20, 475, 380, 25, AccumControl);
        SendMessageW(accumulation, BM_SETCHECK, settings.accumulation ? BST_CHECKED : BST_UNCHECKED, 0);
        const HWND autorun = control(L"BUTTON", L"Запускать вместе с Windows", BS_AUTOCHECKBOX | WS_TABSTOP, 20, 505, 380, 25, AutorunControl);
        SendMessageW(autorun, BM_SETCHECK, autostartEnabled() ? BST_CHECKED : BST_UNCHECKED, 0);
        control(L"STATIC", L"Ctrl+Alt+F11 — пауза   •   Ctrl+Alt+F12 — выход", 0, 20, 540, 380, 22, 0);
        control(L"BUTTON", L"Применить", BS_DEFPUSHBUTTON | WS_TABSTOP, 265, 570, 135, 32, ApplyControl);
        ShowWindow(settingsWindow, SW_SHOWNORMAL); SetForegroundWindow(settingsWindow);
    }
    void applySettings() {
        settings.weather = static_cast<Weather>(SendDlgItemMessageW(settingsWindow, WeatherControl, CB_GETCURSEL, 0, 0));
        settings.profile = static_cast<Profile>(SendDlgItemMessageW(settingsWindow, ProfileControl, CB_GETCURSEL, 0, 0));
        settings.particles = position(CountControl); settings.speed = static_cast<float>(position(SpeedControl)) / 100;
        settings.wind = static_cast<float>(position(WindControl)); settings.size = static_cast<float>(position(SizeControl)) / 100;
        settings.accumulationRate = static_cast<float>(position(RateControl)) / 100; settings.maxDepth = static_cast<float>(position(DepthControl));
        settings.interaction = static_cast<float>(position(InteractionControl)) / 100;
        settings.accumulation = SendDlgItemMessageW(settingsWindow, AccumControl, BM_GETCHECK, 0, 0) == BST_CHECKED;
        settings = validated(settings); simulation.setSettings(settings); updatePolicy(); persist();
        const bool autostart = SendDlgItemMessageW(settingsWindow, AutorunControl, BM_GETCHECK, 0, 0) == BST_CHECKED;
        if (!smoke && !setAutostart(autostart)) MessageBoxW(settingsWindow, L"Не удалось изменить автозапуск Windows.", L"Snow", MB_OK | MB_ICONERROR);
    }
    void smokeTick() {
        switch (smokeStage++) {
        case 0:
            if (simulation.steps() == 0 || overlays.empty()) { fail(); break; }
            manualPause = true; updatePolicy(); smokeSteps = simulation.steps();
            if (frameTimer) fail();
            break;
        case 1:
            if (simulation.steps() != smokeSteps) { fail(); break; }
            manualPause = false; updatePolicy();
            if (!frameTimer) fail();
            break;
        case 2:
            settings.weather = Weather::Blizzard; simulation.setSettings(settings);
            if (!save()) { fail(); break; }
            { std::ifstream input(path, std::ios::binary); std::string content((std::istreambuf_iterator<char>(input)), {});
              const auto restored = parseSettings(content); if (!restored || restored->weather != Weather::Blizzard) { fail(); break; } }
            showSettings(); if (!settingsWindow) { fail(); break; } applySettings();
            DestroyWindow(settingsWindow); settingsWindow = nullptr;
            overlays.clear(); if (!rebuildOverlays() || !draw()) fail();
            break;
        default:
            if (simulation.steps() <= smokeSteps) result = 1;
            PostMessageW(controller, WM_CLOSE, 0, 0); break;
        }
    }
    static LRESULT CALLBACK overlayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        auto* ov = reinterpret_cast<Overlay*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) { ov = static_cast<Overlay*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams); SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ov)); }
        if (!ov) return DefWindowProcW(hwnd, msg, wp, lp);
        if (msg == WM_NCHITTEST) return ov->owner->tool == Tool::None ? HTTRANSPARENT : HTCLIENT;
        if (msg == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
        if (msg == WM_ERASEBKGND) return 1;
        if (msg == WM_LBUTTONDOWN) { SetCapture(hwnd); ov->owner->interact(*ov, lp, true); return 0; }
        if (msg == WM_LBUTTONUP) { ReleaseCapture(); return 0; }
        if (msg == WM_MOUSEMOVE && (wp & MK_LBUTTON)) { ov->owner->interact(*ov, lp, false); return 0; }
        if (msg == WM_RBUTTONUP) { ov->owner->setTool(Tool::None); return 0; }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    static LRESULT CALLBACK settingsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) noexcept {
        try { return settingsDispatch(hwnd, msg, wp, lp); }
        catch (...) { auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA)); if (self) self->fail(); return 0; }
    }
    static LRESULT settingsDispatch(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) { self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams); SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self)); }
        if (self && msg == WM_COMMAND && LOWORD(wp) == ApplyControl) { self->applySettings(); return 0; }
        if (self && msg == WM_CLOSE) { self->settingsWindow = nullptr; DestroyWindow(hwnd); return 0; }
        if (msg == WM_DPICHANGED) { const auto* r = reinterpret_cast<RECT*>(lp); SetWindowPos(hwnd, nullptr, r->left, r->top, r->right-r->left, r->bottom-r->top, SWP_NOZORDER | SWP_NOACTIVATE); return 0; }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    static LRESULT CALLBACK controllerProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) noexcept {
        try { return controllerDispatch(hwnd, msg, wp, lp); }
        catch (...) { auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA)); if (self) self->fail(); return 0; }
    }
    static LRESULT controllerDispatch(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) { self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams); SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self)); }
        if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
        if (self->taskbarMessage && msg == self->taskbarMessage) { if (!self->addTray()) self->fail(); self->reconcile(); return 0; }
        if (msg == TrackerMessage) {
            // Throttle expensive reconciliation during native window drag storms.
            if (!SetTimer(hwnd, TrackerTimer, 50, nullptr)) self->fail();
            return 0;
        }
        switch (msg) {
        case TrayMessage:
            if (LOWORD(lp) == WM_CONTEXTMENU || LOWORD(lp) == NIN_SELECT || LOWORD(lp) == NIN_KEYSELECT) self->menu();
            return 0;
        case WM_TIMER:
            if (wp == FrameTimer) self->frame();
            else if (wp == ReconcileTimer) self->reconcile();
            else if (wp == SmokeTimer) self->smokeTick();
            else if (wp == TrackerTimer) { KillTimer(hwnd, TrackerTimer); self->simulation.accumulation().reconcile(self->tracker.refresh()); self->updatePolicy(); }
            return 0;
        case WM_COMMAND: self->command(LOWORD(wp)); return 0;
        case WM_HOTKEY:
            if (wp == PauseHotkey) self->command(PauseId);
            else if (wp == ExitHotkey) self->command(ExitId);
            else if (wp == EscapeHotkey) self->setTool(Tool::None);
            return 0;
        case WM_DISPLAYCHANGE:
        case WM_SETTINGCHANGE: self->reconcile(); return 0;
        case WM_POWERBROADCAST:
            if (wp == PBT_POWERSETTINGCHANGE && lp) {
                const auto* setting = reinterpret_cast<const POWERBROADCAST_SETTING*>(lp);
                if (IsEqualGUID(setting->PowerSetting, GUID_CONSOLE_DISPLAY_STATE) && setting->DataLength == sizeof(DWORD)) {
                    DWORD state{}; CopyMemory(&state, setting->Data, sizeof(state)); self->displayOff = state == 0;
                }
            }
            if (wp == PBT_APMSUSPEND) self->powerSuspended = true;
            else if (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND) self->powerSuspended = false;
            self->updatePolicy(); return TRUE;
        case WM_WTSSESSION_CHANGE:
            if (wp == WTS_SESSION_LOCK || wp == WTS_REMOTE_DISCONNECT || wp == WTS_CONSOLE_DISCONNECT) self->sessionLocked = true;
            else if (wp == WTS_SESSION_UNLOCK || wp == WTS_REMOTE_CONNECT || wp == WTS_CONSOLE_CONNECT) self->sessionLocked = false;
            self->updatePolicy(); return 0;
        case WM_QUERYENDSESSION: return TRUE;
        case WM_ENDSESSION: if (wp) self->shutdown(); return 0;
        case WM_CLOSE: self->shutdown(); DestroyWindow(hwnd); return 0;
        case WM_DESTROY: self->controller = nullptr; PostQuitMessage(self->result); return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    int run() {
        if (!smoke) {
            mutex = CreateMutexW(nullptr, FALSE, L"Local\\Snow.DesktopOverlay.Singleton");
            if (!mutex) return 1;
            if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;
        }
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        struct ComGuard { Impl* self; HRESULT hr; ~ComGuard() { self->shutdown(); if (SUCCEEDED(hr)) CoUninitialize(); } } guard{this, com};
        INITCOMMONCONTROLSEX common{sizeof(common), ICC_BAR_CLASSES}; if (!InitCommonControlsEx(&common)) return 1;
        load(); simulation.setSettings(settings);
        const auto registerClass = [&](const wchar_t* name, WNDPROC procedure, HBRUSH background) {
            WNDCLASSEXW cls{}; cls.cbSize = sizeof(cls); cls.hInstance = instance; cls.lpfnWndProc = procedure;
            cls.lpszClassName = name; cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); cls.hbrBackground = background;
            return RegisterClassExW(&cls) != 0;
        };
        if (!registerClass(ControllerClass, controllerProc, nullptr) || !registerClass(OverlayClass, overlayProc, nullptr) ||
            !registerClass(SettingsClass, settingsProc, reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1))) return 1;
        taskbarMessage = RegisterWindowMessageW(L"TaskbarCreated");
        controller = CreateWindowExW(WS_EX_TOOLWINDOW, ControllerClass, L"Snow", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, this);
        if (!controller) return 1;
        sessionNotifications = WTSRegisterSessionNotification(controller, NOTIFY_FOR_THIS_SESSION) != 0;
        displayNotification = RegisterPowerSettingNotification(controller, &GUID_CONSOLE_DISPLAY_STATE, DEVICE_NOTIFY_WINDOW_HANDLE);
        pauseHotkey = RegisterHotKey(controller, PauseHotkey, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_F11) != 0;
        exitHotkey = RegisterHotKey(controller, ExitHotkey, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_F12) != 0;
        if (!addTray() || !tracker.start(controller, TrackerMessage) || !rebuildOverlays()) return 1;
        simulation.accumulation().reconcile(tracker.refresh());
        if (!draw()) return 1;
        running = true; updatePolicy();
        if (!SetTimer(controller, ReconcileTimer, 2000, nullptr) || (smoke && !SetTimer(controller, SmokeTimer, 700, nullptr))) return 1;
        if (!smoke && (!pauseHotkey || !exitHotkey)) balloon(L"Горячая клавиша занята", L"Некоторые сочетания недоступны. Пауза и выход доступны в меню трея.");
        MSG message{}; BOOL next;
        while ((next = GetMessageW(&message, nullptr, 0, 0)) > 0) {
            if (settingsWindow && IsDialogMessageW(settingsWindow, &message)) continue;
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        return next == -1 ? 1 : static_cast<int>(message.wParam);
    }
};
Application::Application(HINSTANCE instance, bool smoke, bool gpu) : impl_(std::make_unique<Impl>(instance, smoke, gpu)) {}
Application::~Application() = default;
int Application::run() { return impl_->run(); }
}
