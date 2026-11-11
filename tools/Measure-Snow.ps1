#requires -Version 5.1
<#
.SYNOPSIS
Records local process and GPU memory samples. Does not upload data.
.EXAMPLE
.\tools\Measure-Snow.ps1 -Exe .\build\Release\Snow.exe -DurationSeconds 300 -Renderer gdi -OutputPath .\profiles\gdi-5m.csv
.EXAMPLE
.\tools\Measure-Snow.ps1 -ProcessId 1234 -DurationSeconds 1800 -Renderer d3d11 -OutputPath .\profiles\attached-30m.csv
#>
[CmdletBinding(DefaultParameterSetName = 'Launch')]
param(
    [Parameter(Mandatory = $true, ParameterSetName = 'Launch')]
    [ValidateNotNullOrEmpty()][string]$Exe,
    [Parameter(Mandatory = $true, ParameterSetName = 'Attach')]
    [ValidateRange(1, 2147483647)][int]$ProcessId,
    [ValidateRange(1, 604800)][int]$DurationSeconds = 300,
    [ValidateRange(1, 60)][int]$IntervalSeconds = 5,
    [ValidateSet('gdi', 'd3d11')][string]$Renderer = 'gdi',
    [ValidateNotNullOrEmpty()][string]$OutputPath = 'snow-metrics.csv'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if ($env:OS -ne 'Windows_NT') { throw 'Measure-Snow requires Windows.' }

# Compile before launching: even setup failures must not leave a new process behind.
if (-not ('SnowProfiling.Native' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;
namespace SnowProfiling {
    public static class Native {
        private delegate bool EnumWindowsProc(IntPtr window, IntPtr argument);
        [DllImport("user32.dll", SetLastError = true)]
        private static extern uint GetGuiResources(IntPtr process, uint flags);
        [DllImport("kernel32.dll", EntryPoint = "SetLastError")]
        private static extern void ClearLastError(uint error);
        [DllImport("user32.dll", SetLastError = true)]
        private static extern bool EnumWindows(EnumWindowsProc callback, IntPtr argument);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        private static extern int GetClassName(IntPtr window, StringBuilder name, int capacity);
        [DllImport("user32.dll")]
        private static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);
        [DllImport("user32.dll", SetLastError = true)]
        private static extern bool PostMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);

        public static uint GuiCount(IntPtr process, uint flags) {
            ClearLastError(0);
            uint value = GetGuiResources(process, flags);
            int error = Marshal.GetLastWin32Error();
            if (value == 0 && error != 0) throw new Win32Exception(error);
            return value;
        }

        public static int RequestClose(uint expectedProcessId) {
            int posted = 0;
            int error = 0;
            EnumWindowsProc callback = delegate(IntPtr window, IntPtr unused) {
                uint owner;
                GetWindowThreadProcessId(window, out owner);
                if (owner != expectedProcessId) return true;
                var name = new StringBuilder(256);
                if (GetClassName(window, name, name.Capacity) > 0 && name.ToString() == "SnowControllerV2") {
                    if (PostMessage(window, 0x0010, IntPtr.Zero, IntPtr.Zero)) ++posted;
                    else error = Marshal.GetLastWin32Error();
                }
                return true;
            };
            if (!EnumWindows(callback, IntPtr.Zero)) throw new Win32Exception(Marshal.GetLastWin32Error());
            if (error != 0) throw new Win32Exception(error);
            return posted;
        }
    }
}
'@
}

function Read-OptionalProcessValue {
    param([scriptblock]$Read, [string]$Name, [System.Collections.Generic.List[string]]$Errors)
    try { return (& $Read) }
    catch { [void]$Errors.Add("${Name}: $($_.Exception.Message)"); return $null }
}

$cimTimeout = [uint32][Math]::Min(3, $IntervalSeconds)
function Get-PrivateWorkingSet {
    param([int]$TargetId)
    try {
        $rows = @(Get-CimInstance -ClassName Win32_PerfRawData_PerfProc_Process -Filter "IDProcess = $TargetId" -OperationTimeoutSec $cimTimeout)
        if ($rows.Count -ne 1) { return [pscustomobject]@{ Bytes = $null; Status = 'no_unique_instance' } }
        $property = $rows[0].CimInstanceProperties['WorkingSetPrivate']
        if ($null -eq $property -or $null -eq $property.Value) {
            return [pscustomobject]@{ Bytes = $null; Status = 'property_unavailable' }
        }
        return [pscustomobject]@{ Bytes = [uint64]$property.Value; Status = 'available' }
    }
    catch { return [pscustomobject]@{ Bytes = $null; Status = "query_failed: $($_.Exception.Message)" } }
}

