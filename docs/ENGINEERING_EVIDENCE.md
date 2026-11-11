# Engineering evidence — 2026-11-11

Baseline: f3eea7e, PR #1 draft, branch feat/windows-snow-overlay. Only open PR.
- Windows build run 43055212576: success; MSVC Release x64, process alive after 3s. No unit tests or memory data in baseline.
- Local host: Linux, GCC 14.2, no Windows GUI/SDK. Native tests execute in GitHub Actions, not on this host.
- Audited all seven repository files and nine feature-branch commits.
- Root causes: frame timer survives pause; weather calls resizeDesktop/recreates DIB; full virtual rectangle allocates monitor gaps; global ownership makes failure paths hard to verify; no model/tests for accumulation or window identity.
- Decision: fresh clone on existing feature branch, no separate worktree or duplicate PR. User explicitly authorized sequential implementation and GitHub pushes.
- Decision: isolated renderer/tracker implementation can proceed in parallel against scene.hpp; no shared-file edits. Root owns integration, model, CMake and CI.

Results are appended only after commands run. Desktop Win10/11, hardware GPU, multi-DPI, 2h/8h and competitor parity are NOT verified by CI startup.

## First integrated Windows run (992e25b)

- Run 43060355603: Release and Debug x64 compiled with /W4 /WX. Both executed all four CTests successfully: core, renderer GDI resources/lifecycle, real helper-process window lifecycle/occlusion/generation, application pause/resume/rebuild/settings/normal exit.
- Release portable ZIP, per-user Inno installer and SHA256 files were actually built; artifact 13872248933. Installation/uninstall not yet tested in that run.
- Linux run 43060355621: address/undefined/leak sanitizer CTest passed. Local GCC sanitizer run passed with `ASAN_OPTIONS=detect_leaks=0`; local LeakSanitizer cannot run under this host's ptrace.
- MSVC static analysis rejected two findings: C6262 (two large test models on stack), C6031 (unchecked CoCreateInstance). Tests now allocate their models on the heap; COM creation result is explicitly handled. No warnings disabled.
- MSVC ASan executables built but loader returned 0xc0000135: matching dynamic ASan runtime was absent from the test directory. CMake now stages the selected compiler's runtime; a new run must confirm this fix.
- Independent review reproduced saturated-scene omission of the explicit tool input plane. Regression starts from a 32768-item 8K scene; scene generation now reserves one slot for that plane. Core suite: 2344 assertions passed locally after the fix.
- GDI alpha crests and snowmen are constrained to the 96px tracker occlusion band and fully visible horizontal footprint; regression verified.
- Optional GPU DLLs are delay-loaded for GDI startup. No memory reduction is claimed without comparable active measurements.

### Initial resource sample — state unknown, not active-performance evidence

Five real 3-second samples on Windows Server 2022 / Hyper-V Video / 4 logical CPUs: private commit 6,922,240 bytes; Working Set 21,839,872; private Working Set 5,140,480; 2 GDI / 15 USER objects. GPU counters had no process instance and are unavailable, not zero. CPU samples were zero, but the app may have auto-paused behind the runner's fullscreen window. These values are therefore NOT normal-mode target verification and are NOT an optimization comparison. The sampler now records actual state/renderer/native submissions; explicit `--benchmark` disables only fullscreen auto-pause for reproducible CI workload.
