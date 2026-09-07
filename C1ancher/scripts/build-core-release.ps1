[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateScript({ $_ -gt 0 })]
    [uint64]$Sequence,
    [Parameter(Mandatory)]
    [string]$Version,
    [Parameter(Mandatory)]
    [ValidateScript({ $_ -gt 0 })]
    [uint64]$SecurityEpoch,
    [string]$SourceRevision,
    [Parameter(Mandatory)]
    [ValidateRange(1, 253402300799)]
    [long]$SourceDateEpoch,
    # A normal four-component release never installs /etc/app_daemon,
    # bootstrap.version or recovery-verifier. Raising these minimums requires
    # separately approved maintenance of already enrolled devices.
    [ValidatePattern('^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$')]
    [string]$MinimumBootstrap = '1.0.0',
    [ValidatePattern('^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$')]
    [string]$MinimumUpdater = '1.0.0',
    [string]$Compatibility = 'c1-core-v1',
    [string]$BuildDirectory,
    [string]$OutputDirectory,
    [string]$SigningKeyPath = (Join-Path $env:LOCALAPPDATA 'C1ancher\secrets\core-repo-ed25519.pem'),
    [string]$PublicKeyPemOutputPath,
    [string]$PublicKeyRawOutputPath,
    [string]$OpenSslPath = 'openssl.exe',
    [string]$WslPath = 'wsl.exe',
    [string]$WslDistribution,
    [string]$CrossCompile = 'mipsel-linux-gnu-',
    [switch]$AllowDirtySource,
    [switch]$SkipBuild,
    [switch]$InitializeSigningKey
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
if ([string]::IsNullOrWhiteSpace($BuildDirectory)) { $BuildDirectory = Join-Path $projectRoot 'build' }
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $projectRoot ("build\core-release\{0}-{1}" -f $Sequence, $Version)
}
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
$SigningKeyPath = [IO.Path]::GetFullPath($SigningKeyPath)
if (-not [string]::IsNullOrWhiteSpace($PublicKeyPemOutputPath)) {
    $PublicKeyPemOutputPath = [IO.Path]::GetFullPath($PublicKeyPemOutputPath)
}
if (-not [string]::IsNullOrWhiteSpace($PublicKeyRawOutputPath)) {
    $PublicKeyRawOutputPath = [IO.Path]::GetFullPath($PublicKeyRawOutputPath)
}
$outputPrefix = $OutputDirectory.TrimEnd('\') + '\'
foreach ($publicOutput in @($PublicKeyPemOutputPath, $PublicKeyRawOutputPath)) {
    if (-not [string]::IsNullOrWhiteSpace($publicOutput) -and
        ([string]::Equals($publicOutput, $OutputDirectory, [StringComparison]::OrdinalIgnoreCase) -or $publicOutput.StartsWith($outputPrefix, [StringComparison]::OrdinalIgnoreCase))) {
        throw 'Public trust roots must not be embedded in a core release directory.'
    }
}
$repositoryPrefix = $projectRoot.TrimEnd('\') + '\'
if ($SigningKeyPath.StartsWith($repositoryPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'The core signing key must be outside the repository working tree.'
}

$git = (Get-Command 'git.exe' -ErrorAction Stop).Source
function Get-SourceIdentity {
    $resolvedHead = ((& $git -C $projectRoot rev-parse --verify HEAD 2>$null) | Select-Object -First 1).Trim()
    if ($LASTEXITCODE -ne 0 -or $resolvedHead -cnotmatch '^[0-9a-f]{40}$') { throw 'Unable to resolve the source Git revision.' }
    $entries = @(& $git -C $projectRoot status --porcelain --untracked-files=all 2>$null)
    if ($LASTEXITCODE -ne 0) { throw 'Unable to inspect the source Git worktree.' }
    return [pscustomobject]@{
        Head = $resolvedHead
        Dirty = ($entries.Count -ne 0)
        Revision = $(if ($entries.Count -eq 0) { $resolvedHead } else { "$resolvedHead-dirty" })
    }
}

$sourceIdentity = Get-SourceIdentity
if ($sourceIdentity.Dirty -and -not $AllowDirtySource) {
    throw 'The source worktree is dirty. Commit the intended release or use -AllowDirtySource for non-production testing.'
}
$derivedRevision = $sourceIdentity.Revision
if ([string]::IsNullOrWhiteSpace($SourceRevision)) {
    $SourceRevision = $derivedRevision
} elseif ($SourceRevision -cne $derivedRevision) {
    throw 'SourceRevision does not match the inspected Git worktree identity.'
}

function Test-ProtocolToken([string]$Value) {
    return -not [string]::IsNullOrEmpty($Value) -and $Value.Length -le 64 -and
        $Value -cmatch '^[A-Za-z0-9](?:[A-Za-z0-9._+-]*[A-Za-z0-9])?$'
}

foreach ($token in @($Version, $MinimumBootstrap, $MinimumUpdater, $Compatibility, $SourceRevision)) {
    if (-not (Test-ProtocolToken $token)) { throw "Invalid core manifest token: $token" }
}
if ($CrossCompile -cnotmatch '^[A-Za-z0-9_./+-]+$') { throw 'CrossCompile contains unsupported characters.' }
$versionPath = Join-Path $projectRoot 'VERSION'
if (-not (Test-Path -LiteralPath $versionPath -PathType Leaf)) { throw 'VERSION is missing.' }
$sourceVersion = [IO.File]::ReadAllText($versionPath, [Text.Encoding]::ASCII).Trim()
if ($sourceVersion -cne $Version) { throw "Release version $Version does not match VERSION $sourceVersion." }

function Resolve-Executable([string]$Requested, [string[]]$Candidates) {
    $command = Get-Command $Requested -ErrorAction SilentlyContinue
    if ($null -ne $command) { return $command.Source }
    $candidate = $Candidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
    if ([string]::IsNullOrWhiteSpace($candidate)) { throw "Required executable was not found: $Requested" }
    return $candidate
}

$openssl = Resolve-Executable $OpenSslPath @(
    (Join-Path $env:ProgramFiles 'Git\usr\bin\openssl.exe'),
    (Join-Path $env:ProgramFiles 'Git\mingw64\bin\openssl.exe')
)
$wsl = (Get-Command $WslPath -ErrorAction Stop).Source
$ascii = [Text.Encoding]::ASCII

function Invoke-QuietNative {
    param([Parameter(Mandatory)][string]$FilePath, [Parameter(Mandatory)][string[]]$Arguments)
    $oldPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $discarded = & $FilePath @Arguments 2>&1
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $oldPreference
    }
    if ($exitCode -ne 0) {
        $details = (($discarded | ForEach-Object { $_.ToString() }) -join "`n").Trim()
        if ([string]::IsNullOrEmpty($details)) { $details = 'The command produced no output.' }
        throw "A required external command failed with exit code $exitCode.`n$details"
    }
}

function Protect-PrivateKey([string]$Path) {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent().User
    if ($null -eq $identity) { throw 'Unable to identify the current Windows user.' }
    $security = [Security.AccessControl.FileSecurity]::new()
    $security.SetOwner($identity)
    $security.SetAccessRuleProtection($true, $false)
    [void]$security.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new(
        $identity,
        [Security.AccessControl.FileSystemRights]::FullControl,
        [Security.AccessControl.AccessControlType]::Allow
    ))
    [IO.File]::SetAccessControl($Path, $security)
}

function Convert-ToWslPath([string]$Path) {
    $full = [IO.Path]::GetFullPath($Path)
    $match = [regex]::Match($full, '^([A-Za-z]):\\(.*)$')
    if (-not $match.Success) { throw 'WSL validation requires a local Windows drive path.' }
    return '/mnt/' + $match.Groups[1].Value.ToLowerInvariant() + '/' + $match.Groups[2].Value.Replace('\', '/')
}

if (-not $SkipBuild) {
    & (Join-Path $PSScriptRoot 'build.ps1') -CrossCompile $CrossCompile
    $postBuildIdentity = Get-SourceIdentity
    if ($postBuildIdentity.Head -cne $sourceIdentity.Head -or $postBuildIdentity.Revision -cne $sourceIdentity.Revision) {
        throw 'The source identity changed during the build; refusing to sign artifacts.'
    }
}

$components = @(
    [pscustomobject]@{ Role = 'c1ancher'; Name = 'C1ancher' },
    [pscustomobject]@{ Role = 'c1pkg'; Name = 'c1pkg' },
    [pscustomobject]@{ Role = 'launcher'; Name = 'C1ancher-launcher' },
    [pscustomobject]@{ Role = 'updater'; Name = 'c1updater' }
)
foreach ($component in $components) {
    $source = Join-Path $BuildDirectory $component.Name
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "Missing core artifact: $source" }
    $item = Get-Item -Force -LiteralPath $source
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or $item.Length -le 0 -or $item.Length -gt 33554432) {
        throw "Core artifact metadata rejected: $source"
    }
    Add-Member -InputObject $component -NotePropertyName Source -NotePropertyValue $source
    Add-Member -InputObject $component -NotePropertyName Size -NotePropertyValue ([uint64]$item.Length)
    Add-Member -InputObject $component -NotePropertyName Digest -NotePropertyValue ((Get-FileHash -Algorithm SHA256 -LiteralPath $source).Hash.ToLowerInvariant())
}
$payloadSize = [uint64]0
foreach ($component in $components) { $payloadSize += $component.Size }
if ($payloadSize -gt 100663296) { throw 'Core release payload exceeds 96 MiB.' }