function Get-GpuProcessMemory {
    param([int]$TargetId)
    try {
        # Retain every adapter instance for this exact PID; never include other PIDs or engine counters.
        $rows = @(Get-CimInstance -ClassName Win32_PerfFormattedData_GPUPerformanceCounters_GPUProcessMemory -Filter "Name LIKE 'pid_${TargetId}_%'" -OperationTimeoutSec $cimTimeout |
            Where-Object { $_.Name -match "^pid_${TargetId}_" })
        if ($rows.Count -eq 0) {
            return [pscustomobject]@{ Dedicated = $null; Shared = $null; Status = 'no_process_instance' }
        }
        $dedicated = [uint64]0
        $shared = [uint64]0
        $dedicatedAvailable = $true
        $sharedAvailable = $true
        foreach ($row in $rows) {
            $dedicatedProperty = $row.CimInstanceProperties['DedicatedUsage']
            $sharedProperty = $row.CimInstanceProperties['SharedUsage']
            if ($null -eq $dedicatedProperty -or $null -eq $dedicatedProperty.Value) { $dedicatedAvailable = $false }
            else { $dedicated += [uint64]$dedicatedProperty.Value }
            if ($null -eq $sharedProperty -or $null -eq $sharedProperty.Value) { $sharedAvailable = $false }
            else { $shared += [uint64]$sharedProperty.Value }
        }
        if (-not $dedicatedAvailable) { $dedicated = $null }
        if (-not $sharedAvailable) { $shared = $null }
        $status = if ($dedicatedAvailable -and $sharedAvailable) { 'available' } else { 'property_unavailable' }
        return [pscustomobject]@{ Dedicated = $dedicated; Shared = $shared; Status = $status }
    }
    catch { return [pscustomobject]@{ Dedicated = $null; Shared = $null; Status = "query_failed: $($_.Exception.Message)" } }
}

function Stop-OwnedSnow {
    param([System.Diagnostics.Process]$Target, [long]$ExpectedStartTicks)
    $deadline = [System.Diagnostics.Stopwatch]::StartNew()
    while ($deadline.Elapsed.TotalSeconds -lt 10) {
        $Target.Refresh()
        if ($Target.HasExited) {
            if ($Target.ExitCode -ne 0) { throw "Snow exited with code $($Target.ExitCode)." }
            return
        }
        if ($Target.StartTime.ToUniversalTime().Ticks -ne $ExpectedStartTicks) { throw 'Process identity changed; refusing to send WM_CLOSE.' }
        [void][SnowProfiling.Native]::RequestClose([uint32]$Target.Id)
        [void]$Target.WaitForExit([Math]::Min(1000, $IntervalSeconds * 1000))
    }
    $Target.Refresh()
    if (-not $Target.HasExited) {
        throw "Snow PID $($Target.Id) did not close within 10 seconds and is still running. Close it from the tray or with Ctrl+Alt+F12. It was not force-killed."
    }
    if ($Target.ExitCode -ne 0) { throw "Snow exited with code $($Target.ExitCode)." }
}

$csvPath = [System.IO.Path]::GetFullPath($OutputPath)
$metadataPath = [System.IO.Path]::ChangeExtension($csvPath, '.metadata.json')
if (Test-Path -LiteralPath $csvPath) { throw "Output already exists: $csvPath" }
if (Test-Path -LiteralPath $metadataPath) { throw "Output already exists: $metadataPath" }
[void][System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($csvPath))
if ($PSCmdlet.ParameterSetName -eq 'Launch') {
    $resolvedExe = (Resolve-Path -LiteralPath $Exe).ProviderPath
    if (-not (Test-Path -LiteralPath $resolvedExe -PathType Leaf)) { throw 'Exe must be an executable file.' }
}

