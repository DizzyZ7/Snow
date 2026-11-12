#requires -Version 5.1
<# Verifies the production native counter path against real CPU work, without Snow or a desktop. #>
[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if ($env:OS -ne 'Windows_NT') { throw 'Profiler smoke test requires Windows.' }
Import-Module (Join-Path (Split-Path -Parent $PSScriptRoot) 'tools\Snow-Profiling.psm1') -Force

if (-not ('SnowProfilerSmoke.Workload' -as [type])) {
    Add-Type -TypeDefinition @'
using System.Diagnostics;
namespace SnowProfilerSmoke {
    public static class Workload {
        public static ulong Burn(int milliseconds) {
            var watch = Stopwatch.StartNew();
            ulong value = 1469598103934665603UL;
            do {
                for (int i = 0; i < 250000; ++i)
                    value = unchecked((value ^ (uint)i) * 1099511628211UL);
            } while (watch.ElapsedMilliseconds < milliseconds);
            return value;
        }
    }
}
'@
}

$handle = $null
$invalidHandle = $null
try {
    $handle = Open-SnowNativeProcess -ProcessId $PID
    if (-not (Test-SnowNativeProcessRunning -Handle $handle)) { throw 'Current process handle was not live.' }
    $before = Get-SnowNativeProcessSnapshot -Handle $handle
    if ($before.CpuStatus -ne 'available' -or $null -eq $before.Cpu100Nanoseconds) {
        throw "Initial CPU query unavailable: $($before.CpuStatus)"
    }
    if ($before.MemoryStatus -ne 'available' -or $before.PrivateBytes -le 0 -or $before.WorkingSetBytes -le 0) {
        throw "Current process memory query unavailable or invalid: $($before.MemoryStatus)"
    }
    $expectedCreation = ([System.Diagnostics.Process]::GetCurrentProcess()).StartTime.ToUniversalTime().ToFileTimeUtc()
    if ($before.CreationTimeFileTimeUtc -ne [uint64]$expectedCreation) { throw 'Opened handle creation time did not match the test process.' }
    $deadline = [System.Diagnostics.Stopwatch]::StartNew()
    $cpuDelta = [double]0
    $checksum = [uint64]0
    do {
        $checksum = [SnowProfilerSmoke.Workload]::Burn(250)
        $after = Get-SnowNativeProcessSnapshot -Handle $handle
        if ($after.CpuStatus -ne 'available' -or $null -eq $after.Cpu100Nanoseconds) {
            throw "CPU query failed after real work: $($after.CpuStatus)"
        }
        if ($after.CreationTimeFileTimeUtc -ne $before.CreationTimeFileTimeUtc) { throw 'Process identity changed while its handle was held.' }
        if ($after.Cpu100Nanoseconds -lt $before.Cpu100Nanoseconds) { throw 'Native process CPU counter decreased.' }
        $cpuDelta = [double]($after.Cpu100Nanoseconds - $before.Cpu100Nanoseconds) / 10000000.0
    } while ($cpuDelta -lt 0.05 -and $deadline.Elapsed.TotalSeconds -lt 10)
    if ($cpuDelta -lt 0.05) { throw "CPU counter failed to advance by 50ms during real CPU work (delta ${cpuDelta}s). Treat CPU measurements as inconclusive." }
    $interval = Get-SnowCpuInterval -Previous $before -Current $after -LogicalProcessors ([Environment]::ProcessorCount)
    if ($interval.CpuStatus -ne 'available' -or $null -eq $interval.CpuPercentOfMachine -or
        $interval.CpuPercentOfMachine -le 0 -or [double]::IsNaN($interval.CpuPercentOfMachine) -or
        [double]::IsInfinity($interval.CpuPercentOfMachine) -or $interval.IntervalSeconds -le 0) {
        throw 'Production CPU interval calculation did not report a finite positive rate for real work.'
    }

    # A failed/invalid native query must remain unavailable rather than becoming a valid-looking zero.
    $invalidHandle = New-Object Microsoft.Win32.SafeHandles.SafeProcessHandle -ArgumentList ([IntPtr]::Zero), $false
    $unavailable = Get-SnowNativeProcessSnapshot -Handle $invalidHandle
    if ($null -ne $unavailable.Cpu100Nanoseconds -or $null -ne $unavailable.PrivateBytes -or
        $null -ne $unavailable.WorkingSetBytes -or $unavailable.CpuStatus -eq 'available' -or
        $unavailable.MemoryStatus -eq 'available') { throw 'Invalid-handle counters were fabricated as available data.' }
    $unavailableInterval = Get-SnowCpuInterval -Previous $before -Current $unavailable -LogicalProcessors ([Environment]::ProcessorCount)
    if ($null -ne $unavailableInterval.CpuPercentOfMachine -or $null -ne $unavailableInterval.CpuTotalSeconds) {
        throw 'An unavailable CPU query was converted into numeric CPU data.'
    }
    Write-Host "Profiler native CPU/memory self-test passed: CPU advanced ${cpuDelta}s over $($interval.IntervalSeconds)s; work checksum $checksum."
}
finally {
    if ($null -ne $invalidHandle) { $invalidHandle.Dispose() }
    if ($null -ne $handle) { $handle.Dispose() }
}
