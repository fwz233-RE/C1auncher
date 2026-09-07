[CmdletBinding()]
param(
    [string]$OutputDirectory,
    [string]$DotnetPath
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if (-not $DotnetPath) {
    $command = Get-Command dotnet -ErrorAction SilentlyContinue
    if ($command) { $DotnetPath = $command.Source }
    else { $DotnetPath = Join-Path $env:ProgramFiles 'dotnet/dotnet.exe' }
}
if (-not (Test-Path -LiteralPath $DotnetPath -PathType Leaf)) { throw 'Install .NET SDK 8 or supply -DotnetPath.' }
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $PSScriptRoot ('../build/installer-gui-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
$OutputDirectory = [System.IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $OutputDirectory) { throw 'Output already exists. Choose a NEW output directory; existing releases are never overwritten.' }
& $DotnetPath publish (Join-Path $PSScriptRoot 'C1SlimInstaller.csproj') `
    --configuration Release --runtime win-x64 --self-contained true --nologo `
    -p:DebugType=None -p:DebugSymbols=false --output $OutputDirectory
if ($LASTEXITCODE -ne 0) { throw "Installer publish failed with exit code $LASTEXITCODE" }
if (-not (Test-Path -LiteralPath (Join-Path $OutputDirectory 'C1SlimInstaller.exe') -PathType Leaf)) { throw 'Installer EXE was not produced.' }
Write-Host "Built Windows x64 self-contained installer: $OutputDirectory"
Write-Host 'Core and publisher files are NOT embedded. Assemble EXE + payload with build-bundle.py.'
Write-Host 'This build does not run ADB, connect to devices, or deploy servers.'
