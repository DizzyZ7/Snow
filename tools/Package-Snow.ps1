#requires -Version 5.1
<#
.SYNOPSIS
Creates a portable ZIP, optional per-user installer, release metadata and SHA256 checksums.
.EXAMPLE
.\tools\Package-Snow.ps1 -Exe .\build\Release\Snow.exe -Version 0.2.0 -OutputDirectory .\dist -RequireInstaller
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][ValidateNotNullOrEmpty()][string]$Exe,
    [Parameter(Mandatory = $true)][ValidatePattern('^\d+\.\d+\.\d+(?:-[0-9A-Za-z.-]+)?$')][string]$Version,
    [ValidateNotNullOrEmpty()][string]$OutputDirectory = 'dist',
    [string]$IsccPath,
    [switch]$RequireInstaller
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if ($env:OS -ne 'Windows_NT') { throw 'Package-Snow requires Windows.' }
$repoRoot = Split-Path -Parent $PSScriptRoot
$resolvedExe = (Resolve-Path -LiteralPath $Exe).ProviderPath
$readme = Join-Path $repoRoot 'README.md'
$installerScript = Join-Path $repoRoot 'installer\Snow.iss'
$output = [System.IO.Path]::GetFullPath($OutputDirectory)
if (-not (Test-Path -LiteralPath $readme -PathType Leaf)) { throw "README is missing: $readme" }
if (-not (Test-Path -LiteralPath $installerScript -PathType Leaf)) { throw "Installer script is missing: $installerScript" }

# The artifact name promises x64. Reject an arbitrary file or an accidental Win32 build.
$stream = [System.IO.File]::OpenRead($resolvedExe)
$reader = New-Object System.IO.BinaryReader($stream)
try {
    if ($stream.Length -lt 64 -or $reader.ReadUInt16() -ne 0x5A4D) { throw 'Exe is not a PE executable.' }
    $stream.Position = 0x3C
    $peOffset = $reader.ReadInt32()
    if ($peOffset -lt 64 -or $peOffset -gt $stream.Length - 6) { throw 'Exe has an invalid PE header offset.' }
    $stream.Position = $peOffset
    if ($reader.ReadUInt32() -ne 0x00004550 -or $reader.ReadUInt16() -ne 0x8664) { throw 'Exe must be a Windows x64 (AMD64) PE executable.' }
}
finally { $reader.Dispose(); $stream.Dispose() }
$numericParts = ($Version -split '-')[0] -split '\.'
foreach ($part in $numericParts) { if ([uint64]$part -gt 65535) { throw 'Numeric version components must be in the range 0..65535.' } }
$numericVersion = ($numericParts -join '.') + '.0'

$compiler = $null
if ($IsccPath) {
    $compiler = (Resolve-Path -LiteralPath $IsccPath).ProviderPath
    if (-not (Test-Path -LiteralPath $compiler -PathType Leaf)) { throw 'IsccPath must identify ISCC.exe.' }
}
else {
    $command = Get-Command ISCC.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -ne $command) { $compiler = $command.Source }
    else {
        foreach ($programDirectory in @(${env:ProgramFiles(x86)}, $env:ProgramFiles)) {
            if ($programDirectory) {
                $candidate = Join-Path $programDirectory 'Inno Setup 6\ISCC.exe'
                if (Test-Path -LiteralPath $candidate -PathType Leaf) { $compiler = $candidate; break }
            }
        }
    }
}
if (-not $compiler -and $RequireInstaller) { throw 'Inno Setup ISCC.exe was not found. Install Inno Setup 6.3+ or pass -IsccPath.' }
if (-not $compiler) { Write-Warning 'ISCC.exe was not found: the installer will be explicitly marked not built. Use -RequireInstaller in release CI.' }

