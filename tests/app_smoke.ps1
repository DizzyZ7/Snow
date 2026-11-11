param([Parameter(Mandatory=$true)][string]$Exe)
$ErrorActionPreference = 'Stop'
$process = Start-Process -FilePath $Exe -ArgumentList '--smoke-test' -PassThru
if (-not $process.WaitForExit(30000)) {
    Stop-Process -Id $process.Id -Force
    throw 'Snow did not exit normally within the smoke-test deadline'
}
if ($process.ExitCode -ne 0) { throw "Snow smoke test returned $($process.ExitCode)" }
Write-Host 'Snow application initialized, paused/resumed, rebuilt overlays and exited normally.'
