# Snow desktop implementation plan

Goal: extend the existing draft into a testable, bounded native desktop snow implementation.
Architecture: portable core supplies a bounded scene to per-monitor renderers; event-driven Windows tracking supplies surface geometry. Application owns lifecycle, timers, settings and explicit tools.
Tech stack: C++20, Win32/GDI, optional D3D11/DirectComposition, CMake/CTest, MSVC, PowerShell.
Spec: docs/superpowers/specs/2026-11-11-snow-desktop-design.md and the user's master prompt.

## Global constraints
Windows 10/11 x64; no heavy UI/runtime/dependencies, admin, injection or telemetry. Continue PR #1, preserve main. Never claim unexecuted GUI or performance checks passed.

## Review focus
Negative coordinates, HWND reuse, occluded edges, pause/resume catch-up, renderer failure during topology changes.

## Task 1: Portable model (root)
- [ ] Tests first: deterministic fixed-step particles, pause, weather, accumulation collision, movement, resize, hidden intervals, removal/reuse, bounded storage, snow clearing/collection, config validation/roundtrip.
- [ ] src/scene.hpp: shared Rect, Span, SurfaceSpec and DrawItem data only.
- [ ] src/core.hpp/cpp: Settings, HeightField, SnowSimulation and bounded scene generation. g++ -std=c++20 -Wall -Wextra -Wpedantic -Werror; sanitizer run.

## Task 2: Renderer (separate ownership)
- [ ] src/renderer.hpp/cpp, tests/renderer_smoke.cpp: RAII alpha GDI and opt-in D3D11/DirectComposition using scene.hpp. Failures propagate; bounded resources; one renderer per monitor. Native smoke test participates in Windows CI.

## Task 3: Window tracking (separate ownership)
- [ ] src/window_tracker.hpp/cpp, tests/window_tracker_smoke.cpp: coalesced out-of-context hooks, DWM/desktop checks, z-order clipping, taskbars, generation identities, bounded snapshots. Native lifecycle smoke test participates in Windows CI.

## Task 4: Application integration (root)
- [ ] Replace global lifecycle with Application ownership and per-monitor overlay instances; killed frame timer on pause, power/session/fullscreen policy, tray lifecycle, explicit tools/Escape and settings. Application smoke mode exits normally with a checked result.

## Task 5: Packaging/evidence (root)
- [ ] CMake portable tests and Windows executable, Release/Debug tests and MSVC analysis/ASan CI, portable ZIP, per-user installer, SHA256, reproducible memory sampler. Record measured results separately from manual checks.
- [ ] Review complete diff independently; resolve defects; push to existing branch, inspect actual CI results and update PR #1. Stable release stays gated on missing desktop/long-duration evidence.
