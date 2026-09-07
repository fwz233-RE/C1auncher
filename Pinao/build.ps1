[CmdletBinding()]
param([string]$Version = '0.1.4', [string]$OutputPath)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if ($Version -cnotmatch '^(0|[1-9][0-9]*)(\.(0|[1-9][0-9]*)){1,3}$') { throw 'Invalid numeric version.' }
if ([string]::IsNullOrWhiteSpace($OutputPath)) { $OutputPath = Join-Path $PSScriptRoot 'build\pinao' }
$OutputPath = [IO.Path]::GetFullPath($OutputPath)
[void][IO.Directory]::CreateDirectory((Split-Path -Parent $OutputPath))
Push-Location $PSScriptRoot
$old = @{}; foreach ($name in @('GOOS','GOARCH','GOMIPS','CGO_ENABLED')) { $old[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
try {
 $env:GOOS='windows'; $env:GOARCH='amd64'; $env:CGO_ENABLED='0'; Remove-Item Env:GOMIPS -ErrorAction SilentlyContinue
 go test ./...
 if ($LASTEXITCODE -ne 0) { throw 'Host tests failed.' }
 go vet ./...
 if ($LASTEXITCODE -ne 0) { throw 'Go vet failed.' }
 $env:GOOS='linux'; $env:GOARCH='mipsle'; $env:GOMIPS='hardfloat'
 go build -trimpath -ldflags "-s -w -X main.version=$Version" -o $OutputPath .
 if ($LASTEXITCODE -ne 0) { throw 'MIPS build failed.' }
} finally {
 foreach ($name in $old.Keys) { [Environment]::SetEnvironmentVariable($name,$old[$name],'Process') }; Pop-Location
}
# Native Go verifier keeps ABI validation available when WSL is unavailable.
Push-Location $PSScriptRoot
try {
 go run ./tests/check-elf $OutputPath
 if ($LASTEXITCODE -ne 0) { throw 'MIPS ABI verification failed.' }
} finally { Pop-Location }
Write-Output "Built pinao $Version : $OutputPath"
