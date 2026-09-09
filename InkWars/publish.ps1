[CmdletBinding()]
param(
    [switch]$AllowInsecureHttp,
    [string]$TokenFile = "$env:LOCALAPPDATA\C1ancher\publisher-tokens\fwz233.token"
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$exe = Join-Path $root 'build\publisher\c1publish.exe'
$key = Join-Path (Split-Path $root) 'C1ancher\config\app-repo\repository.ed25519.pub'
if (-not $AllowInsecureHttp) { throw 'Explicit -AllowInsecureHttp is required: HTTP exposes the publisher token.' }
if (-not (Test-Path $exe)) { throw 'Build c1publish 1.1.0 or later into build/publisher first; it must support -mode.' }
$version = [regex]::Match([IO.File]::ReadAllText((Join-Path $root 'src\version.h')), '#define IW_VERSION "([0-9.]+)"').Groups[1].Value
$next = (& $exe -server http://www.fwz233.com -public-key $key -id inkwars -next-version | Out-String).Trim()
if ($LASTEXITCODE -ne 0 -or $next -ne $version) { throw "Server next version is $next, local version is $version. Rebuild before uploading." }
& py -3 (Join-Path $root 'tools\prepare_upload.py')
if ($LASTEXITCODE -ne 0) { throw 'Fresh payload preparation failed.' }
$stage = Get-ChildItem (Join-Path $root 'build') -Directory -Filter "publish-$version-*" | Sort-Object Name -Descending | Select-Object -First 1
# The explicit mode is mandatory. Older publishers lacking this flag fail closed.
& $exe -server http://www.fwz233.com -public-key $key -allow-insecure-http -token-file $TokenFile -id inkwars -version $version -name 'Ink Wars' -entry inkwars -mode direct -payload (Join-Path $stage.FullName 'payload')
if ($LASTEXITCODE -ne 0) { throw 'Publication failed. Preserve the staged payload for an identical retry.' }
Write-Host 'Upload accepted. Verify the signed catalog and package hash before announcing success.'
