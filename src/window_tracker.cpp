#include "window_tracker.hpp"
#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <iterator>

namespace snow {
namespace {
constexpr std::size_t SnapshotLimit = 512;
constexpr std::size_t WalkLimit = 4096;
constexpr std::size_t MonitorLimit = 32;
constexpr std::size_t SpanLimit = 32;
constexpr int SnowBandHeight = 96;
constexpr int AutoHideSliver = 4;
constexpr int MaximumSurfaceWidth = static_cast<int>(MaxSurfaceCells) * CellWidth;

Rect fromNative(const RECT& rect) noexcept {
    return {static_cast<int>(rect.left), static_cast<int>(rect.top),
        static_cast<int>(rect.right), static_cast<int>(rect.bottom)};
}

bool sameClass(const wchar_t* actual, const wchar_t* expected) noexcept {
    return std::wcscmp(actual, expected) == 0;
}

bool desktopClass(const wchar_t* name) noexcept {
    return sameClass(name, L"Progman") || sameClass(name, L"WorkerW") ||
        sameClass(name, L"#32769");
}

bool taskbarClass(const wchar_t* name) noexcept {
    return sameClass(name, L"Shell_TrayWnd") || sameClass(name, L"Shell_SecondaryTrayWnd");
}

bool physicalBounds(HWND window, Rect& result) noexcept {
    RECT rect{};
    // Extended frame bounds are physical pixels and exclude invisible resize borders.
    if (FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &rect,
            static_cast<DWORD>(sizeof(rect)))) && !GetWindowRect(window, &rect)) return false;
    result = fromNative(rect);
    return result.valid();
}

bool coversMonitor(Rect bounds, Rect monitor) noexcept {
    // A one-pixel tolerance handles rounding at a DPI boundary. A maximized window
    // covering an auto-hide monitor is intentionally treated as fullscreen too.
    return static_cast<std::int64_t>(bounds.left) <= static_cast<std::int64_t>(monitor.left) + 1 &&
        static_cast<std::int64_t>(bounds.top) <= static_cast<std::int64_t>(monitor.top) + 1 &&
        static_cast<std::int64_t>(bounds.right) >= static_cast<std::int64_t>(monitor.right) - 1 &&
        static_cast<std::int64_t>(bounds.bottom) >= static_cast<std::int64_t>(monitor.bottom) - 1;
}

bool autoHideTaskbar(HWND window, const RECT& monitor, bool primary) noexcept {
    APPBARDATA data{};
    data.cbSize = sizeof(data);
    data.hWnd = window;
    data.rc = monitor;
    for (UINT edge = ABE_LEFT; edge <= ABE_BOTTOM; ++edge) {
        data.uEdge = edge;
        if (reinterpret_cast<HWND>(SHAppBarMessage(ABM_GETAUTOHIDEBAREX, &data)) == window) return true;
    }
    // Some Explorer versions do not expose the primary bar through the extended
    // query. ABM_GETSTATE is a documented fallback for the primary taskbar.
    return primary && (SHAppBarMessage(ABM_GETSTATE, &data) & ABS_AUTOHIDE) != 0;
}

struct DpiScope {
    DPI_AWARENESS_CONTEXT previous{SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)};
    ~DpiScope() { if (previous) SetThreadDpiAwarenessContext(previous); }
};

struct Intervals {
    std::array<Span, SpanLimit> spans{};
    std::size_t count{};

    void onMonitors(Rect bounds, const std::array<Rect, MonitorLimit>& monitors, std::size_t monitorCount) noexcept {
        count = 0;
        for (std::size_t index = 0; index < monitorCount && count < spans.size(); ++index) {
            const Rect monitor = monitors[index];
            if (bounds.top < monitor.top || bounds.top >= monitor.bottom) continue;
            const Span span{std::max(bounds.left, monitor.left), std::min(bounds.right, monitor.right)};
            if (span.right > span.left) spans[count++] = span;
        }
        std::sort(spans.begin(), spans.begin() + static_cast<std::ptrdiff_t>(count), [](Span left, Span right) {
            return left.left < right.left;
        });
        std::size_t merged{};
        for (std::size_t index = 0; index < count; ++index) {
            if (merged != 0 && spans[index].left <= spans[merged - 1].right)
                spans[merged - 1].right = std::max(spans[merged - 1].right, spans[index].right);
            else spans[merged++] = spans[index];
        }
        count = merged;
    }

