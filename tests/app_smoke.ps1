param([Parameter(Mandatory=$true)][string]$Exe)
$ErrorActionPreference = 'Stop'
$logs = Join-Path ([IO.Path]::GetTempPath()) ('Snow-app-smoke-' + [guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($logs)
$process = $null
try {
    $out = Join-Path $logs 'stdout.log'
    $err = Join-Path $logs 'stderr.log'
    $process = Start-Process -FilePath $Exe -ArgumentList '--smoke-test' -PassThru -RedirectStandardOutput $out -RedirectStandardError $err
    # Hold the native handle before waiting: Start-Process with redirected
    # streams can otherwise leave ExitCode unavailable after process exit.
    $null = $process.Handle
    if (-not $process.WaitForExit(30000)) {
        Stop-Process -Id $process.Id -Force
        throw 'Snow did not exit normally within the smoke-test deadline'
    }
    $process.WaitForExit() # Drain redirected output after the bounded wait succeeded.
    $process.Refresh()
    if ($null -eq $process.ExitCode) { throw 'Snow exited but its exit code could not be read' }
    if ($process.ExitCode -ne 0) { throw "Snow smoke test returned $($process.ExitCode)" }
    Write-Host 'Snow application initialized, paused/resumed, kept animating in a popup, rebuilt overlays and exited normally.'
} finally {
    foreach ($file in @('stdout.log','stderr.log')) {
        $path = Join-Path $logs $file
        if (Test-Path $path) { Get-Content $path | Write-Host }
    }
    if ($null -ne $process) { $process.Refresh(); if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force }; $process.Dispose() }
    Remove-Item -LiteralPath $logs -Recurse -Force
}
