[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Version,
    [string]$OutputPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if ($Version -cnotmatch '^(0|[1-9][0-9]*)(\.(0|[1-9][0-9]*)){1,3}$') { throw 'Version must use 2-4 numeric components.' }
$appRoot = [IO.Path]::GetFullPath($PSScriptRoot)
$projectRoot = [IO.Path]::GetFullPath((Join-Path $appRoot '..\..'))
if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $OutputPath = Join-Path $projectRoot 'build\catalog-payload\hello\hello'
}
$OutputPath = [IO.Path]::GetFullPath($OutputPath)
[IO.Directory]::CreateDirectory((Split-Path -Parent $OutputPath)) | Out-Null

Push-Location $appRoot
$oldGOOS = $env:GOOS
$oldGOARCH = $env:GOARCH
$oldGOMIPS = $env:GOMIPS
$oldCGO = $env:CGO_ENABLED
try {
    $env:GOOS = 'windows'
    $env:GOARCH = 'amd64'
    Remove-Item Env:GOMIPS -ErrorAction SilentlyContinue
    $env:CGO_ENABLED = '0'
    go test ./...
    if ($LASTEXITCODE -ne 0) { throw 'Host tests failed.' }

    $env:GOOS = 'linux'
    $env:GOARCH = 'mipsle'
    $env:GOMIPS = 'hardfloat'
    $env:CGO_ENABLED = '0'
    go build -trimpath -ldflags "-s -w -X main.version=$Version" -o $OutputPath .
    if ($LASTEXITCODE -ne 0) { throw 'MIPS Go build failed.' }
} finally {
    $env:GOOS = $oldGOOS
    $env:GOARCH = $oldGOARCH
    $env:GOMIPS = $oldGOMIPS
    $env:CGO_ENABLED = $oldCGO
    Pop-Location
}

$wslPath = '/mnt/' + $OutputPath.Substring(0, 1).ToLowerInvariant() + '/' + $OutputPath.Substring(3).Replace('\', '/')
wsl.exe sh -lc "set -eu; file '$wslPath' | grep -q 'ELF 32-bit LSB executable, MIPS'; file '$wslPath' | grep -q 'statically linked'; readelf -A '$wslPath' | grep -q 'Hard float (double precision)'; ! readelf -l '$wslPath' | grep -q INTERP"
if ($LASTEXITCODE -ne 0) { throw 'The hello binary failed MIPS ABI validation.' }
Write-Output $OutputPath