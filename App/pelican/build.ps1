[CmdletBinding()]
param(
    [string]$Version = '0.1.0',
    [string]$OutputPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if ($Version -cnotmatch '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$') {
    throw 'Version must be a three-component numeric version.'
}
if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $OutputPath = Join-Path $PSScriptRoot '..\..\build\catalog-payload\pelican\pelican'
}
$OutputPath = [IO.Path]::GetFullPath($OutputPath)
[IO.Directory]::CreateDirectory((Split-Path -Parent $OutputPath)) | Out-Null
$oldGOOS = $env:GOOS
$oldGOARCH = $env:GOARCH
$oldGOMIPS = $env:GOMIPS
$oldCGO = $env:CGO_ENABLED
Push-Location $PSScriptRoot
try {
    $env:GOOS = 'windows'
    $env:GOARCH = 'amd64'
    $env:CGO_ENABLED = '0'
    Remove-Item Env:GOMIPS -ErrorAction SilentlyContinue
    go test ./...
    if ($LASTEXITCODE -ne 0) { throw 'Host tests failed.' }
    go vet ./...
    if ($LASTEXITCODE -ne 0) { throw 'Go vet failed.' }
    $env:GOOS = 'linux'
    $env:GOARCH = 'mipsle'
    $env:GOMIPS = 'hardfloat'
    go build -trimpath -ldflags "-s -w -X main.version=$Version" -o $OutputPath .
    if ($LASTEXITCODE -ne 0) { throw 'Device build failed.' }
} finally {
    $env:GOOS = $oldGOOS
    $env:GOARCH = $oldGOARCH
    $env:GOMIPS = $oldGOMIPS
    $env:CGO_ENABLED = $oldCGO
    Pop-Location
}
Write-Output $OutputPath