$logicalProcessors = [Environment]::ProcessorCount
$logicalProcessorSource = 'Environment.ProcessorCount'
try {
    $machine = Get-CimInstance -ClassName Win32_ComputerSystem -OperationTimeoutSec $cimTimeout
    if ($machine.NumberOfLogicalProcessors -gt 0) {
        $logicalProcessors = [int]$machine.NumberOfLogicalProcessors
        $logicalProcessorSource = 'Win32_ComputerSystem.NumberOfLogicalProcessors'
    }
}
catch { Write-Warning 'Machine logical processor count unavailable; using the count visible to this PowerShell process.' }
$gpuAdapters = @()
try {
    $gpuAdapters = @(Get-CimInstance -ClassName Win32_VideoController -OperationTimeoutSec $cimTimeout |
        Select-Object Name, DriverVersion)
}
catch { Write-Warning 'GPU adapter metadata is unavailable; individual sample status fields still report counter availability.' }

$metadata = [ordered]@{
    SchemaVersion = 1
    Status = 'starting'
    Error = $null
    StartedUtc = [DateTime]::UtcNow.ToString('o')
    FinishedUtc = $null
    RequestedDurationSeconds = $DurationSeconds
    RequestedIntervalSeconds = $IntervalSeconds
    Samples = 0
    ActualElapsedSeconds = 0
    ProcessId = $null
    ProcessStartTimeUtc = $null
    Executable = $null
    ExecutableSha256 = $null
    Ownership = $PSCmdlet.ParameterSetName
    CleanupStatus = 'not_started'
    RendererRequested = $Renderer
    RendererActual = 'not_verified; D3D11 can fall back to GDI'
    LogicalProcessors = $logicalProcessors
    LogicalProcessorSource = $logicalProcessorSource
    OS = [Environment]::OSVersion.VersionString
    PowerShellVersion = $PSVersionTable.PSVersion.ToString()
    Is64BitSampler = [Environment]::Is64BitProcess
    GpuAdapters = $gpuAdapters
    CsvPath = $csvPath
}
$target = $null
$owned = $false
$startTicks = [long]0
$watch = $null
$failure = $null
try {
    if ($PSCmdlet.ParameterSetName -eq 'Launch') {
        $target = Start-Process -FilePath $resolvedExe -WorkingDirectory ([System.IO.Path]::GetDirectoryName($resolvedExe)) -ArgumentList "--renderer=$Renderer" -PassThru
        $owned = $true
    }
    else { $target = Get-Process -Id $ProcessId }
    $target.Refresh()
    if ($target.HasExited) { throw 'Target exited before sampling. Close an existing Snow instance before starting a new profile.' }
    $startTicks = $target.StartTime.ToUniversalTime().Ticks
    $metadata.ProcessId = $target.Id
    $metadata.ProcessStartTimeUtc = $target.StartTime.ToUniversalTime().ToString('o')
    $metadata.CleanupStatus = if ($owned) { 'pending' } else { 'attached_process_left_running' }
    try {
        $metadata.Executable = $target.Path
        $metadata.ExecutableSha256 = (Get-FileHash -LiteralPath $target.Path -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    catch { Write-Warning 'Executable path/hash is unavailable for the attached process.' }
    $previousCpuSeconds = $target.TotalProcessorTime.TotalSeconds
    $previousElapsed = [double]0
    $watch = [System.Diagnostics.Stopwatch]::StartNew()
    $nextSample = [double][Math]::Min($IntervalSeconds, $DurationSeconds)
    $metadata.Status = 'running'
    while ($true) {
        $waitMilliseconds = [int][Math]::Floor(($nextSample - $watch.Elapsed.TotalSeconds) * 1000)
        if ($waitMilliseconds -gt 0) {
            Start-Sleep -Milliseconds ([Math]::Min($waitMilliseconds, $IntervalSeconds * 1000))
            continue
        }
        $collection = [System.Diagnostics.Stopwatch]::StartNew()
        $target.Refresh()
        if ($target.HasExited) { throw "Target exited before the requested duration (exit code $($target.ExitCode))." }
        if ($target.StartTime.ToUniversalTime().Ticks -ne $startTicks) { throw 'Process identity changed; refusing to sample a reused PID.' }
        $elapsed = $watch.Elapsed.TotalSeconds
        $timestamp = [DateTime]::UtcNow.ToString('o')
        $cpuSeconds = $target.TotalProcessorTime.TotalSeconds
        $actualInterval = $elapsed - $previousElapsed
        $cpuDelta = $cpuSeconds - $previousCpuSeconds
        if ($actualInterval -le 0 -or $cpuDelta -lt 0) { throw 'Invalid CPU measurement interval; no fabricated sample will be written.' }
        $errors = New-Object 'System.Collections.Generic.List[string]'
        $privateBytes = Read-OptionalProcessValue { $target.PrivateMemorySize64 } 'PrivateBytes' $errors
        $workingSet = Read-OptionalProcessValue { $target.WorkingSet64 } 'WorkingSetBytes' $errors
        $gdiObjects = Read-OptionalProcessValue { [SnowProfiling.Native]::GuiCount($target.Handle, 0) } 'GdiObjects' $errors
        $userObjects = Read-OptionalProcessValue { [SnowProfiling.Native]::GuiCount($target.Handle, 1) } 'UserObjects' $errors
        $privateWorkingSet = Get-PrivateWorkingSet $target.Id
        $gpuMemory = Get-GpuProcessMemory $target.Id
        $collection.Stop()
        $row = [pscustomobject][ordered]@{
            TimestampUtc = $timestamp
            ElapsedSeconds = [Math]::Round($elapsed, 6)
            IntervalSeconds = [Math]::Round($actualInterval, 6)
            ProcessId = $target.Id
            ProcessStartTimeUtc = $metadata.ProcessStartTimeUtc
            RendererRequested = $Renderer
            LogicalProcessors = $logicalProcessors
            CpuTotalSeconds = $cpuSeconds
            CpuPercentOfMachine = [Math]::Round(100.0 * $cpuDelta / $actualInterval / $logicalProcessors, 6)
            PrivateBytes = $privateBytes
            CommitSizeBytes = $privateBytes
            WorkingSetBytes = $workingSet
            PrivateWorkingSetBytes = $privateWorkingSet.Bytes
            GdiObjects = $gdiObjects
            UserObjects = $userObjects
            GpuDedicatedBytes = $gpuMemory.Dedicated
            GpuSharedBytes = $gpuMemory.Shared
            PrivateWorkingSetStatus = $privateWorkingSet.Status
            GpuMemoryStatus = $gpuMemory.Status
            ProcessMetricErrors = $errors -join '; '
            SampleCollectionMilliseconds = [Math]::Round($collection.Elapsed.TotalMilliseconds, 3)
        }
        if ($metadata.Samples -eq 0) { $row | Export-Csv -LiteralPath $csvPath -NoTypeInformation -Encoding UTF8 }
        else { $row | Export-Csv -LiteralPath $csvPath -NoTypeInformation -Encoding UTF8 -Append }
        ++$metadata.Samples
        $previousCpuSeconds = $cpuSeconds
        $previousElapsed = $elapsed
        if ($elapsed -ge $DurationSeconds) { break }
        # Skip missed deadlines rather than inventing catch-up samples.
        do { $nextSample += $IntervalSeconds } while ($nextSample -le $watch.Elapsed.TotalSeconds)
        $nextSample = [Math]::Min($nextSample, $DurationSeconds)
    }
    $metadata.Status = 'complete'
}
catch {
    $failure = $_
    $metadata.Status = 'failed'
    $metadata.Error = $_.Exception.Message
}
finally {
    if ($null -ne $watch) { $watch.Stop(); $metadata.ActualElapsedSeconds = $watch.Elapsed.TotalSeconds }
    if ($owned -and $null -ne $target) {
        try {
            # The initial identity read can fail during immediate startup exit.
            if ($startTicks -eq 0 -and -not $target.HasExited) { $startTicks = $target.StartTime.ToUniversalTime().Ticks }
            Stop-OwnedSnow $target $startTicks
            $metadata.CleanupStatus = 'exited_normally'
        }
        catch {
            $metadata.CleanupStatus = "failed: $($_.Exception.Message)"
            $metadata.Status = 'failed'
            if ($null -eq $failure) { $failure = $_; $metadata.Error = $_.Exception.Message }
            else { Write-Warning $_.Exception.Message }
        }
    }
    $metadata.FinishedUtc = [DateTime]::UtcNow.ToString('o')
    try { $metadata | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $metadataPath -Encoding UTF8 }
    catch { if ($null -eq $failure) { $failure = $_ } else { Write-Warning "Could not save metadata: $($_.Exception.Message)" } }
    if ($null -ne $target) { $target.Dispose() }
}
if ($null -ne $failure) { throw $failure }
Write-Host "Recorded $($metadata.Samples) real samples: $csvPath"
Write-Host "Environment and outcome: $metadataPath"
