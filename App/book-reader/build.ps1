[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Version,
    [string]$OutputPath
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$appRoot = [IO.Path]::GetFullPath($PSScriptRoot)
$projectRoot = [IO.Path]::GetFullPath((Join-Path $appRoot '..\..'))
if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $OutputPath = Join-Path $projectRoot "build\book-reader-$Version\payload\book-reader"
}
$OutputPath = [IO.Path]::GetFullPath($OutputPath)
$payloadRoot = Split-Path -Parent $OutputPath
if ($Version -cnotmatch '^(0|[1-9][0-9]*)(\.(0|[1-9][0-9]*)){1,3}$') { throw 'Version must use 2-4 numeric components.' }
[IO.Directory]::CreateDirectory($payloadRoot) | Out-Null
$goCommand = Get-Command go -ErrorAction SilentlyContinue
$go = 'C:\Program Files\Go\bin\go.exe'
if ($null -ne $goCommand) { $go = $goCommand.Source }
Push-Location $appRoot
$oldGOOS = $env:GOOS
$oldGOARCH = $env:GOARCH
$oldGOMIPS = $env:GOMIPS
$oldCGO = $env:CGO_ENABLED
try {
    $env:GOOS = 'windows'; $env:GOARCH = 'amd64'; $env:CGO_ENABLED = '0'
    Remove-Item Env:GOMIPS -ErrorAction SilentlyContinue
    & $go test ./...
    if ($LASTEXITCODE -ne 0) { throw 'Host tests failed.' }
    & $go vet ./...
    if ($LASTEXITCODE -ne 0) { throw 'Host vet failed.' }
    $env:GOOS = 'linux'; $env:GOARCH = 'mipsle'; $env:GOMIPS = 'hardfloat'
    & $go build -trimpath -ldflags "-s -w -X main.version=$Version" -o $OutputPath .
    if ($LASTEXITCODE -ne 0) { throw 'MIPS Go build failed.' }
    Copy-Item -LiteralPath (Join-Path $appRoot 'assets\font-LICENSE.txt') -Destination (Join-Path $payloadRoot 'font-LICENSE.txt') -Force
    Copy-Item -LiteralPath (Join-Path $projectRoot 'C1ancher\LICENSE') -Destination (Join-Path $payloadRoot 'LICENSE') -Force
    foreach ($name in @('README.md','CHANGELOG.md','THIRD_PARTY_NOTICES.md','VALIDATION.md')) {
        Copy-Item -LiteralPath (Join-Path $appRoot $name) -Destination (Join-Path $payloadRoot $name) -Force
    }
} finally {
    $env:GOOS = $oldGOOS; $env:GOARCH = $oldGOARCH; $env:GOMIPS = $oldGOMIPS; $env:CGO_ENABLED = $oldCGO
    Pop-Location
}
$wslPath = '/mnt/' + $OutputPath.Substring(0, 1).ToLowerInvariant() + '/' + $OutputPath.Substring(3).Replace('\', '/')
wsl.exe -d Ubuntu-22.04 -- sh -lc "set -eu; file '$wslPath' | grep -q 'ELF 32-bit LSB executable, MIPS'; file '$wslPath' | grep -q 'statically linked'; readelf -A '$wslPath' | grep -q 'Hard float (double precision)'; ! readelf -l '$wslPath' | grep -q INTERP"
if ($LASTEXITCODE -ne 0) { throw 'The book-reader binary failed MIPS ABI validation.' }
Write-Output $OutputPath
