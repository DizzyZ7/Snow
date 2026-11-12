#pragma once
#include <windows.h>
#include <memory>
#include <vector>
#include "scene.hpp"

namespace snow {
// Construct, start, refresh, stop and destroy on the same message-pumping UI thread.
// One started tracker is supported on each UI thread. Notification messages carry
// no payload; the receiver calls refresh() to acknowledge and rebuild the snapshot.
class WindowTracker {
public:
    WindowTracker();
    ~WindowTracker();
    WindowTracker(const WindowTracker&) = delete;
    WindowTracker& operator=(const WindowTracker&) = delete;

    [[nodiscard]] bool start(HWND notificationWindow, UINT dirtyMessage);
    void stop() noexcept;
    [[nodiscard]] bool dirty() const noexcept;
    const std::vector<SurfaceSpec>& refresh();
    [[nodiscard]] bool foregroundFullscreen() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
