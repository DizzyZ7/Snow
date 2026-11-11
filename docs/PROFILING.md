# Local profiling and release artifacts

Run these commands in a normal, interactive Windows 10/11 x64 user session using
Windows PowerShell 5.1 or PowerShell 7. No administrator session is required. The
scripts write local files and do not upload data. Linux source inspection and
hosted CI cannot establish real desktop, GPU, monitor, or long-duration results.

## Build and package

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
.\tools\Package-Snow.ps1 -Exe .\build\Release\Snow.exe -Version 0.2.0 -OutputDirectory .\dist -RequireInstaller
```

Install Inno Setup 6.3+ to produce the installer. `ISCC.exe` is discovered on PATH
or under the standard Inno Setup 6 installation directories; `-IsccPath` accepts
an explicit compiler path. Without `-RequireInstaller`, a missing compiler
produces the portable artifacts and an explicit `not_built` installer status.
With that switch, a missing compiler or failed compilation fails the command.
Existing artifacts are not overwritten: use a clean output directory per build.

The output contains `Snow-0.2.0-windows-x64.zip`, release notes, packaging metadata,
`SHA256SUMS.txt`, and, when built, `Snow-0.2.0-windows-x64-setup.exe`. The ZIP
contains a named folder with `Snow.exe`, `README.md`, and `RELEASE-NOTES.txt`.
The script checks the executable's AMD64 PE header, but packaging is not an
installation test or a performance verdict. SHA256 checks integrity; it is not a
publisher signature. Neither the script nor the installer publishes a release.

The installer uses `%LOCALAPPDATA%\Programs\Snow`, requires no elevation, and
offers unchecked desktop/Start menu shortcuts and launch choices. It does not
enable autostart; users can explicitly enable it in Snow settings. The static
MSVC runtime is selected by the Release CMake configuration. The installer does
not download or install a runtime. Uninstall retains preferences by default;
an interactive, default-No prompt offers removal of `settings.ini`. Silent
uninstall retains preferences. An autostart entry is removed only when its
command exactly names the executable being uninstalled.

Example Windows CI packaging step after building and testing:

```yaml
- name: Package portable build and per-user installer
  shell: pwsh
  run: ./tools/Package-Snow.ps1 -Exe ./build/Release/Snow.exe -Version 0.2.0 -OutputDirectory ./dist -RequireInstaller
