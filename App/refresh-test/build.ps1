[CmdletBinding()]
param(
    [string]$Version = '0.1.1',
    [string]$OutputPath
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if ($Version -cnotmatch '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$') { throw 'Version must be numeric major.minor.patch.' }
$appRoot = [IO.Path]::GetFullPath($PSScriptRoot)
if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $OutputPath = Join-Path $appRoot '..\..\build\refresh-test-publish-0.1.1-20260909\payload\refresh-test'
}
$OutputPath = [IO.Path]::GetFullPath($OutputPath)
$payloadRoot = Split-Path -Parent $OutputPath
[IO.Directory]::CreateDirectory($payloadRoot) | Out-Null
$variables = @('GOOS', 'GOARCH', 'GOMIPS', 'CGO_ENABLED')
$previous = @{}
foreach ($name in $variables) { $previous[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
Push-Location $appRoot
try {
    $hostOS = (& go env GOHOSTOS).Trim()
    if ($LASTEXITCODE -ne 0) { throw 'Cannot determine Go host OS.' }
    $hostArch = (& go env GOHOSTARCH).Trim()
    if ($LASTEXITCODE -ne 0) { throw 'Cannot determine Go host architecture.' }
    $env:GOOS = $hostOS; $env:GOARCH = $hostArch; $env:CGO_ENABLED = '0'
    Remove-Item Env:GOMIPS -ErrorAction SilentlyContinue
    go mod download
    if ($LASTEXITCODE -ne 0) { throw 'Module download failed.' }
    go test ./...
    if ($LASTEXITCODE -ne 0) { throw 'Host tests failed.' }
    go vet ./...
    if ($LASTEXITCODE -ne 0) { throw 'Host vet failed.' }
    $goRoot = (& go env GOROOT).Trim()
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($goRoot)) { throw 'Cannot locate Go runtime license.' }
    $sysRoot = (& go list -m -f '{{.Dir}}' golang.org/x/sys).Trim()
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($sysRoot)) { throw 'Cannot locate x/sys license.' }
    $env:GOOS = 'linux'; $env:GOARCH = 'mipsle'; $env:GOMIPS = 'hardfloat'
    go build -trimpath -ldflags "-s -w -X main.version=$Version" -o $OutputPath .
    if ($LASTEXITCODE -ne 0) { throw 'MIPS build failed.' }
    go vet ./...
    if ($LASTEXITCODE -ne 0) { throw 'MIPS vet failed.' }
    $buildInfo = (& go version -m $OutputPath) -join "`n"
    if ($LASTEXITCODE -ne 0) { throw 'Cannot read device binary build settings.' }
    foreach ($setting in @('GOOS=linux', 'GOARCH=mipsle', 'GOMIPS=hardfloat', 'CGO_ENABLED=0')) {
        if ($buildInfo -notmatch ('(?m)^\s*build\s+' + [regex]::Escape($setting) + '\s*$')) {
            throw "Missing required device build setting: $setting"
        }
    }
} finally {
    foreach ($name in $variables) { [Environment]::SetEnvironmentVariable($name, $previous[$name], 'Process') }
    Pop-Location
}
# Validate the ELF header and static program segments without requiring WSL.
$binary = [IO.File]::ReadAllBytes($OutputPath)
if ($binary.Length -lt 52 -or $binary[0] -ne 0x7f -or $binary[1] -ne 69 -or $binary[2] -ne 76 -or $binary[3] -ne 70 -or $binary[4] -ne 1 -or $binary[5] -ne 1 -or $binary[6] -ne 1 -or [BitConverter]::ToUInt16($binary,16) -ne 2 -or [BitConverter]::ToUInt16($binary,18) -ne 8) {
    throw 'Expected an ELF32 little-endian MIPS executable.'
}
$phoff = [uint64][BitConverter]::ToUInt32($binary,28)
$phsize = [uint64][BitConverter]::ToUInt16($binary,42)
$phnum = [uint64][BitConverter]::ToUInt16($binary,44)
if ($phsize -lt 32 -or $phnum -eq 0 -or $phoff -lt 52 -or ($phoff + $phsize * $phnum) -gt $binary.LongLength) {
    throw 'Invalid ELF program header table.'
}
for ($i=0; $i -lt $phnum; $i++) {
    $offset = [int]($phoff + $i * $phsize)
    $kind = [BitConverter]::ToUInt32($binary,$offset)
    if ($kind -eq 2 -or $kind -eq 3) { throw 'Dynamic segment/interpreter in device binary.' }
}
$licenseOutput = Join-Path $payloadRoot 'licenses'
[IO.Directory]::CreateDirectory($licenseOutput) | Out-Null
Copy-Item -LiteralPath (Join-Path $goRoot 'LICENSE') -Destination (Join-Path $licenseOutput 'Go-LICENSE.txt') -Force
Copy-Item -LiteralPath (Join-Path $sysRoot 'LICENSE') -Destination (Join-Path $licenseOutput 'x-sys-LICENSE.txt') -Force
Copy-Item -LiteralPath (Join-Path $appRoot '..\..\C1ancher\LICENSE') -Destination (Join-Path $licenseOutput 'GPL-v3.txt') -Force
Copy-Item -LiteralPath (Join-Path $appRoot 'THIRD_PARTY_NOTICES.md') -Destination (Join-Path $licenseOutput 'THIRD_PARTY_NOTICES.md') -Force
Copy-Item -LiteralPath (Join-Path $appRoot 'README.md') -Destination (Join-Path $payloadRoot 'README.md') -Force
Write-Output "Built ELF32 little-endian MIPS, CGO disabled, GOMIPS=hardfloat: $OutputPath"
Write-Output "README and licenses copied to: $payloadRoot"
