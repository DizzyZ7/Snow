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
    [Parameter(ParameterSetName = 'Launch')][switch]$Benchmark,
    [ValidateNotNullOrEmpty()][string]$OutputPath = 'snow-metrics.csv'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if ($env:OS -ne 'Windows_NT') { throw 'Measure-Snow requires Windows.' }

# Load and compile all native query helpers before launching a process.
Import-Module (Join-Path $PSScriptRoot 'Snow-Profiling.psm1') -Force

function Get-SnowControllerStats {
    param([int]$TargetId)
    $snapshot = [pscustomobject]@{
        State = $null; RendererActual = $null; TargetFps = $null; PresentCount = $null
        StatsTickMs = $null; SuspendReasons = $null; Status = 'unavailable'
    }
    try {
        $nativeStats = [SnowProfilingV3.Native]::GetControllerStats([uint32]$TargetId)
        $snapshot.Status = $nativeStats.Status
        if ($nativeStats.Status -eq 'available') {
            $snapshot.State = if ($nativeStats.State -eq 1) { 'running' } else { 'paused' }
            $snapshot.RendererActual = switch ($nativeStats.Renderer) { 1 { 'gdi' }; 2 { 'd3d11' }; 3 { 'mixed' } }
            $snapshot.TargetFps = [Math]::Round(1000.0 / $nativeStats.IntervalMs, 6)
            $snapshot.PresentCount = $nativeStats.Presents
            $snapshot.StatsTickMs = $nativeStats.StatsTickMs
            $snapshot.SuspendReasons = $nativeStats.SuspendReasons
        }
    }
    catch { $snapshot.Status = "query_failed: $($_.Exception.Message)" }
    return $snapshot
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
        [void][SnowProfilingV3.Native]::RequestClose([uint32]$Target.Id)
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
    SchemaVersion = 3
    Status = 'starting'
    Error = $null
    StartedUtc = [DateTime]::UtcNow.ToString('o')
    FinishedUtc = $null
    RequestedDurationSeconds = $DurationSeconds
    RequestedIntervalSeconds = $IntervalSeconds
    Samples = 0
    RunningStateSamples = 0
    PausedStateSamples = 0
    UnavailableStateSamples = 0
    SamplesWithMeasuredPresentFps = 0
    AvailableCpuSamples = 0
    UnavailableCpuSamples = 0
    ActualElapsedSeconds = 0
    ProcessId = $null
    ProcessStartTimeUtc = $null
    Executable = $null
    ExecutableSha256 = $null
    Ownership = $PSCmdlet.ParameterSetName
    CleanupStatus = 'not_started'
    RendererRequested = $Renderer
    RendererActual = $null
    RenderersObserved = @()
    BenchmarkRequested = if ($PSCmdlet.ParameterSetName -eq 'Launch') { [bool]$Benchmark } else { $null }
    LogicalProcessors = $logicalProcessors
    LogicalProcessorSource = $logicalProcessorSource
    CpuCounterSource = 'GetProcessTimes'
    ProcessMemorySource = 'K32GetProcessMemoryInfo/PROCESS_MEMORY_COUNTERS_EX'
    MonotonicClockFrequency = [System.Diagnostics.Stopwatch]::Frequency
    OS = [Environment]::OSVersion.VersionString
    PowerShellVersion = $PSVersionTable.PSVersion.ToString()
    Is64BitSampler = [Environment]::Is64BitProcess
    GpuAdapters = $gpuAdapters
    CsvPath = $csvPath
}
$target = $null
$nativeHandle = $null
$owned = $false
$startTicks = [long]0
$watch = $null
$failure = $null
try {
    if ($PSCmdlet.ParameterSetName -eq 'Launch') {
        $launchArguments = @("--renderer=$Renderer")
        if ($Benchmark) { $launchArguments += '--benchmark' }
        $target = Start-Process -FilePath $resolvedExe -WorkingDirectory ([System.IO.Path]::GetDirectoryName($resolvedExe)) -ArgumentList $launchArguments -PassThru
        $owned = $true
    }
    else { $target = Get-Process -Id $ProcessId }
    $target.Refresh()
    if ($target.HasExited) { throw 'Target exited before sampling. Close an existing Snow instance before starting a new profile.' }
    $startTicks = $target.StartTime.ToUniversalTime().Ticks
    $expectedCreationFileTime = [uint64]$target.StartTime.ToUniversalTime().ToFileTimeUtc()
    $nativeHandle = Open-SnowNativeProcess -ProcessId $target.Id
    if (-not (Test-SnowNativeProcessRunning -Handle $nativeHandle)) { throw 'Target exited before native sampling could start.' }
    $metadata.ProcessId = $target.Id
    $metadata.ProcessStartTimeUtc = $target.StartTime.ToUniversalTime().ToString('o')
    $metadata.CleanupStatus = if ($owned) { 'pending' } else { 'attached_process_left_running' }
    try {
        $metadata.Executable = $target.Path
        $metadata.ExecutableSha256 = (Get-FileHash -LiteralPath $target.Path -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    catch { Write-Warning 'Executable path/hash is unavailable for the attached process.' }
    $previousControllerStats = Get-SnowControllerStats $target.Id
    $previousNative = Get-SnowNativeProcessSnapshot -Handle $nativeHandle
    if ($null -ne $previousNative.CreationTimeFileTimeUtc -and $previousNative.CreationTimeFileTimeUtc -ne $expectedCreationFileTime) {
        throw 'Native handle creation time differs from the selected process; refusing to sample a reused PID.'
    }
    $baselineTimestamp = $previousNative.CpuSampleTimestamp
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
        if (-not (Test-SnowNativeProcessRunning -Handle $nativeHandle)) { throw 'The exact opened target process exited before sampling.' }
        $native = Get-SnowNativeProcessSnapshot -Handle $nativeHandle
        if ($null -ne $native.CreationTimeFileTimeUtc -and $native.CreationTimeFileTimeUtc -ne $expectedCreationFileTime) {
            throw 'Native process creation time changed; refusing to sample a reused PID.'
        }
        $elapsed = [double]($native.CpuSampleTimestamp - $baselineTimestamp) / [System.Diagnostics.Stopwatch]::Frequency
        $timestamp = [DateTime]::UtcNow.ToString('o')
        $cpuInterval = Get-SnowCpuInterval -Previous $previousNative -Current $native -LogicalProcessors $logicalProcessors
        $actualInterval = $cpuInterval.IntervalSeconds
        if ($actualInterval -le 0) { throw 'Invalid monotonic measurement interval; no fabricated sample will be written.' }
        $errors = New-Object 'System.Collections.Generic.List[string]'
        $privateBytes = $native.PrivateBytes
        $workingSet = $native.WorkingSetBytes
        $gdiObjects = Read-OptionalProcessValue { [SnowProfilingV3.Native]::GuiCount($nativeHandle.DangerousGetHandle(), 0) } 'GdiObjects' $errors
        $userObjects = Read-OptionalProcessValue { [SnowProfilingV3.Native]::GuiCount($nativeHandle.DangerousGetHandle(), 1) } 'UserObjects' $errors
        $controllerStats = Get-SnowControllerStats $target.Id
        $measuredPresentFps = $null
        if ($controllerStats.Status -eq 'available' -and $previousControllerStats.Status -eq 'available' -and
            $controllerStats.State -eq 'running' -and $previousControllerStats.State -eq 'running' -and
            $controllerStats.SuspendReasons -eq 0 -and $previousControllerStats.SuspendReasons -eq 0 -and
            $controllerStats.StatsTickMs -gt $previousControllerStats.StatsTickMs -and
            $controllerStats.PresentCount -ge $previousControllerStats.PresentCount) {
            $measuredPresentFps = [Math]::Round(1000.0 * ($controllerStats.PresentCount - $previousControllerStats.PresentCount) /
                ($controllerStats.StatsTickMs - $previousControllerStats.StatsTickMs), 6)
        }
        if ($null -ne $controllerStats.RendererActual) {
            if ($metadata.RenderersObserved -notcontains $controllerStats.RendererActual) {
                $metadata.RenderersObserved += $controllerStats.RendererActual
            }
            $metadata.RendererActual = if ($metadata.RenderersObserved.Count -eq 1) { $metadata.RenderersObserved[0] } else { 'multiple_observed' }
        }
        $privateWorkingSet = Get-PrivateWorkingSet $target.Id
        $gpuMemory = Get-GpuProcessMemory $target.Id
        if (-not (Test-SnowNativeProcessRunning -Handle $nativeHandle)) {
            throw 'The exact target process exited during collection; no potentially reused-PID row was written.'
        }
        $collection.Stop()
        $cpuPercent = if ($null -ne $cpuInterval.CpuPercentOfMachine) { [Math]::Round($cpuInterval.CpuPercentOfMachine, 6) } else { $null }
        $row = [pscustomobject][ordered]@{
            TimestampUtc = $timestamp
            ElapsedSeconds = [Math]::Round($elapsed, 6)
            IntervalSeconds = [Math]::Round($actualInterval, 6)
            ProcessId = $target.Id
            ProcessStartTimeUtc = $metadata.ProcessStartTimeUtc
            RendererRequested = $Renderer
            RendererActual = $controllerStats.RendererActual
            BenchmarkRequested = $metadata.BenchmarkRequested
            State = $controllerStats.State
            TargetFps = $controllerStats.TargetFps
            PresentCount = $controllerStats.PresentCount
            MeasuredPresentFps = $measuredPresentFps
            SuspendReasons = $controllerStats.SuspendReasons
            StatsTickMs = $controllerStats.StatsTickMs
            ControllerStatsStatus = $controllerStats.Status
            LogicalProcessors = $logicalProcessors
            CpuCounterSource = $metadata.CpuCounterSource
            CpuCounterStatus = $cpuInterval.CpuStatus
            CpuTotalSeconds = $cpuInterval.CpuTotalSeconds
            CpuDeltaSeconds = $cpuInterval.CpuDeltaSeconds
            CpuPercentOfMachine = $cpuPercent
            ProcessMemorySource = $metadata.ProcessMemorySource
            ProcessMemoryStatus = $native.MemoryStatus
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
        if ($controllerStats.State -eq 'running') { ++$metadata.RunningStateSamples }
        elseif ($controllerStats.State -eq 'paused') { ++$metadata.PausedStateSamples }
        else { ++$metadata.UnavailableStateSamples }
        if ($null -ne $measuredPresentFps) { ++$metadata.SamplesWithMeasuredPresentFps }
        if ($cpuInterval.CpuStatus -eq 'available' -and $null -ne $cpuPercent) { ++$metadata.AvailableCpuSamples }
        else { ++$metadata.UnavailableCpuSamples }
        $previousNative = $native
        $previousControllerStats = $controllerStats
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
    if ($null -ne $nativeHandle) { $nativeHandle.Dispose() }
}
if ($null -ne $failure) { throw $failure }
Write-Host "Recorded $($metadata.Samples) real samples: $csvPath"
Write-Host "Environment and outcome: $metadataPath"