if (-not (Test-Path -LiteralPath $SigningKeyPath -PathType Leaf)) {
    if (-not $InitializeSigningKey) { throw 'The core signing key is missing. Use -InitializeSigningKey only for first-time key creation.' }
    $keyParent = Split-Path -Parent $SigningKeyPath
    [IO.Directory]::CreateDirectory($keyParent) | Out-Null
    $temporaryKey = Join-Path $keyParent ('.new-core-key-' + [Guid]::NewGuid().ToString('N'))
    try {
        [IO.File]::WriteAllBytes($temporaryKey, [byte[]]@())
        Protect-PrivateKey $temporaryKey
        Invoke-QuietNative $openssl @('genpkey', '-algorithm', 'ED25519', '-out', $temporaryKey)
        Protect-PrivateKey $temporaryKey
        [IO.File]::Move($temporaryKey, $SigningKeyPath)
    } finally {
        Remove-Item -Force -ErrorAction SilentlyContinue -LiteralPath $temporaryKey
    }
}
$keyItem = Get-Item -Force -LiteralPath $SigningKeyPath
if (($keyItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw 'The core signing key may not be a link.' }
Protect-PrivateKey $SigningKeyPath

$outputParent = Split-Path -Parent $OutputDirectory
[IO.Directory]::CreateDirectory($outputParent) | Out-Null
if (Test-Path -LiteralPath $OutputDirectory) { throw 'OutputDirectory already exists; core releases are immutable.' }
$work = Join-Path $outputParent ('.core-release-' + [Guid]::NewGuid().ToString('N'))
$publicKey = Join-Path ([IO.Path]::GetTempPath()) ('.c1-core-public-' + [Guid]::NewGuid().ToString('N') + '.pem')
[IO.Directory]::CreateDirectory((Join-Path $work 'artifacts')) | Out-Null
try {
    foreach ($component in $components) {
        [IO.File]::Copy($component.Source, (Join-Path $work ('artifacts\' + $component.Name)), $false)
    }
    $manifestLines = @(
        'C1CORE-MANIFEST 1',
        "S`t$Sequence",
        "V`t$Version",
        "E`t$SecurityEpoch",
        "T`tmips32r2-little-o32-hard-float-double-static",
        "B`t$MinimumBootstrap",
        "U`t$MinimumUpdater",
        "C`t$Compatibility",
        "R`t$SourceRevision",
        "D`t$SourceDateEpoch"
    )
    foreach ($component in $components) {
        $manifestLines += "F`t$($component.Role)`tartifacts/$($component.Name)`t$($component.Digest)`t$($component.Size)`t700"
    }
    foreach ($line in $manifestLines) {
        if ($line.Length -gt 512 -or $line -cnotmatch '^[\x20-\x7e\t]+$') { throw 'Manifest line is not protocol-compatible ASCII.' }
    }
    $manifestPath = Join-Path $work 'manifest.v1'
    $signaturePath = Join-Path $work 'manifest.v1.sig'
    [IO.File]::WriteAllText($manifestPath, (($manifestLines -join "`n") + "`n"), $ascii)
    Invoke-QuietNative $openssl @('pkeyutl', '-sign', '-rawin', '-inkey', $SigningKeyPath, '-in', $manifestPath, '-out', $signaturePath)
    if ((Get-Item -LiteralPath $signaturePath).Length -ne 64) { throw 'Ed25519 signature is not exactly 64 bytes.' }
    Invoke-QuietNative $openssl @('pkey', '-in', $SigningKeyPath, '-pubout', '-out', $publicKey)
    $publicDer = Join-Path ([IO.Path]::GetTempPath()) ('.c1-core-public-' + [Guid]::NewGuid().ToString('N') + '.der')
    try {
        Invoke-QuietNative $openssl @('pkey', '-in', $SigningKeyPath, '-pubout', '-outform', 'DER', '-out', $publicDer)
        $der = [IO.File]::ReadAllBytes($publicDer)
        $prefix = [byte[]](0x30,0x2a,0x30,0x05,0x06,0x03,0x2b,0x65,0x70,0x03,0x21,0x00)
        if ($der.Length -ne 44) { throw 'The core trust root is not canonical Ed25519 SPKI.' }
        for ($i = 0; $i -lt $prefix.Length; $i++) {
            if ($der[$i] -ne $prefix[$i]) { throw 'The core trust root is not Ed25519.' }
        }
        $rawPublic = [byte[]]::new(32)
        [Array]::Copy($der, 12, $rawPublic, 0, 32)
        foreach ($output in @(
            [pscustomobject]@{ Path = $PublicKeyPemOutputPath; Bytes = [IO.File]::ReadAllBytes($publicKey) },
            [pscustomobject]@{ Path = $PublicKeyRawOutputPath; Bytes = $rawPublic }
        )) {
            if ([string]::IsNullOrWhiteSpace($output.Path)) { continue }
            [IO.Directory]::CreateDirectory((Split-Path -Parent $output.Path)) | Out-Null
            if (Test-Path -LiteralPath $output.Path) {
                $existingPublic = [IO.File]::ReadAllBytes($output.Path)
                if ([Convert]::ToBase64String($existingPublic) -cne [Convert]::ToBase64String($output.Bytes)) {
                    throw 'A public-key output path already contains a different trust root.'
                }
            } else {
                [IO.File]::WriteAllBytes($output.Path, $output.Bytes)
            }
        }
    } finally {
        Remove-Item -Force -ErrorAction SilentlyContinue -LiteralPath $publicDer
    }
    Invoke-QuietNative $openssl @('pkeyutl', '-verify', '-rawin', '-pubin', '-inkey', $publicKey, '-in', $manifestPath, '-sigfile', $signaturePath)

    $validator = Join-Path $PSScriptRoot 'validate-core-release.sh'
    if (-not (Test-Path -LiteralPath $validator -PathType Leaf)) { throw 'Canonical core release validator is missing.' }
    $arguments = @()
    if (-not [string]::IsNullOrWhiteSpace($WslDistribution)) { $arguments += @('--distribution', $WslDistribution) }
    $arguments += @('--', 'bash', (Convert-ToWslPath $validator), (Convert-ToWslPath $work), (Convert-ToWslPath $publicKey))
    Invoke-QuietNative $wsl $arguments

    [IO.Directory]::Move($work, $OutputDirectory)
    $work = $null
    $manifestDigest = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $OutputDirectory 'manifest.v1')).Hash.ToLowerInvariant()
    Write-Host "Created immutable core release: $OutputDirectory"
    Write-Host "Sequence: $Sequence; version: $Version; manifest SHA-256: $manifestDigest"
} finally {
    if ($null -ne $work -and (Test-Path -LiteralPath $work)) { Remove-Item -Recurse -Force -LiteralPath $work }
    Remove-Item -Force -ErrorAction SilentlyContinue -LiteralPath $publicKey
}