- name: Upload release candidate artifacts
  uses: actions/upload-artifact@v4
  with:
    name: Snow-0.2.0-windows-x64
    path: dist/*
    if-no-files-found: error
```

The runner must already have Inno Setup available. A green packaging step proves
artifact creation; manually install and uninstall as a standard user before
accepting the installer. Test retained preferences, opt-in deletion, opt-in
shortcuts, autostart cleanup, upgrade, and silent-install behavior separately.

To check generated checksums after moving/downloading all files into one folder:

```powershell
Get-Content .\dist\SHA256SUMS.txt | ForEach-Object {
    if ($_ -notmatch '^([0-9a-f]{64})  (.+)$') { throw 'Invalid checksum line' }
    $expected = $Matches[1]
    $file = Join-Path .\dist $Matches[2]
    if ((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -ne $expected) {
        throw "Checksum mismatch: $file"
    }
}
```

## Reproducible profiles

Close existing Snow instances first: Snow permits one normal instance per
session. Use the same executable hash, settings, monitor arrangement, DPI,
power mode, foreground workload, and GPU driver for renderer comparisons.
Record Windows build, CPU model, RAM, GPU/driver, refresh rates, physical
resolutions, scaling, session type, and whether the machine is on battery.
Record settings before each run; starting Snow loads saved settings. Keep other
workloads stable and capture any tray notification that reports GDI fallback.
`RendererRequested` records the command-line request, not proof of the actual
renderer: D3D11 may fall back to GDI. The metadata makes this limit explicit.

These separate runs start a fresh process and sample every five seconds after
the initial CPU baseline is read. Startup before that baseline is not measured.
Run both renderers on the same machine, repeating each comparison
at least three times. Each profile launches its own process, samples only that
process identity, then requests graceful exit. Commands do not manufacture
results when a counter or process is unavailable.

| Profile | DurationSeconds | Purpose |
| --- | ---: | --- |
| 5 minutes | 300 | Initial resource trend and counter availability |
| 30 minutes | 1800 | Settling and normal desktop workload |
| 2 hours | 7200 | Repeated window/display interactions |
| 8 hours | 28800 | Soak test, resource growth, lock/resume cycles |

```powershell
.\tools\Measure-Snow.ps1 -Exe .\build\Release\Snow.exe -Renderer gdi -DurationSeconds 300 -OutputPath .\profiles\gdi-5m.csv
.\tools\Measure-Snow.ps1 -Exe .\build\Release\Snow.exe -Renderer gdi -DurationSeconds 1800 -OutputPath .\profiles\gdi-30m.csv
.\tools\Measure-Snow.ps1 -Exe .\build\Release\Snow.exe -Renderer gdi -DurationSeconds 7200 -OutputPath .\profiles\gdi-2h.csv
.\tools\Measure-Snow.ps1 -Exe .\build\Release\Snow.exe -Renderer gdi -DurationSeconds 28800 -OutputPath .\profiles\gdi-8h.csv

.\tools\Measure-Snow.ps1 -Exe .\build\Release\Snow.exe -Renderer d3d11 -DurationSeconds 300 -OutputPath .\profiles\d3d11-5m.csv
.\tools\Measure-Snow.ps1 -Exe .\build\Release\Snow.exe -Renderer d3d11 -DurationSeconds 1800 -OutputPath .\profiles\d3d11-30m.csv
.\tools\Measure-Snow.ps1 -Exe .\build\Release\Snow.exe -Renderer d3d11 -DurationSeconds 7200 -OutputPath .\profiles\d3d11-2h.csv
.\tools\Measure-Snow.ps1 -Exe .\build\Release\Snow.exe -Renderer d3d11 -DurationSeconds 28800 -OutputPath .\profiles\d3d11-8h.csv
```

Rename output paths for each repetition; existing CSV/metadata files cause an
error. The default duration is 300 seconds, the default renderer is `gdi`, and
`-IntervalSeconds` accepts 1 through 60. Short intervals increase observer cost.
The first row follows one real interval; there is no synthetic zero-CPU startup
row. Sampling and CIM queries add overhead, so actual timestamps and intervals
are recorded and missed deadlines are skipped. Windows sleep/resume and slow
providers can create gaps; do not interpret these gaps as continuous samples.
Total wall-clock duration can exceed the requested duration by collection and
graceful shutdown overhead. The `*.metadata.json` companion is always
attempted, including failures; it records run outcome, environment, executable
hash, sample count, requested interval and elapsed sampling time.

To attach without changing or terminating an existing process:

```powershell
.\tools\Measure-Snow.ps1 -ProcessId 1234 -Renderer gdi -DurationSeconds 300 -OutputPath .\profiles\attached-5m.csv
```

The renderer label for an attached process is supplied by the caller. In launch
mode, cleanup sends `WM_CLOSE` only to a `SnowControllerV2` window belonging to
the process the script started and waits up to ten seconds for normal exit. A
shutdown failure is reported as an error, with the still-running PID; no silent
force-kill occurs. `Ctrl+C` attempts the same cleanup through `finally`. Closing
or forcibly terminating PowerShell itself can prevent cleanup. Use the tray
menu or `Ctrl+Alt+F12` if that happens. Attach mode always leaves the process
under the user's control. Early target exit fails the profile and keeps any
real rows already written.

## Metric definitions

All memory columns are **bytes**, CPU is a percentage, and GDI/USER columns are
object counts. An unavailable numeric value is an empty CSV field/JSON null,
never a substitute zero. A reported zero is a real counter value. Status/error
columns distinguish absent instances, missing properties and query failures.

| CSV field | Source and meaning |
| --- | --- |
| `CpuPercentOfMachine` | Delta of `Process.TotalProcessorTime`, divided by actual monotonic interval and logical processor count, times 100. A fully occupied logical CPU is approximately `100 / LogicalProcessors` percent. |
| `CpuTotalSeconds` | Cumulative user plus kernel process CPU time at the sampling point. |
| `PrivateBytes` | `Process.PrivateMemorySize64`: process private committed memory, including nonresident pages. |
| `CommitSizeBytes` | The same private-commit value as `PrivateBytes`, provided with its Task Manager label. These columns are aliases and must never be added together. It is not virtual address-space size or system-wide commit. |
| `WorkingSetBytes` | `Process.WorkingSet64`: resident process pages, including private and shareable pages. |
| `PrivateWorkingSetBytes` | `Win32_PerfRawData_PerfProc_Process.WorkingSetPrivate`, selected by exact `IDProcess`: resident private pages only. Unavailable providers/properties remain empty. |
| `GdiObjects`, `UserObjects` | `GetGuiResources` for the sampled process handle. Errors remain empty; zero without a Win32 error is valid. |
| `GpuDedicatedBytes`, `GpuSharedBytes` | `DedicatedUsage` and `SharedUsage` from `Win32_PerfFormattedData_GPUPerformanceCounters_GPUProcessMemory`, summed over all adapter instances whose name starts with the exact `pid_<PID>_` prefix. No matching process instance produces empty fields, including in GDI-only or unsupported-driver sessions. |
| `IntervalSeconds`, `ElapsedSeconds` | Actual monotonic CPU measurement interval and elapsed time, not requested nominal intervals. |
| `SampleCollectionMilliseconds` | Time taken to read one row, including counter queries. Useful for identifying observer overhead. |

The metadata records whether logical processor count came from
`Win32_ComputerSystem` or the count visible to PowerShell. The independent
providers are not sampled atomically. GPU shared memory and process resident
memory can overlap; do not add process RAM and GPU bytes to claim a total RAM
footprint. Per-process GPU memory may also include allocations shared by other
processes; summing process totals is not adapter-wide usage. Some Windows 10
GPU process counters have a documented overcount issue: use WPR/WPA to validate
suspected GPU leaks and retain the raw measurements with OS/driver details.
The sampler does **not** measure GPU utilization, frame time, energy use,
system-wide committed bytes, or end-to-end visual smoothness. Use WPR/WPA or a
graphics profiler for those; a memory column cannot establish them.

## Controlled scenarios and reporting

Keep an event log with UTC times next to the CSV. For 30-minute, 2-hour and
8-hour runs, perform the same planned sequence for each renderer: move and
resize windows, maximize/minimize/close them, use Win+D and virtual desktops,
switch weather, clear accumulated snow, use each tool, pause/resume, and test
fullscreen transitions. On suitable hardware include mixed DPI, negative
monitor origins, monitor hotplug, session lock/unlock, sleep/resume, battery
and power-saver changes. Record missed steps and automatic pauses explicitly.

For a controlled paused comparison, attach or run a profile, note the exact
UTC time, press `Ctrl+Alt+F11` to pause, leave it paused for a fixed interval,
then press again to resume. Saved settings can affect background work, so keep
them identical. CPU while paused must be judged over those marked intervals;
the process still services tray/window/session events. Do not call a low GPU
memory value evidence of a paused timer or zero CPU work.

Report sample count, missing metric count, first/last values, median/p95 CPU,
peak private commit/working set/GPU values, and slopes over comparable settled
intervals. Examine GDI/USER counts across repeated rebuilds and verify they
return to a repeatable baseline. Retain anomalous runs rather than replacing
their values. Distinguish an observed growth trend from a confirmed leak and
document the reproduction before drawing a conclusion. Do not turn arbitrary
minimum/maximum values from a hosted runner into a desktop pass gate.

No 5-minute, 30-minute, 2-hour, or 8-hour Windows measurements are supplied by
this document. Mark a profile passed only after an actual interactive run,
review of its CSV/metadata/event log, and functional checks on the target
Windows version. CI may exercise short sampler startup/cleanup and packaging
without establishing the full desktop quality bar.

Definition references:

- [Microsoft memory counter/Task Manager mappings](https://learn.microsoft.com/en-us/windows/win32/memory/memory-performance-information)
- [Process.PrivateMemorySize64](https://learn.microsoft.com/en-us/dotnet/api/system.diagnostics.process.privatememorysize64)
- [GetGuiResources](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getguiresources)
- [GPU accounting in Task Manager](https://devblogs.microsoft.com/directx/gpus-in-the-task-manager/)
- [Known Windows 10 GPU process memory counter issue](https://learn.microsoft.com/en-us/troubleshoot/windows-client/performance/gpu-process-memory-counters-report-wrong-value)
- [Inno Setup non-administrative mode](https://jrsoftware.org/ishelp/topic_setup_privilegesrequired.htm)
