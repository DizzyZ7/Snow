# Window and taskbar tracking

`WindowTracker` is a Windows UI-thread component. Construct, start, refresh, stop,
and destroy it on the same thread, and keep that thread's message loop running.
There can be one started tracker per UI thread. `start()` requires a notification
window owned by that thread and a message in the `WM_APP` or registered-message
range. It returns false if those requirements or hook installation fail. The
notification has no payload.

Call `refresh()` once after the coalesced notification, and independently on the
application's two-second reconciliation timer. Do not call it from the animation
or fixed-step simulation loop. `dirty()` only reads a boolean; it performs no
window enumeration. `foregroundFullscreen()` reads the result of the last
refresh. Display changes, Explorer restart, and shell state changes missed by
WinEvents are recovered at the next reconciliation. The timer should continue
while snow animation is paused or suspended so lifecycle policy can recover.

The seven out-of-context `SetWinEventHook` registrations cover foreground,
move/resize, minimize, desktop switch, window create/destroy/show/hide/reorder,
state/location, and cloak/uncloak notifications. They use
`WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS`: no DLL is injected and no code
runs in other processes. Windows delivers callbacks on the registering UI
thread. The callback marks dirty, invalidates matching bounded identities on
window destruction, and posts at most one pending wakeup. It stores no event
queue and does not enumerate windows. Refresh acknowledges the wakeup, including
removing an older queued wakeup if the reconciliation timer ran first. A failed
`PostMessage` leaves dirty set; another event or the timer recovers it.

## Geometry and identities

Refresh temporarily selects per-monitor-v2 DPI awareness for its calling thread,
restoring the original context afterward. `DWMWA_EXTENDED_FRAME_BOUNDS` supplies
physical frame bounds without invisible resize borders. `GetWindowRect` is a
fallback under that DPI context. Bounds preserve signed desktop coordinates;
spans contain absolute physical x coordinates. Monitor rectangles clip spans,
including display gaps and windows partially outside a display. A supporting
edge above a display has no span on that display.

Each accepted window receives a nonzero generation, independent of its HWND.
Move and resize retain the generation. An observed destroy event, or a change of
the owning process or window thread, replaces it. Excluded or budget-evicted
windows lose their identity immediately on refresh, so restoring a minimized or
hidden window starts a new surface. No historical HWND map grows over time.
Windows can recycle HWNDs before a callback is delivered; the destroy callback
therefore invalidates by the event's HWND without inspecting the now-reusable
handle. A completely lost destroy event followed by reuse in the same process
and thread cannot be distinguished reliably with the public identity APIs; the
reconciliation path cannot manufacture an operating-system generation number.

Hidden, minimized, cloaked, desktop, off-virtual-desktop, and own-process windows
are excluded. COM `IVirtualDesktopManager` is best effort; a failed COM setup or
query falls back to visibility and independent DWM cloaking checks. Tool,
no-activate, and popup-menu windows do not receive snow but remain rectangular
occluders when visible. Regular dialogs may receive snow. A window covering its
nearest monitor within one physical pixel is treated as fullscreen; it occludes
lower surfaces and receives none. This conservative rule also treats a maximized
window covering an auto-hide monitor as fullscreen. Foreground suppression uses
the same rule, rather than process names or game-specific heuristics.

Both `Shell_TrayWnd` and `Shell_SecondaryTrayWnd` are recognized. Taskbars bypass
the ordinary tool-window exclusion. `ABM_GETAUTOHIDEBAREX` checks each monitor's
edges, with `ABM_GETSTATE` as a primary-taskbar fallback. Configured auto-hide bars
remain snow-free even while temporarily revealed, and any activation strip of
four pixels or less is excluded independently. Visible non-auto-hide taskbars
use their real top edge. No Explorer HWND is retained outside the bounded
snapshot/identity set, so Explorer restart does not require restarting Snow.

## Occlusion and bounds

