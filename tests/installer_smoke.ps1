param([Parameter(Mandatory=$true)][string]$Setup)
$ErrorActionPreference = 'Stop'
$folder = Join-Path ([IO.Path]::GetTempPath()) ('Snow-install-smoke-' + [guid]::NewGuid().ToString('N'))
$installed = Join-Path $folder 'Snow.exe'
$uninstaller = Join-Path $folder 'unins000.exe'
try {
    $install = Start-Process -FilePath (Resolve-Path $Setup).Path -ArgumentList @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', "/DIR=`"$folder`"") -PassThru
    if (-not $install.WaitForExit(45000)) { throw 'Installer did not finish within 45 seconds' }
    if ($install.ExitCode -ne 0 -or -not (Test-Path $installed)) { throw "Installation failed: $($install.ExitCode)" }
    & "$PSScriptRoot/app_smoke.ps1" -Exe $installed
    $remove = Start-Process -FilePath $uninstaller -ArgumentList @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART') -PassThru
    if (-not $remove.WaitForExit(30000)) { throw 'Uninstaller did not finish within 30 seconds' }
    if ($remove.ExitCode -ne 0) { throw "Uninstall failed: $($remove.ExitCode)" }
    if (Test-Path $installed) { throw 'Uninstaller left Snow.exe installed' }
    Write-Host 'Per-user installer, installed executable lifecycle and silent uninstall passed.'
} finally {
    # Only the uniquely named test installation is eligible for cleanup.
    if (Test-Path $uninstaller) {
        $cleanup = Start-Process -FilePath $uninstaller -ArgumentList @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART') -PassThru
        if (-not $cleanup.WaitForExit(10000)) { Write-Warning 'Test uninstaller is still running' }
    }
}
