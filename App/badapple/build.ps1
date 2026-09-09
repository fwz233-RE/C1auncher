[CmdletBinding()]
param(
    [string]$Version = '0.1.1',
    [string]$OutputPath,
    [string]$AssetPath
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if ($Version -cnotmatch '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$') { throw 'Version must be numeric major.minor.patch.' }
$appRoot = [IO.Path]::GetFullPath($PSScriptRoot)
if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $OutputPath = Join-Path $appRoot '..\..\build\catalog-payload\badapple\badapple'
}
$OutputPath = [IO.Path]::GetFullPath($OutputPath)
if ($AssetPath) { $AssetPath = (Resolve-Path -LiteralPath $AssetPath).Path }
$embeddedPath = Join-Path $appRoot 'embedded\badapple.bap'
if ($AssetPath) {
    [IO.Directory]::CreateDirectory((Split-Path -Parent $embeddedPath)) | Out-Null
    if ([IO.Path]::GetFullPath($AssetPath) -ne [IO.Path]::GetFullPath($embeddedPath)) {
        Copy-Item -LiteralPath $AssetPath -Destination $embeddedPath -Force
    }
}
if (-not (Test-Path -LiteralPath $embeddedPath -PathType Leaf)) {
    throw 'Built-in animation is required. Supply -AssetPath with the preprocessed badapple.bap; media-free releases are not supported.'
}
[IO.Directory]::CreateDirectory((Split-Path -Parent $OutputPath)) | Out-Null
$variables = @('GOOS', 'GOARCH', 'GOMIPS', 'CGO_ENABLED')
$previous = @{}
foreach ($name in $variables) { $previous[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
Push-Location $appRoot
try {
    $env:GOOS = 'windows'; $env:GOARCH = 'amd64'; $env:CGO_ENABLED = '0'
    Remove-Item Env:GOMIPS -ErrorAction SilentlyContinue
    go test ./...
    if ($LASTEXITCODE -ne 0) { throw 'Host tests failed.' }
    go vet ./...
    if ($LASTEXITCODE -ne 0) { throw 'Host vet failed.' }
    go run . --check $embeddedPath
    if ($LASTEXITCODE -ne 0) { throw 'Invalid built-in animation asset.' }
    $goRoot = (& go env GOROOT).Trim()
    if ($LASTEXITCODE -ne 0) { throw 'Cannot locate Go runtime license.' }
    $sysRoot = (& go list -m -f '{{.Dir}}' golang.org/x/sys).Trim()
    if ($LASTEXITCODE -ne 0) { throw 'Cannot locate x/sys license.' }
    $env:GOOS = 'linux'; $env:GOARCH = 'mipsle'; $env:GOMIPS = 'hardfloat'
    go build -trimpath -ldflags "-s -w -X main.version=$Version" -o $OutputPath .
    if ($LASTEXITCODE -ne 0) { throw 'MIPS build failed.' }
    go vet ./...
    if ($LASTEXITCODE -ne 0) { throw 'MIPS vet failed.' }
} finally {
    foreach ($name in $variables) { [Environment]::SetEnvironmentVariable($name, $previous[$name], 'Process') }
    Pop-Location
}
# Validate the ELF header locally, without requiring WSL.
$binary = [IO.File]::ReadAllBytes($OutputPath)
if ($binary.Length -lt 52 -or $binary[0] -ne 0x7f -or $binary[1] -ne 69 -or $binary[2] -ne 76 -or $binary[3] -ne 70 -or $binary[4] -ne 1 -or $binary[5] -ne 1 -or [BitConverter]::ToUInt16($binary,18) -ne 8) {
    throw 'Expected ELF32 little-endian MIPS.'
}
$phoff = [BitConverter]::ToUInt32($binary,28)
$phsize = [BitConverter]::ToUInt16($binary,42)
$phnum = [BitConverter]::ToUInt16($binary,44)
for ($i=0; $i -lt $phnum; $i++) {
    $offset = $phoff + $i * $phsize
    $kind = [BitConverter]::ToUInt32($binary,$offset)
    if ($kind -eq 2 -or $kind -eq 3) { throw 'Dynamic segment/interpreter in device binary.' }
}
$licenseOutput = Join-Path (Split-Path -Parent $OutputPath) 'licenses'
[IO.Directory]::CreateDirectory($licenseOutput) | Out-Null
Copy-Item -LiteralPath (Join-Path $goRoot 'LICENSE') -Destination (Join-Path $licenseOutput 'Go-LICENSE.txt') -Force
Copy-Item -LiteralPath (Join-Path $sysRoot 'LICENSE') -Destination (Join-Path $licenseOutput 'x-sys-LICENSE.txt') -Force
Copy-Item -LiteralPath (Join-Path $appRoot '..\..\C1ancher\LICENSE') -Destination (Join-Path $licenseOutput 'GPL-v3.txt') -Force
Copy-Item -LiteralPath (Join-Path $appRoot 'THIRD_PARTY_NOTICES.md') -Destination (Join-Path $licenseOutput 'THIRD_PARTY_NOTICES.md') -Force
Write-Output 'Animation is embedded in the executable; no external media file is needed on the device.'
Write-Output "Built ELF32 little-endian MIPS, CGO disabled, GOMIPS=hardfloat: $OutputPath"