    void subtract(Rect occluder) noexcept {
        std::array<Span, SpanLimit> remaining{};
        std::size_t written{};
        auto append = [&](Span span) {
            // Excess fragments are discarded, never joined across an occluded gap.
            if (span.right > span.left && written < remaining.size()) remaining[written++] = span;
        };
        for (std::size_t index = 0; index < count; ++index) {
            const Span span = spans[index];
            if (occluder.right <= span.left || occluder.left >= span.right) append(span);
            else {
                append({span.left, std::min(span.right, occluder.left)});
                append({std::max(span.left, occluder.right), span.right});
            }
        }
        spans = remaining;
        count = written;
    }
};
}

struct WindowTracker::Impl {
    struct Snapshot {
        HWND window{};
        DWORD process{}, thread{};
        Rect bounds{};
        bool surface{}, taskbar{}, fullscreen{};
    };
    struct Identity {
        HWND window{};
        DWORD process{}, thread{};
        std::uint64_t generation{};
        bool invalid{};
    };
    static thread_local Impl* active;
    static constexpr std::array<std::array<DWORD, 2>, 7> EventRanges{{
        {{EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND}},
        {{EVENT_SYSTEM_MOVESIZESTART, EVENT_SYSTEM_MOVESIZEEND}},
        {{EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND}},
        {{EVENT_SYSTEM_DESKTOPSWITCH, EVENT_SYSTEM_DESKTOPSWITCH}},
        {{EVENT_OBJECT_CREATE, EVENT_OBJECT_REORDER}},
        {{EVENT_OBJECT_STATECHANGE, EVENT_OBJECT_LOCATIONCHANGE}},
        {{EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED}}
    }};
    HWND notificationWindow{};
    UINT dirtyMessage{};
    DWORD ownerThread{};
    std::array<HWINEVENTHOOK, EventRanges.size()> hooks{};
    IVirtualDesktopManager* virtualDesktop{};
    bool comInitialized{}, running{}, isDirty{}, notificationPosted{}, fullscreen{}, refreshing{};
    std::array<Snapshot, SnapshotLimit> snapshots{};
    std::size_t snapshotCount{};
    std::array<Identity, MaxSurfaces> identities{};
    std::size_t identityCount{};
    std::uint64_t nextGeneration{1};
    std::array<Rect, MonitorLimit> monitors{};
    std::size_t monitorCount{};
    std::vector<SurfaceSpec> surfaces;

    void markDirty() noexcept {
        isDirty = true;
        if (!notificationPosted && notificationWindow &&
            PostMessageW(notificationWindow, dirtyMessage, 0, 0)) notificationPosted = true;
    }

    static void CALLBACK eventCallback(HWINEVENTHOOK hook, DWORD event, HWND window,
        LONG object, LONG child, DWORD, DWORD) noexcept {
        Impl* self = active;
        if (!self || !self->running ||
            std::find(self->hooks.begin(), self->hooks.end(), hook) == self->hooks.end()) return;
        const bool systemEvent = event < EVENT_OBJECT_CREATE;
        const bool windowEvent = object == OBJID_WINDOW && child == CHILDID_SELF;
        // Desktop reorder events may use OBJID_CLIENT; they still change occlusion.
        if (!systemEvent && event != EVENT_OBJECT_REORDER && !windowEvent) return;
        if (event == EVENT_OBJECT_DESTROY && windowEvent) {
            // Do not query a destroyed HWND: it may already identify another window.
            for (std::size_t index = 0; index < self->identityCount; ++index)
                if (self->identities[index].window == window) self->identities[index].invalid = true;
        }
        self->markDirty();
    }

    bool start(HWND window, UINT message) {
        const DWORD thread = GetCurrentThreadId();
        DWORD process{};
        if (!window || !IsWindow(window) || GetWindowThreadProcessId(window, &process) != thread ||
            process != GetCurrentProcessId() || message < WM_APP || message > 0xFFFFU ||
            (active && active != this) || (ownerThread != 0 && ownerThread != thread)) return false;
        stop();
        notificationWindow = window;
        dirtyMessage = message;
        ownerThread = thread;
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        comInitialized = SUCCEEDED(initialized);
        // RPC_E_CHANGED_MODE means the caller already owns a different apartment.
        // CoCreateInstance remains best effort; cloaking is independently checked.
        if (comInitialized || initialized == RPC_E_CHANGED_MODE)
            CoCreateInstance(CLSID_VirtualDesktopManager, nullptr, CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&virtualDesktop));
        active = this;
        running = true;
        for (std::size_t index = 0; index < hooks.size(); ++index) {
            hooks[index] = SetWinEventHook(EventRanges[index][0], EventRanges[index][1], nullptr,
                eventCallback, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
            if (!hooks[index]) { stop(); return false; }
        }
        markDirty();
        return true;
    }

