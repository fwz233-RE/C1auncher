[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$CoreReleaseDirectory,
    [Parameter(Mandatory)]
    [string[]]$AllowedAppDaemonSha256,
    [Parameter(Mandatory)]
    [string]$SigningKeyPath,
    [Parameter(Mandatory)]
    [string]$OutputDirectory,
    [string]$OpenSslPath = 'openssl.exe',
    [string]$WslPath = 'wsl.exe',
    [string]$WslDistribution
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$CoreReleaseDirectory = [IO.Path]::GetFullPath($CoreReleaseDirectory)
$SigningKeyPath = [IO.Path]::GetFullPath($SigningKeyPath)
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
$repositoryPrefix = $projectRoot.TrimEnd('\') + '\'
if ($SigningKeyPath.StartsWith($repositoryPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'The core signing key must be outside the repository working tree.'
}
if (-not (Test-Path -LiteralPath $CoreReleaseDirectory -PathType Container)) { throw 'CoreReleaseDirectory is missing.' }
if (-not (Test-Path -LiteralPath $SigningKeyPath -PathType Leaf)) { throw 'SigningKeyPath is missing.' }
foreach ($item in @(Get-Item -Force -LiteralPath $CoreReleaseDirectory, $SigningKeyPath)) {
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw 'Enrollment inputs may not be links.' }
}
if ($AllowedAppDaemonSha256.Count -lt 1 -or $AllowedAppDaemonSha256.Count -gt 16) {
    throw 'One to sixteen explicit app_daemon baseline hashes are required.'
}
$allowed = @($AllowedAppDaemonSha256 | ForEach-Object {
    $value = $_.ToLowerInvariant()
    if ($value -cnotmatch '^[0-9a-f]{64}$') { throw 'An app_daemon baseline SHA-256 is invalid.' }
    $value
} | Sort-Object -Unique)
if ($allowed.Count -ne $AllowedAppDaemonSha256.Count) { throw 'App daemon baseline hashes must be unique.' }

function Resolve-Executable([string]$Requested, [string[]]$Candidates) {
    $command = Get-Command $Requested -ErrorAction SilentlyContinue
    if ($null -ne $command) { return $command.Source }
    $candidate = $Candidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
    if ([string]::IsNullOrWhiteSpace($candidate)) { throw "Required executable was not found: $Requested" }
    return $candidate
}

function Invoke-QuietNative {
    param([Parameter(Mandatory)][string]$FilePath, [Parameter(Mandatory)][string[]]$Arguments)
    $oldPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $discarded = & $FilePath @Arguments 2>&1
        $exitCode = $LASTEXITCODE
    } finally { $ErrorActionPreference = $oldPreference }
    if ($exitCode -ne 0) {
        $details = (($discarded | ForEach-Object { $_.ToString() }) -join "`n").Trim()
        throw "A required external command failed with exit code $exitCode.`n$details"
    }
}

function Convert-ToWslPath([string]$Path) {
    $match = [regex]::Match([IO.Path]::GetFullPath($Path), '^([A-Za-z]):\\(.*)$')
    if (-not $match.Success) { throw 'WSL validation requires a local Windows drive path.' }
    return '/mnt/' + $match.Groups[1].Value.ToLowerInvariant() + '/' + $match.Groups[2].Value.Replace('\', '/')
}

function Get-Sha256([string]$Path) {
    return (Get-FileHash -Algorithm SHA256 -LiteralPath $Path).Hash.ToLowerInvariant()
}

$openssl = Resolve-Executable $OpenSslPath @(
    (Join-Path $env:ProgramFiles 'Git\usr\bin\openssl.exe'),
    (Join-Path $env:ProgramFiles 'Git\mingw64\bin\openssl.exe')
)
$wsl = (Get-Command $WslPath -ErrorAction Stop).Source
$deviceScript = Join-Path $PSScriptRoot 'device-core-enroll.sh'
$bootstrapScript = Join-Path $PSScriptRoot 'app-daemon-bootstrap.sh'
$validator = Join-Path $PSScriptRoot 'validate-core-release.sh'
foreach ($required in @($deviceScript, $bootstrapScript, $validator)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) { throw "Required enrollment source is missing: $required" }
    if (((Get-Item -Force -LiteralPath $required).Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        throw 'Enrollment sources may not be links.'
    }
}
if (Test-Path -LiteralPath $OutputDirectory) { throw 'OutputDirectory already exists; enrollment bundles are immutable.' }
$outputParent = Split-Path -Parent $OutputDirectory
[IO.Directory]::CreateDirectory($outputParent) | Out-Null
$work = Join-Path $outputParent ('.core-enrollment-' + [Guid]::NewGuid().ToString('N'))
$pem = Join-Path $work 'core.ed25519.pem'
$der = Join-Path $work '.core.ed25519.der'
$raw = Join-Path $work 'core.ed25519.pub'
[IO.Directory]::CreateDirectory($work) | Out-Null
try {
    Invoke-QuietNative $openssl @('pkey', '-in', $SigningKeyPath, '-pubout', '-out', $pem)
    Invoke-QuietNative $openssl @('pkey', '-in', $SigningKeyPath, '-pubout', '-outform', 'DER', '-out', $der)
    $derBytes = [IO.File]::ReadAllBytes($der)
    $prefix = [byte[]](0x30,0x2a,0x30,0x05,0x06,0x03,0x2b,0x65,0x70,0x03,0x21,0x00)
    if ($derBytes.Length -ne 44) { throw 'The core signing key is not canonical Ed25519.' }
    for ($i = 0; $i -lt $prefix.Length; $i++) {
        if ($derBytes[$i] -ne $prefix[$i]) { throw 'The core signing key is not Ed25519.' }
    }
    $rawBytes = [byte[]]::new(32)
    [Array]::Copy($derBytes, 12, $rawBytes, 0, 32)
    [IO.File]::WriteAllBytes($raw, $rawBytes)
    Remove-Item -Force -LiteralPath $der

    $wslArguments = @()
    if (-not [string]::IsNullOrWhiteSpace($WslDistribution)) { $wslArguments += @('--distribution', $WslDistribution) }
    $wslArguments += @('--', 'bash', (Convert-ToWslPath $validator), (Convert-ToWslPath $CoreReleaseDirectory), (Convert-ToWslPath $pem))
    Invoke-QuietNative $wsl $wslArguments

    $releaseTarget = Join-Path $work 'release'
    [IO.Directory]::CreateDirectory((Join-Path $releaseTarget 'artifacts')) | Out-Null
    foreach ($relative in @(
        'manifest.v1', 'manifest.v1.sig', 'artifacts\C1ancher', 'artifacts\c1pkg',
        'artifacts\C1ancher-launcher', 'artifacts\c1updater'
    )) {
        [IO.File]::Copy((Join-Path $CoreReleaseDirectory $relative), (Join-Path $releaseTarget $relative), $false)
    }
    [IO.File]::WriteAllText(
        (Join-Path $work 'device-core-enroll.sh'),
        [IO.File]::ReadAllText($deviceScript).Replace("`r`n", "`n"),
        [Text.UTF8Encoding]::new($false)
    )
    [IO.File]::WriteAllText(
        (Join-Path $work 'app-daemon-bootstrap.sh'),
        [IO.File]::ReadAllText($bootstrapScript).Replace("`r`n", "`n"),
        [Text.UTF8Encoding]::new($false)
    )
    $launch = @'
#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
exec "$root/device-core-enroll.sh" install "$root"
'@.Replace("`r`n", "`n")
    [IO.File]::WriteAllText((Join-Path $work 'enroll.sh'), $launch + "`n", [Text.UTF8Encoding]::new($false))

    # The signed updater is also copied into a fixed, read-only recovery
    # verifier by enrollment. It is deliberately outside the four-component
    # release update contract and can change only during trusted maintenance.
    $updaterBytes = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes((Join-Path $releaseTarget 'artifacts\c1updater')))
    if (-not $updaterBytes.Contains('C1RECOVERY-VERIFIER 1.1.0')) {
        throw 'Enrollment requires the independently installed recovery-verifier 1.1.0 implementation.'
    }
    $manifestLines = @(
        'C1CORE-BOOTSTRAP 1',
        "V`t1.1.0",
        ("K`t" + (Get-Sha256 $raw)),
        ("P`t" + (Get-Sha256 $pem)),
        ("B`t" + (Get-Sha256 (Join-Path $work 'app-daemon-bootstrap.sh'))),
        ("D`t" + (Get-Sha256 (Join-Path $work 'device-core-enroll.sh'))),
        ("L`t" + (Get-Sha256 (Join-Path $work 'enroll.sh'))),
        ("M`t" + (Get-Sha256 (Join-Path $releaseTarget 'manifest.v1'))),
        ("G`t" + (Get-Sha256 (Join-Path $releaseTarget 'manifest.v1.sig'))),
        ("U`t" + (Get-Sha256 (Join-Path $releaseTarget 'artifacts\c1updater')))
    )
    foreach ($hash in $allowed) { $manifestLines += "H`t$hash" }
    foreach ($line in $manifestLines) {
        if ($line -cnotmatch '^[\x20-\x7e\t]+$' -or $line.Length -gt 512) { throw 'Bootstrap manifest is not canonical ASCII.' }
    }
    $manifest = Join-Path $work 'bootstrap.v1'
    $signature = Join-Path $work 'bootstrap.v1.sig'
    [IO.File]::WriteAllText($manifest, (($manifestLines -join "`n") + "`n"), [Text.Encoding]::ASCII)
    Invoke-QuietNative $openssl @('pkeyutl', '-sign', '-rawin', '-inkey', $SigningKeyPath, '-in', $manifest, '-out', $signature)
    if ((Get-Item -LiteralPath $signature).Length -ne 64) { throw 'Bootstrap signature is not exactly 64 bytes.' }
    Invoke-QuietNative $openssl @('pkeyutl', '-verify', '-rawin', '-pubin', '-inkey', $pem, '-in', $manifest, '-sigfile', $signature)
    [IO.Directory]::Move($work, $OutputDirectory)
    $work = $null
    Write-Host "Created signed core enrollment bundle: $OutputDirectory"
    Write-Host "Bootstrap manifest SHA-256: $(Get-Sha256 (Join-Path $OutputDirectory 'bootstrap.v1'))"
} finally {
    if ($null -ne $work -and (Test-Path -LiteralPath $work)) { Remove-Item -Recurse -Force -LiteralPath $work }
}