The snapshot walks top-level windows from `GetTopWindow` through `GW_HWNDNEXT`.
It checks handles again after geometry queries, detects repeated accepted
handles, and caps traversal to avoid a destroyed/reordered-window loop. A desktop
snapshot is not atomic; another event or reconciliation resolves concurrent
changes.

Each surface begins with its monitor-visible top edge. Every higher visible
window whose rectangle intersects the band from `top - 96` through `top`
subtracts its horizontal extent. This deliberately conservative rectangular
approximation may hide snow that would have fit below a partially intersecting
window. It prevents a mound of height at most 96 pixels from being drawn across
a higher window. The simulation must not exceed that supported height without
changing the tracker band too. Rounded corners, custom window regions,
translucency, layered windows, shadows, and animated shell clipping are not
pixel-exact. Fully occluded accepted surfaces have an empty visible-span list;
their generation can remain stable until they are uncovered.

| Resource | Hard limit | Overflow behavior |
| --- | ---: | --- |
| Top-level handle visits per refresh | 4096 | Stop traversal; omit lower windows |
| Accepted visible window snapshots | 512 | Stop traversal; omit lower windows |
| Monitor rectangles | 32 | Omit additional displays |
| Output surfaces and identity records | 128 | Prefer higher windows in Z order |
| Width of one accepted surface | 2048 cells / 16384 pixels | Wider windows only occlude |
| Visible intervals per surface | 32 | Discard excess fragments; never bridge an occluded gap |
| Pending tracker wakeups | 1 | Merge subsequent events into dirty state |

The output and a bounded replacement vector may coexist during refresh, so a
reentrant read sees the preceding complete output. Removed output vectors and
identity records are released at refresh completion; `stop()` releases all owned
state and unhooks all seven registrations. The fixed snapshot arrays contain no
owned operating-system handles. Memory does not depend on the number of events,
previous windows, or process lifetime. These are storage bounds, not measured
performance or process-memory results.

## Native verification

`tests/window_tracker_smoke.cpp` launches the same executable with `--helper` to
create normal windows in another process. The production component has no test
flags or fake-window path. The test pumps real messages and exercises initial
discovery, own-process exclusion, DWM geometry, stable move/resize identity,
higher tool-window occlusion/uncovering, minimize/restore, a partly off-monitor
signed move, event-storm coalescing, destroy/immediate-recreate generations,
fullscreen exclusion, and cleanup. The foreground policy assertion runs when
Windows grants helper foreground activation; a denial is printed explicitly.
The test follows dirty notifications with a two-second reconciliation fallback.

Required link libraries are `user32`, `dwmapi`, `shell32`, `ole32`, and `uuid`.
Build the native smoke target as a console executable with `UNICODE`, `_UNICODE`,
`WIN32_LEAN_AND_MEAN`, `NOMINMAX`, and C++20. MSVC Release and Debug execution on
Windows 10/11 with an interactive desktop is required. The implementation was
authored in a Linux environment without a Windows SDK; native compile/run and
real taskbar/virtual-desktop behavior must be reported from Windows CI or manual
testing, not inferred from this source.

Manual coverage still includes Explorer restart, primary/secondary taskbars on
each edge, auto-hide reveal/hide, mixed-DPI negative-origin monitors, virtual
desktop switching, shaped/translucent windows, rapid HWND churn, and long-running
resource measurements.

Microsoft API references:

- [SetWinEventHook](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwineventhook)
- [UnhookWinEvent](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-unhookwinevent)
- [GetWindowRect and physical DWM frame bounds](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getwindowrect)
- [GetTopWindow](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-gettopwindow)
- [GetWindow traversal and handle lifetime caveats](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getwindow)
- [IVirtualDesktopManager](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nn-shobjidl_core-ivirtualdesktopmanager)
- [ABM_GETAUTOHIDEBAREX](https://learn.microsoft.com/en-us/windows/win32/shell/abm-getautohidebarex)