    void stop() noexcept {
        assert(ownerThread == 0 || ownerThread == GetCurrentThreadId());
        running = false;
        if (active == this) active = nullptr;
        for (auto& hook : hooks) {
            if (hook) UnhookWinEvent(hook);
            hook = nullptr;
        }
        if (notificationWindow && notificationPosted) {
            MSG message{};
            PeekMessageW(&message, notificationWindow, dirtyMessage, dirtyMessage, PM_REMOVE);
        }
        if (virtualDesktop) { virtualDesktop->Release(); virtualDesktop = nullptr; }
        if (comInitialized) CoUninitialize();
        comInitialized = false;
        notificationWindow = nullptr;
        dirtyMessage = 0;
        ownerThread = 0;
        isDirty = notificationPosted = fullscreen = false;
        snapshotCount = identityCount = monitorCount = 0;
        identities.fill({});
        snapshots.fill({});
        monitors.fill({});
        std::vector<SurfaceSpec>().swap(surfaces);
    }

    bool readWindow(HWND window, Snapshot& result) const noexcept {
        DWORD process{};
        const DWORD thread = GetWindowThreadProcessId(window, &process);
        if (!thread || process == GetCurrentProcessId() || !IsWindowVisible(window) || IsIconic(window)) return false;
        if ((static_cast<ULONG_PTR>(GetWindowLongPtrW(window, GWL_STYLE)) & WS_CHILD) != 0) return false;
        DWORD cloaked{};
        if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked,
                static_cast<DWORD>(sizeof(cloaked)))) && cloaked != 0) return false;
        if (virtualDesktop) {
            BOOL current = TRUE;
            if (SUCCEEDED(virtualDesktop->IsWindowOnCurrentVirtualDesktop(window, &current)) && !current) return false;
        }
        wchar_t className[256]{};
        if (!GetClassNameW(window, className, static_cast<int>(std::size(className))) || desktopClass(className)) return false;
        Rect bounds{};
        if (!physicalBounds(window, bounds)) return false;
        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor)) return false;
        const bool taskbar = taskbarClass(className);
        const bool full = !taskbar && coversMonitor(bounds, fromNative(monitor.rcMonitor));
        const auto exStyle = static_cast<ULONG_PTR>(GetWindowLongPtrW(window, GWL_EXSTYLE));
        const std::int64_t width = static_cast<std::int64_t>(bounds.right) - bounds.left;
        bool surface = !full && (taskbar || (exStyle & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) == 0) &&
            !sameClass(className, L"#32768") && width <= MaximumSurfaceWidth;
        if (taskbar) {
            const Rect screen = fromNative(monitor.rcMonitor);
            const int visibleWidth = std::max(0, std::min(bounds.right, screen.right) - std::max(bounds.left, screen.left));
            const int visibleHeight = std::max(0, std::min(bounds.bottom, screen.bottom) - std::max(bounds.top, screen.top));
            // Hidden taskbars generally retain a 1-2 pixel activation strip. Keep
            // their real geometry as an occluder, never as a snow-bearing edge.
            // Keep auto-hide bars snow-free even while temporarily revealed. This
            // also handles shells whose DWM rectangle omits the hide animation.
            if (std::min(visibleWidth, visibleHeight) <= AutoHideSliver ||
                autoHideTaskbar(window, monitor.rcMonitor, sameClass(className, L"Shell_TrayWnd"))) surface = false;
        }
        DWORD finalProcess{};
        if (GetWindowThreadProcessId(window, &finalProcess) != thread || finalProcess != process) return false;
        result = {window, process, thread, bounds, surface, taskbar, full};
        return true;
    }

    static BOOL CALLBACK monitorCallback(HMONITOR, HDC, LPRECT rect, LPARAM context) noexcept {
        auto* self = reinterpret_cast<Impl*>(context);
        if (self->monitorCount < self->monitors.size()) self->monitors[self->monitorCount++] = fromNative(*rect);
        return self->monitorCount < self->monitors.size() ? TRUE : FALSE;
    }

    void takeSnapshot() noexcept {
        snapshotCount = monitorCount = 0;
        EnumDisplayMonitors(nullptr, nullptr, monitorCallback, reinterpret_cast<LPARAM>(this));
        HWND window = GetTopWindow(nullptr);
        // Walk explicitly in Z order, with hard bounds and duplicate detection.
        // Revalidate each handle: applications can destroy/reorder windows here.
        for (std::size_t visited = 0; window && visited < WalkLimit && snapshotCount < snapshots.size(); ++visited) {
            bool duplicate{};
            for (std::size_t index = 0; index < snapshotCount; ++index)
                if (snapshots[index].window == window) { duplicate = true; break; }
            if (duplicate) break;
            const HWND next = GetWindow(window, GW_HWNDNEXT);
            Snapshot snapshot{};
            if (readWindow(window, snapshot)) snapshots[snapshotCount++] = snapshot;
            if (next == window) break;
            window = next;
        }
        Snapshot foreground{};
        fullscreen = readWindow(GetForegroundWindow(), foreground) && foreground.fullscreen;
    }

    std::uint64_t generationFor(const Snapshot& snapshot) noexcept {
        Identity* identity{};
        for (std::size_t index = 0; index < identityCount; ++index) {
            if (identities[index].window == snapshot.window) { identity = &identities[index]; break; }
        }
        if (!identity) {
            if (identityCount == identities.size()) return 0;
            identity = &identities[identityCount++];
        }
        if (identity->invalid || identity->generation == 0 || identity->process != snapshot.process || identity->thread != snapshot.thread) {
            *identity = {snapshot.window, snapshot.process, snapshot.thread, nextGeneration++, false};
            if (nextGeneration == 0) ++nextGeneration;
        }
        return identity->generation;
    }

    const std::vector<SurfaceSpec>& refresh() {
        if (!running) return surfaces;
        assert(ownerThread == GetCurrentThreadId());
        if (refreshing) { markDirty(); return surfaces; }
        struct RefreshScope { bool& value; explicit RefreshScope(bool& flag) : value(flag) { value = true; } ~RefreshScope() { value = false; } } guard(refreshing);
        // A reconciliation timer may acknowledge dirty state before its posted
        // message is dispatched. Remove that stale wakeup before allowing another.
        if (notificationPosted) {
            MSG message{};
            PeekMessageW(&message, notificationWindow, dirtyMessage, dirtyMessage, PM_REMOVE);
        }
        notificationPosted = isDirty = false;
        DpiScope dpi;
        takeSnapshot();
        std::vector<SurfaceSpec> next;
        next.reserve(MaxSurfaces);
        std::array<std::size_t, MaxSurfaces> selected{};
        for (std::size_t index = 0; index < snapshotCount && next.size() < MaxSurfaces; ++index) {
            const Snapshot snapshot = snapshots[index];
            if (!snapshot.surface) continue;
            Intervals visible;
            visible.onMonitors(snapshot.bounds, monitors, monitorCount);
            if (visible.count == 0) continue;
            const auto bandTop = static_cast<std::int64_t>(snapshot.bounds.top) - SnowBandHeight;
            const auto bandBottom = static_cast<std::int64_t>(snapshot.bounds.top) + 1;
            for (std::size_t above = 0; above < index && visible.count != 0; ++above) {
                const Rect occluder = snapshots[above].bounds;
                if (static_cast<std::int64_t>(occluder.bottom) > bandTop &&
                    static_cast<std::int64_t>(occluder.top) < bandBottom) visible.subtract(occluder);
            }
            SurfaceSpec surface{0, snapshot.bounds, snapshot.taskbar, {}};
            surface.visible.assign(visible.spans.begin(), visible.spans.begin() + static_cast<std::ptrdiff_t>(visible.count));
            selected[next.size()] = index;
            next.push_back(std::move(surface));
        }
        // Admit only the selected bounded output. If the previous 128 surfaces
        // remain somewhere in Z order, they must not starve new higher windows.
        std::size_t kept{};
        for (std::size_t index = 0; index < identityCount; ++index) {
            Identity identity = identities[index];
            bool present{};
            for (std::size_t result = 0; result < next.size(); ++result)
                if (snapshots[selected[result]].window == identity.window) { present = true; break; }
            if (present) identities[kept++] = identity;
        }
        std::fill(identities.begin() + static_cast<std::ptrdiff_t>(kept), identities.end(), Identity{});
        identityCount = kept;
        for (std::size_t index = 0; index < next.size(); ++index)
            next[index].id = generationFor(snapshots[selected[index]]);
        surfaces.swap(next);
        return surfaces;
    }
};

thread_local WindowTracker::Impl* WindowTracker::Impl::active = nullptr;

WindowTracker::WindowTracker() : impl_(std::make_unique<Impl>()) {}
WindowTracker::~WindowTracker() { impl_->stop(); }
bool WindowTracker::start(HWND window, UINT message) { return impl_->start(window, message); }
void WindowTracker::stop() noexcept { impl_->stop(); }
bool WindowTracker::dirty() const noexcept { return impl_->isDirty; }
const std::vector<SurfaceSpec>& WindowTracker::refresh() { return impl_->refresh(); }
bool WindowTracker::foregroundFullscreen() const noexcept { return impl_->fullscreen; }
}
