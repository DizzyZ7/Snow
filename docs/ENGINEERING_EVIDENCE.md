# Engineering evidence — 2026-11-11

Baseline: f3eea7e, PR #1 draft, branch feat/windows-snow-overlay. Only open PR.
- Windows build run 43055212576: success; MSVC Release x64, process alive after 3s. No unit tests or memory data in baseline.
- Local host: Linux, GCC 14.2, no Windows GUI/SDK. Native tests execute in GitHub Actions, not on this host.
- Audited all seven repository files and nine feature-branch commits.
- Root causes: frame timer survives pause; weather calls resizeDesktop/recreates DIB; full virtual rectangle allocates monitor gaps; global ownership makes failure paths hard to verify; no model/tests for accumulation or window identity.
- Decision: fresh clone on existing feature branch, no separate worktree or duplicate PR. User explicitly authorized sequential implementation and GitHub pushes.
- Decision: isolated renderer/tracker implementation can proceed in parallel against scene.hpp; no shared-file edits. Root owns integration, model, CMake and CI.

Results are appended only after commands run. Desktop Win10/11, hardware GPU, multi-DPI, 2h/8h and competitor parity are NOT verified by CI startup.