$baseName = "Snow-$Version-windows-x64"
$zipName = "$baseName.zip"
$setupName = "$baseName-setup.exe"
$notesName = "$baseName-RELEASE-NOTES.txt"
$metadataName = "$baseName-metadata.json"
$checksumsName = 'SHA256SUMS.txt'
foreach ($name in @($zipName, $setupName, $notesName, $metadataName, $checksumsName)) {
    if (Test-Path -LiteralPath (Join-Path $output $name)) { throw "Refusing to overwrite an existing artifact: $name. Choose a clean output directory." }
}
$tempRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("Snow-package-" + [Guid]::NewGuid().ToString('N'))
[void][System.IO.Directory]::CreateDirectory($tempRoot)
try {
    $stagedOutput = Join-Path $tempRoot 'output'
    $portable = Join-Path $tempRoot $baseName
    [void][System.IO.Directory]::CreateDirectory($stagedOutput)
    [void][System.IO.Directory]::CreateDirectory($portable)
    $exeHash = (Get-FileHash -LiteralPath $resolvedExe -Algorithm SHA256).Hash.ToLowerInvariant()
    $createdUtc = [DateTime]::UtcNow.ToString('o')
    $notes = @"
Snow $Version - Windows x64
Packaged UTC: $createdUtc
Snow.exe SHA256: $exeHash

Experimental build. Packaging does not establish desktop correctness, performance,
8-hour stability, code signing, or production release approval.

Portable: extract the complete folder and run Snow.exe as the current user.
Renderer: GDI is the default; pass --renderer=d3d11 to request the GPU candidate.
Pause/resume: Ctrl+Alt+F11. Exit: Ctrl+Alt+F12 or the tray menu.
Settings: %LOCALAPPDATA%\Snow\settings.ini. Autostart is an explicit app setting.
See README.md and docs/PROFILING.md in the source repository for validation steps.
"@
    $notes | Set-Content -LiteralPath (Join-Path $stagedOutput $notesName) -Encoding UTF8
    Copy-Item -LiteralPath $resolvedExe -Destination (Join-Path $portable 'Snow.exe')
    Copy-Item -LiteralPath $readme -Destination (Join-Path $portable 'README.md')
    $notes | Set-Content -LiteralPath (Join-Path $portable 'RELEASE-NOTES.txt') -Encoding UTF8
    Compress-Archive -LiteralPath $portable -DestinationPath (Join-Path $stagedOutput $zipName) -CompressionLevel Optimal

    $installerStatus = 'not_built; ISCC.exe not found'
    $artifacts = @($zipName, $notesName, $metadataName)
    if ($compiler) {
        $compilerArguments = @(
            '/Qp', "/O$stagedOutput", "/DSnowVersion=$Version", "/DSnowNumericVersion=$numericVersion",
            "/DSnowSourceExe=$resolvedExe", "/DSnowReadme=$readme", $installerScript
        )
        & $compiler @compilerArguments
        if ($LASTEXITCODE -ne 0) { throw "ISCC.exe failed with exit code $LASTEXITCODE." }
        $setup = Join-Path $stagedOutput $setupName
        if (-not (Test-Path -LiteralPath $setup -PathType Leaf) -or (Get-Item -LiteralPath $setup).Length -eq 0) { throw 'ISCC.exe did not produce the expected non-empty installer.' }
        $installerStatus = 'built; installation and uninstall not exercised by packaging'
        $artifacts += $setupName
    }
    [ordered]@{
        SchemaVersion = 1
        Version = $Version
        Architecture = 'x64'
        CreatedUtc = $createdUtc
        ExecutableSha256 = $exeHash
        InstallerStatus = $installerStatus
        Validation = 'Artifact packaging only; no desktop or long-duration performance verdict'
        CodeSigning = 'not performed by this script'
        Artifacts = $artifacts
    } | ConvertTo-Json -Depth 3 | Set-Content -LiteralPath (Join-Path $stagedOutput $metadataName) -Encoding UTF8

    $checksumLines = foreach ($name in ($artifacts | Sort-Object)) {
        $hash = (Get-FileHash -LiteralPath (Join-Path $stagedOutput $name) -Algorithm SHA256).Hash.ToLowerInvariant()
        "$hash  $name"
    }
    $checksumLines | Set-Content -LiteralPath (Join-Path $stagedOutput $checksumsName) -Encoding ASCII
    [void][System.IO.Directory]::CreateDirectory($output)
    foreach ($name in ($artifacts + $checksumsName)) {
        Copy-Item -LiteralPath (Join-Path $stagedOutput $name) -Destination (Join-Path $output $name) -ErrorAction Stop
    }
    Write-Host "Packaged $baseName into $output"
    Write-Host "Installer: $installerStatus"
    Write-Host "Checksums: $(Join-Path $output $checksumsName)"
}
finally { if (Test-Path -LiteralPath $tempRoot) { Remove-Item -LiteralPath $tempRoot -Recurse -Force } }
