# Snow: native desktop environment

The user's master engineering prompt is the product specification. This iteration extends the existing draft PR #1; it does not claim release readiness. Windows 10/11 x64, C++20/Win32, no administrator, injection, telemetry, web runtime or database. Correctness and bounded process memory precede visual features.

## Decisions
- Keep GDI as the default baseline until Windows A/B measurements justify a renderer switch. Add an explicitly selectable D3D11/DirectComposition candidate, rather than claiming GPU means lower RAM. Render per physical monitor to avoid allocating gaps between displays.
- Separate portable deterministic simulation, compact height fields and settings from Windows tracking, rendering and application lifecycle. Use bounded contiguous buffers, an 8px height-field grid, maximum 128 surfaces and 4096 particles. No per-particle heap objects.
- Use out-of-context WinEvent notifications, coalesce events, and reconcile at most periodically. Exclude hidden/minimized/cloaked windows. Surface identity changes after destruction or process/thread changes; retained height fields use window-local coordinates.
- All geometry is in physical virtual-desktop pixels, including negative origins. Visible surface intervals are clipped by higher windows. Arbitrary shaped/translucent windows remain a documented approximation requiring manual testing.
- 120Hz fixed simulation steps with bounded catch-up; pause discards elapsed time. Frame timer is killed while paused or suspended. Weather interpolates instead of rebuilding surfaces/render targets.
- Explicit tools use a temporary mouse-input overlay, with Escape and tray exit. Normal mode is click-through; no global mouse hook. Snowballs/snowmen consume actual model snow, are bounded and belong to a surface.
- Versioned small text configuration, bounded validated values, atomic replacement. Autostart is an explicit HKCU opt-in.

## Evidence required
Portable regression tests and sanitizers; Windows Release/Debug compilation, unit tests and real lifecycle smoke checks in CI; resource samples labeled with environment/renderer. Windows 10/11 interactive desktop and 8-hour/multi-DPI testing remain mandatory before stable release. No performance targets or GUI behavior are marked passed from code inspection.
