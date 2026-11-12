#requires -Version 5.1
# Shared production counter queries; imported by the sampler and its Windows smoke test.
Set-StrictMode -Version Latest
if ($env:OS -ne 'Windows_NT') { throw 'Snow profiling counters require Windows.' }

# Compile before launching: even setup failures must not leave a new process behind.
if (-not ('SnowProfilingV3.Native' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Diagnostics;
using Microsoft.Win32.SafeHandles;
using System.Runtime.InteropServices;
using System.Text;
namespace SnowProfilingV3 {
    public sealed class ControllerStats {
        public uint State;
        public uint Renderer;
        public ulong Presents;
        public uint IntervalMs;
        public uint SuspendReasons;
        public ulong StatsTickMs;
        public string Status = "controller_not_found";
    }
    public sealed class ProcessSnapshot {
        public ulong? CreationTimeFileTimeUtc;
        public ulong? Cpu100Nanoseconds;
        public long CpuSampleTimestamp;
        public string CpuStatus = "unavailable";
        public ulong? PrivateBytes;
        public ulong? WorkingSetBytes;
        public string MemoryStatus = "unavailable";
    }
    public static class Native {
        [StructLayout(LayoutKind.Sequential)]
        private struct FileTime {
            public uint Low;
            public uint High;
            public ulong Value { get { return ((ulong)High << 32) | Low; } }
        }
        [StructLayout(LayoutKind.Sequential)]
        private struct MemoryCounters {
            public uint Size;
            public uint PageFaultCount;
            public UIntPtr PeakWorkingSetSize;
            public UIntPtr WorkingSetSize;
            public UIntPtr QuotaPeakPagedPoolUsage;
            public UIntPtr QuotaPagedPoolUsage;
            public UIntPtr QuotaPeakNonPagedPoolUsage;
            public UIntPtr QuotaNonPagedPoolUsage;
            public UIntPtr PagefileUsage;
            public UIntPtr PeakPagefileUsage;
            public UIntPtr PrivateUsage;
        }
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern SafeProcessHandle OpenProcess(uint access, bool inherit, uint processId);
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern uint GetProcessId(SafeProcessHandle process);
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern uint WaitForSingleObject(SafeProcessHandle handle, uint milliseconds);
        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool GetProcessTimes(SafeProcessHandle process, out FileTime creation, out FileTime exit, out FileTime kernel, out FileTime user);
        [DllImport("kernel32.dll", EntryPoint = "K32GetProcessMemoryInfo", SetLastError = true)]
        private static extern bool GetProcessMemoryInfo(SafeProcessHandle process, ref MemoryCounters counters, uint size);

        public static SafeProcessHandle OpenForSampling(uint processId) {
            if (processId == 0) throw new ArgumentOutOfRangeException("processId");
            // Query-limited information plus synchronize, without requesting termination or write access.
            SafeProcessHandle handle = OpenProcess(0x1000U | 0x00100000U, false, processId);
            if (handle.IsInvalid) {
                int error = Marshal.GetLastWin32Error();
                handle.Dispose();
                throw new Win32Exception(error);
            }
            if (GetProcessId(handle) != processId) {
                handle.Dispose();
                throw new InvalidOperationException("Opened process handle has the wrong identity.");
            }
            return handle;
        }

        public static bool IsRunning(SafeProcessHandle process) {
            uint result = WaitForSingleObject(process, 0);
            if (result == 258) return true;
            if (result == 0) return false;
            throw new Win32Exception(Marshal.GetLastWin32Error());
        }

        public static ProcessSnapshot ReadProcessCounters(SafeProcessHandle process) {
            var result = new ProcessSnapshot();
            result.CpuSampleTimestamp = Stopwatch.GetTimestamp();
            if (process == null || process.IsInvalid || process.IsClosed) {
                result.CpuStatus = "handle_unavailable";
                result.MemoryStatus = "handle_unavailable";
                return result;
            }
            FileTime creation, exit, kernel, user;
            long before = Stopwatch.GetTimestamp();
            bool cpuAvailable = GetProcessTimes(process, out creation, out exit, out kernel, out user);
            int cpuError = Marshal.GetLastWin32Error();
            long after = Stopwatch.GetTimestamp();
            result.CpuSampleTimestamp = before + (after - before) / 2;
            if (cpuAvailable) {
                result.CreationTimeFileTimeUtc = creation.Value;
                result.Cpu100Nanoseconds = checked(kernel.Value + user.Value);
                result.CpuStatus = "available";
            }
            else result.CpuStatus = "GetProcessTimes_failed: " + new Win32Exception(cpuError).Message;
            var memory = new MemoryCounters();
            memory.Size = (uint)Marshal.SizeOf(typeof(MemoryCounters));
            if (GetProcessMemoryInfo(process, ref memory, memory.Size)) {
                result.PrivateBytes = memory.PrivateUsage.ToUInt64();
                result.WorkingSetBytes = memory.WorkingSetSize.ToUInt64();
                result.MemoryStatus = "available";
            }
            else result.MemoryStatus = "K32GetProcessMemoryInfo_failed: " + new Win32Exception(Marshal.GetLastWin32Error()).Message;
            return result;
        }

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
        [DllImport("user32.dll", EntryPoint = "GetPropW", CharSet = CharSet.Unicode, ExactSpelling = true)]
        private static extern IntPtr GetProp(IntPtr window, string name);

        public static ControllerStats GetControllerStats(uint expectedProcessId) {
            var result = new ControllerStats();
            if (IntPtr.Size != 8) { result.Status = "unsupported_32_bit_sampler"; return result; }
            IntPtr controller = IntPtr.Zero;
            int matches = 0;
            EnumWindowsProc callback = delegate(IntPtr window, IntPtr unused) {
                uint owner;
                GetWindowThreadProcessId(window, out owner);
                if (owner != expectedProcessId) return true;
                var name = new StringBuilder(256);
                if (GetClassName(window, name, name.Capacity) > 0 && name.ToString() == "SnowControllerV2") {
                    controller = window;
                    ++matches;
                }
                return true;
            };
            if (!EnumWindows(callback, IntPtr.Zero)) throw new Win32Exception(Marshal.GetLastWin32Error());
            if (matches == 0) return result;
            if (matches != 1) { result.Status = "no_unique_controller"; return result; }
            for (int attempt = 0; attempt < 3; ++attempt) {
                long before = GetProp(controller, "Snow.StatsTickMs").ToInt64();
                long state = GetProp(controller, "Snow.State").ToInt64();
                long renderer = GetProp(controller, "Snow.Renderer").ToInt64();
                long interval = GetProp(controller, "Snow.IntervalMs").ToInt64();
                ulong presents = unchecked((ulong)GetProp(controller, "Snow.Presents").ToInt64());
                ulong reasons = unchecked((ulong)GetProp(controller, "Snow.SuspendReasons").ToInt64());
                long after = GetProp(controller, "Snow.StatsTickMs").ToInt64();
                if (before != after) continue;
                // Positive protocol sentinels distinguish absent stats from legitimate zero counts/flags.
                if (before <= 0 || (state != 1 && state != 2) || renderer < 1 || renderer > 3 || interval <= 0 || interval > uint.MaxValue || reasons > uint.MaxValue) {
                    result.Status = "properties_unavailable_or_invalid";
                    return result;
                }
                result.State = (uint)state;
                result.Renderer = (uint)renderer;
                result.IntervalMs = (uint)interval;
                result.Presents = presents;
                result.SuspendReasons = (uint)reasons;
                result.StatsTickMs = (ulong)after;
                result.Status = "available";
                return result;
            }
            result.Status = "publication_changed_during_read";
            return result;
        }

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

function Open-SnowNativeProcess {
    [CmdletBinding()]
    param([Parameter(Mandatory = $true)][ValidateRange(1, 2147483647)][int]$ProcessId)
    return [SnowProfilingV3.Native]::OpenForSampling([uint32]$ProcessId)
}

function Test-SnowNativeProcessRunning {
    [CmdletBinding()]
    param([Parameter(Mandatory = $true)][Microsoft.Win32.SafeHandles.SafeProcessHandle]$Handle)
    return [SnowProfilingV3.Native]::IsRunning($Handle)
}

function Get-SnowNativeProcessSnapshot {
    [CmdletBinding()]
    param([Parameter(Mandatory = $true)][Microsoft.Win32.SafeHandles.SafeProcessHandle]$Handle)
    return [SnowProfilingV3.Native]::ReadProcessCounters($Handle)
}

function Get-SnowCpuInterval {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]$Previous,
        [Parameter(Mandatory = $true)]$Current,
        [Parameter(Mandatory = $true)][ValidateRange(1, 1048576)][int]$LogicalProcessors
    )
    $seconds = [double]($Current.CpuSampleTimestamp - $Previous.CpuSampleTimestamp) / [System.Diagnostics.Stopwatch]::Frequency
    $interval = [pscustomobject]@{
        IntervalSeconds = $seconds
        CpuTotalSeconds = $null
        CpuPercentOfMachine = $null
        CpuDeltaSeconds = $null
        CpuStatus = $Current.CpuStatus
    }
    if ($Current.CpuStatus -ne 'available' -or $null -eq $Current.Cpu100Nanoseconds) { return $interval }
    $interval.CpuTotalSeconds = [double]$Current.Cpu100Nanoseconds / 10000000.0
    if ($Previous.CpuStatus -ne 'available' -or $null -eq $Previous.Cpu100Nanoseconds) {
        $interval.CpuStatus = 'baseline_unavailable'
    }
    elseif ($seconds -le 0) { $interval.CpuStatus = 'nonpositive_monotonic_interval' }
    elseif ($Current.CreationTimeFileTimeUtc -ne $Previous.CreationTimeFileTimeUtc) { $interval.CpuStatus = 'process_identity_changed' }
    elseif ($Current.Cpu100Nanoseconds -lt $Previous.Cpu100Nanoseconds) { $interval.CpuStatus = 'counter_decreased' }
    else {
        $interval.CpuDeltaSeconds = [double]($Current.Cpu100Nanoseconds - $Previous.Cpu100Nanoseconds) / 10000000.0
        $interval.CpuPercentOfMachine = 100.0 * $interval.CpuDeltaSeconds / $seconds / $LogicalProcessors
    }
    return $interval
}

Export-ModuleMember -Function Open-SnowNativeProcess, Test-SnowNativeProcessRunning, Get-SnowNativeProcessSnapshot, Get-SnowCpuInterval
