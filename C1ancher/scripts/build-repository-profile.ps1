[CmdletBinding()]
param(
    [string]$RepositoryUrl = 'http://www.fwz233.com/c1/v2',
    [string]$CoreRepositoryUrl = 'http://www.fwz233.com/c1/core/v1/stable',
    [Parameter(Mandatory)][string]$PublicKeyPath,
    [Parameter(Mandatory)][string]$OutputDirectory
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
foreach ($value in @($RepositoryUrl, $CoreRepositoryUrl)) {
    $uri = $null
    if (-not [Uri]::TryCreate($value, [UriKind]::Absolute, [ref]$uri) -or
        $uri.Scheme -notin @('http', 'https') -or $uri.UserInfo -ne '' -or
        $uri.Query -ne '' -or $uri.Fragment -ne '' -or $value -match '[^\x21-\x7e]' -or
        $value -match "['\\]" -or $value.Length -gt 1024) { throw 'Provide a plain HTTP(S) repository URL without credentials, query or fragment.' }
}
$key = [IO.File]::ReadAllBytes([IO.Path]::GetFullPath($PublicKeyPath))
if ($key.Length -ne 32) { throw 'PublicKeyPath must contain the existing raw 32-byte application public key.' }
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $OutputDirectory) { throw 'Output directory already exists; profiles are immutable.' }
$utf8 = [Text.UTF8Encoding]::new($false)
[IO.Directory]::CreateDirectory($OutputDirectory) | Out-Null
[IO.File]::WriteAllText((Join-Path $OutputDirectory 'repository.url'), $RepositoryUrl.TrimEnd('/') + "`n", $utf8)
[IO.File]::WriteAllText((Join-Path $OutputDirectory 'core-repository.url'), $CoreRepositoryUrl.TrimEnd('/') + "`n", $utf8)
[IO.File]::WriteAllBytes((Join-Path $OutputDirectory 'repository.ed25519.pub'), $key)
foreach ($name in @('device-repository-config.sh', 'c1-update-check.sh')) {
    [IO.File]::WriteAllText((Join-Path $OutputDirectory $name), [IO.File]::ReadAllText((Join-Path $PSScriptRoot $name)).Replace("`r`n", "`n"), $utf8)
}
$hashes = foreach ($name in @('repository.url', 'core-repository.url', 'repository.ed25519.pub', 'device-repository-config.sh', 'c1-update-check.sh')) {
    ((Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $OutputDirectory $name)).Hash.ToLowerInvariant() + '  ' + $name)
}
[IO.File]::WriteAllText((Join-Path $OutputDirectory 'SHA256SUMS'), ($hashes -join "`n") + "`n", $utf8)
Write-Output "Created reusable repository profile: $OutputDirectory"
Write-Output 'This profile contains public configuration only. It can be installed on every supported device.'
