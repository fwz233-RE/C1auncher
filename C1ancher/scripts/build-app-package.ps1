[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$CatalogPath,
    [Parameter(Mandatory)]
    [ValidateScript({ $_ -gt 0 })]
    [uint64]$Sequence,
    [string]$OutputDirectory,
    [string]$SigningKeyPath = (Join-Path $env:LOCALAPPDATA 'C1ancher\secrets\app-repo-ed25519.pem'),
    [string]$OpenSslPath = 'openssl.exe',
    [string]$WslPath = 'wsl.exe',
    [string]$WslDistribution,
    [ValidateRange(0, 253402300799)]
    [long]$SourceDateEpoch = 0,
    [switch]$InitializeSigningKey
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$CatalogPath = [IO.Path]::GetFullPath($CatalogPath)
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $projectRoot ("build\app-repo\{0}" -f $Sequence)
}
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
$SigningKeyPath = [IO.Path]::GetFullPath($SigningKeyPath)
$projectPrefix = $projectRoot.TrimEnd('\') + '\'
if ($SigningKeyPath.StartsWith($projectPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'The private signing key must be outside the repository working tree.'
}
$opensslCommand = Get-Command $OpenSslPath -ErrorAction SilentlyContinue
if ($null -ne $opensslCommand) {
    $openssl = $opensslCommand.Source
} else {
    $opensslCandidates = @(
        (Join-Path $env:ProgramFiles 'Git\usr\bin\openssl.exe'),
        (Join-Path $env:ProgramFiles 'Git\mingw64\bin\openssl.exe')
    )
    $openssl = $opensslCandidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
    if ([string]::IsNullOrWhiteSpace($openssl)) { throw 'OpenSSL was not found. Pass -OpenSslPath explicitly.' }
}
$wsl = (Get-Command $WslPath -ErrorAction Stop).Source
$ascii = [Text.Encoding]::ASCII
$utf8NoBom = [Text.UTF8Encoding]::new($false, $true)

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
    if ($exitCode -ne 0) { throw "A required external command failed with exit code $exitCode." }
}

function Invoke-OpenSsl([string[]]$Arguments) {
    Invoke-QuietNative -FilePath $openssl -Arguments $Arguments
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
    if (-not $match.Success) { throw 'WSL inputs and outputs must be on a local Windows drive.' }
    return '/mnt/' + $match.Groups[1].Value.ToLowerInvariant() + '/' + $match.Groups[2].Value.Replace('\', '/')
}

function Test-SafeToken([string]$Value, [int]$Maximum, [bool]$Version) {
    if ([string]::IsNullOrEmpty($Value) -or $Value.Length -gt $Maximum -or $Value.StartsWith('.') -or $Value.EndsWith('.') -or $Value.Contains('..')) { return $false }
    $pattern = if ($Version) { '^[A-Za-z0-9._+-]+$' } else { '^[A-Za-z0-9._-]+$' }
    return $Value -cmatch $pattern
}

function Test-SafeRelativePath([string]$Value) {
    if ([string]::IsNullOrEmpty($Value) -or $Value.Length -gt 192 -or $Value.StartsWith('/') -or $Value.EndsWith('/')) { return $false }
    if ($Value -cnotmatch '^[\x21-\x7e]+$' -or $Value -match '[\\:*?\[]') { return $false }
    foreach ($component in $Value.Split('/')) {
        if ($component.Length -eq 0 -or $component -eq '.' -or $component -eq '..') { return $false }
    }
    return $true
}

function Sort-Ordinal([object[]]$Values, [scriptblock]$Selector) {
    $result = @($Values)
    for ($i = 0; $i -lt $result.Count; $i++) {
        for ($j = $i + 1; $j -lt $result.Count; $j++) {
            if ([string]::CompareOrdinal((& $Selector $result[$i]), (& $Selector $result[$j])) -gt 0) {
                $temporary = $result[$i]; $result[$i] = $result[$j]; $result[$j] = $temporary
            }
        }
    }
    return $result
}

if (-not (Test-Path -LiteralPath $CatalogPath -PathType Leaf)) { throw 'CatalogPath is missing.' }
$catalogItem = Get-Item -Force -LiteralPath $CatalogPath
if (($catalogItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw 'CatalogPath may not be a link.' }
$catalogText = [IO.File]::ReadAllText($CatalogPath, [Text.UTF8Encoding]::new($false, $true))
try { $catalog = $catalogText | ConvertFrom-Json -ErrorAction Stop } catch { throw 'CatalogPath is not valid strict UTF-8 JSON.' }
if ($null -eq $catalog -or $catalog -isnot [pscustomobject]) { throw 'The catalog root must be an object.' }
$rootProperties = @($catalog.PSObject.Properties.Name)
if ($rootProperties.Count -ne 1 -or $rootProperties[0] -cne 'packages') { throw 'The catalog root must contain only the packages property.' }
if ($catalog.packages -isnot [Array]) { throw 'The packages property must be a JSON array.' }
$catalogPackages = @($catalog.packages)
if ($catalogPackages.Count -gt 128) { throw 'The client supports at most 128 packages.' }

$packages = @()
$seenIds = [Collections.Generic.Dictionary[string,bool]]::new([StringComparer]::Ordinal)
$seenIdsInsensitive = [Collections.Generic.Dictionary[string,bool]]::new([StringComparer]::OrdinalIgnoreCase)
$catalogDirectory = Split-Path -Parent $CatalogPath
foreach ($definition in $catalogPackages) {
    if ($null -eq $definition -or $definition -isnot [pscustomobject]) { throw 'Every package definition must be an object.' }
    $properties = @($definition.PSObject.Properties.Name)
    $required = @('id', 'version', 'displayName', 'entry', 'payloadDirectory')
    if ($properties.Count -ne $required.Count) { throw 'Every package definition must contain exactly id, version, displayName, entry, and payloadDirectory.' }
    foreach ($property in $required) { if ($properties -cnotcontains $property) { throw "Package definition is missing $property." } }
    foreach ($property in $required) { if ($definition.$property -isnot [string]) { throw "$property must be a JSON string." } }

    $id = [string]$definition.id
    $version = [string]$definition.version
    $displayName = [string]$definition.displayName
    $entry = ([string]$definition.entry).Replace('\', '/')
    if (-not (Test-SafeToken $id 32 $false)) { throw "Invalid package ID: $id" }
    if (-not (Test-SafeToken $version 48 $true)) { throw "Invalid package version for $id." }
    if ($displayName.Length -lt 1 -or $displayName.Length -gt 40 -or $displayName -cnotmatch '^[\x21-\x7e](?:[\x20-\x7e]{0,38}[\x21-\x7e])?$') { throw "displayName for $id must be 1-40 printable ASCII characters without surrounding spaces." }
    if (-not (Test-SafeRelativePath $entry)) { throw "Invalid entry path for $id." }
    if ($seenIds.ContainsKey($id)) { throw "Duplicate package ID: $id" }
    if ($seenIdsInsensitive.ContainsKey($id.ToLowerInvariant())) { throw 'Package IDs differing only by case cannot be emitted on Windows.' }
    $seenIds[$id] = $true
    $seenIdsInsensitive[$id.ToLowerInvariant()] = $true

    $payloadDirectory = [string]$definition.payloadDirectory
    if (-not [IO.Path]::IsPathRooted($payloadDirectory)) { $payloadDirectory = Join-Path $catalogDirectory $payloadDirectory }
    $payloadDirectory = [IO.Path]::GetFullPath($payloadDirectory)
    if (-not (Test-Path -LiteralPath $payloadDirectory -PathType Container)) { throw "payloadDirectory for $id is missing." }
    $payloadRootItem = Get-Item -Force -LiteralPath $payloadDirectory
    if (($payloadRootItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw "payloadDirectory for $id may not be a link." }

    $items = @(Get-ChildItem -Force -LiteralPath $payloadDirectory -Recurse)
    $payloadPrefix = $payloadDirectory.TrimEnd('\') + '\'
    $relativeFiles = [Collections.Generic.Dictionary[string,string]]::new([StringComparer]::Ordinal)
    [long]$totalSize = 0
    foreach ($item in $items) {
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw "Package $id contains a link." }
        $relative = $item.FullName.Substring($payloadPrefix.Length).Replace('\', '/')
        if (-not (Test-SafeRelativePath $relative)) { throw "Package $id contains a client-incompatible path." }
        if (-not $item.PSIsContainer) {
            if ($item -isnot [IO.FileInfo]) { throw "Package $id contains a special file." }
            if ($item.Length -gt 16777216) { throw "Package $id contains a file larger than 16 MiB." }
            $totalSize += $item.Length
            if ($totalSize -gt 67108864) { throw "Package $id exceeds the 64 MiB unpacked limit." }
            $relativeFiles[$relative] = $item.FullName
        }
    }
    if (-not $relativeFiles.ContainsKey($entry)) { throw "Entry for $id is not a regular file with exact case." }
    $archive = "packages/$id/$version.tar.gz"
    if ($archive.Length -gt 192) { throw "Archive path for $id exceeds the client limit." }
    $packages += [pscustomobject]@{
        Id = $id; Version = $version; DisplayName = $displayName; Entry = $entry
        PayloadDirectory = $payloadDirectory; Archive = $archive; Items = $items
    }
}
$packages = @(Sort-Ordinal $packages { param($item) $item.Id })

if (-not (Test-Path -LiteralPath $SigningKeyPath -PathType Leaf)) {
    if (-not $InitializeSigningKey) { throw 'The local signing key is missing. Re-run once with -InitializeSigningKey to create it.' }
    $keyDirectory = Split-Path -Parent $SigningKeyPath
    [IO.Directory]::CreateDirectory($keyDirectory) | Out-Null
    $temporaryKey = Join-Path $keyDirectory ('.new-key-' + [Guid]::NewGuid().ToString('N'))
    try {
        [IO.File]::WriteAllBytes($temporaryKey, [byte[]]@())
        Protect-PrivateKey $temporaryKey
        Invoke-OpenSsl -Arguments @('genpkey', '-algorithm', 'ED25519', '-out', $temporaryKey)
        Protect-PrivateKey $temporaryKey
        [IO.File]::Move($temporaryKey, $SigningKeyPath)
    } finally { if (Test-Path -LiteralPath $temporaryKey) { Remove-Item -Force -LiteralPath $temporaryKey } }
}
$keyItem = Get-Item -Force -LiteralPath $SigningKeyPath
if (($keyItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw 'The private signing key may not be a link.' }
Protect-PrivateKey $SigningKeyPath

$outputParent = Split-Path -Parent $OutputDirectory
[IO.Directory]::CreateDirectory($outputParent) | Out-Null
$work = Join-Path $outputParent ('.c1-app-repo-' + [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($work) | Out-Null
try {
    $pemPath = Join-Path $work 'repository-public-key.pem'
    $derPath = Join-Path $work 'repository-public-key.der'
    $rawPublicPath = Join-Path $work 'repository.ed25519.pub'
    Invoke-OpenSsl -Arguments @('pkey', '-in', $SigningKeyPath, '-pubout', '-out', $pemPath)
    Invoke-OpenSsl -Arguments @('pkey', '-in', $SigningKeyPath, '-pubout', '-outform', 'DER', '-out', $derPath)
    $publicText = [IO.File]::ReadAllText($pemPath, $ascii).Replace("`r`n", "`n").Replace("`r", "`n")
    if ($publicText -cnotmatch '\A-----BEGIN PUBLIC KEY-----\n[A-Za-z0-9+/=\n]+-----END PUBLIC KEY-----\n\z') { throw 'OpenSSL returned an invalid public PEM.' }
    [IO.File]::WriteAllText($pemPath, $publicText, $utf8NoBom)
    $der = [IO.File]::ReadAllBytes($derPath)
    $spkiPrefix = [byte[]](0x30,0x2a,0x30,0x05,0x06,0x03,0x2b,0x65,0x70,0x03,0x21,0x00)
    if ($der.Length -ne 44) { throw 'The public key is not the canonical Ed25519 SPKI DER form.' }
    for ($i = 0; $i -lt $spkiPrefix.Length; $i++) { if ($der[$i] -ne $spkiPrefix[$i]) { throw 'The signing key is not Ed25519.' } }
    $rawKey = [byte[]]::new(32)
    [Array]::Copy($der, 12, $rawKey, 0, 32)
    [IO.File]::WriteAllBytes($rawPublicPath, $rawKey)
    Remove-Item -Force -LiteralPath $derPath

    $helperPath = Join-Path $work 'make-package.sh'
    $helper = @'
#!/usr/bin/env bash
set -euo pipefail
source_root=$1
archive=$2
entry=$3
epoch=$4
temporary=$(mktemp -d)
cleanup() { rm -rf -- "$temporary"; rm -f -- "$archive.tmp"; }
trap cleanup EXIT HUP INT TERM
tar --version | head -n 1 | grep -q 'GNU tar'
command -v gzip >/dev/null
mkdir -p -- "$temporary/root"
cp -a --no-preserve=mode,ownership,timestamps -- "$source_root/." "$temporary/root/"
test -f "$temporary/root/payload/$entry" && test ! -L "$temporary/root/payload/$entry"
test "$(find "$temporary/root" -mindepth 1 ! -type f ! -type d -print -quit)" = ''
find "$temporary/root" -type d -exec chmod 0755 {} +
find "$temporary/root" -type f -exec chmod 0644 {} +
chmod 0755 "$temporary/root/payload/$entry"
(cd "$temporary/root" && find manifest.v1 payload -type f -print0 | LC_ALL=C sort -z | tar --format=gnu --sort=name --owner=0 --group=0 --numeric-owner --mtime="@$epoch" --no-recursion --null --verbatim-files-from --files-from=- --create --file=-) | gzip -n -9 >"$archive.tmp"
mv -f -- "$archive.tmp" "$archive"
'@
    [IO.File]::WriteAllText($helperPath, $helper.Replace("`r`n", "`n") + "`n", $utf8NoBom)
    $helperWsl = Convert-ToWslPath $helperPath

    foreach ($package in $packages) {
        $packageRoot = Join-Path $work ('package-' + [Guid]::NewGuid().ToString('N'))
        $payloadRoot = Join-Path $packageRoot 'payload'
        [IO.Directory]::CreateDirectory($payloadRoot) | Out-Null
        $sourcePrefix = $package.PayloadDirectory.TrimEnd('\') + '\'
        foreach ($item in $package.Items) {
            $relativeWindows = $item.FullName.Substring($sourcePrefix.Length)
            $destination = Join-Path $payloadRoot $relativeWindows
            if ($item.PSIsContainer) { [IO.Directory]::CreateDirectory($destination) | Out-Null }
            else {
                [IO.Directory]::CreateDirectory((Split-Path -Parent $destination)) | Out-Null
                [IO.File]::Copy($item.FullName, $destination, $false)
            }
        }
        $manifest = "C1PKG-PACKAGE 1`nid`t$($package.Id)`nversion`t$($package.Version)`nentry`t$($package.Entry)`n"
        [IO.File]::WriteAllText((Join-Path $packageRoot 'manifest.v1'), $manifest, $ascii)
        $archivePath = Join-Path $work ($package.Archive.Replace('/', '\'))
        [IO.Directory]::CreateDirectory((Split-Path -Parent $archivePath)) | Out-Null
        $wslArguments = @()
        if (-not [string]::IsNullOrWhiteSpace($WslDistribution)) { $wslArguments += @('--distribution', $WslDistribution) }
        $wslArguments += @('--', 'bash', $helperWsl, (Convert-ToWslPath $packageRoot), (Convert-ToWslPath $archivePath), $package.Entry, $SourceDateEpoch.ToString())
        Invoke-QuietNative -FilePath $wsl -Arguments $wslArguments
        Remove-Item -Recurse -Force -LiteralPath $packageRoot
        $archiveItem = Get-Item -LiteralPath $archivePath
        if ($archiveItem.Length -le 0 -or $archiveItem.Length -gt 33554432) { throw "Archive for $($package.Id) exceeds the client limit." }
        Add-Member -InputObject $package -NotePropertyName Size -NotePropertyValue ([long]$archiveItem.Length)
        Add-Member -InputObject $package -NotePropertyName Hash -NotePropertyValue ((Get-FileHash -Algorithm SHA256 -LiteralPath $archivePath).Hash.ToLowerInvariant())
    }
    Remove-Item -Force -LiteralPath $helperPath

    $indexLines = @('C1PKG-INDEX 1', "S`t$Sequence")
    foreach ($package in $packages) {
        $indexLines += "P`t$($package.Id)`t$($package.Version)`t$($package.DisplayName)`t$($package.Archive)`t$($package.Hash)`t$($package.Size)`t$($package.Entry)"
    }
    foreach ($line in $indexLines) { if ($line.Length -gt 1024 -or $line -cnotmatch '^[\x20-\x7e\t]+$') { throw 'An index line is not client-compatible ASCII.' } }
    $indexText = ($indexLines -join "`n") + "`n"
    if ($ascii.GetByteCount($indexText) -gt 262144) { throw 'index.v1 exceeds the client limit.' }
    $indexPath = Join-Path $work 'index.v1'
    [IO.File]::WriteAllText($indexPath, $indexText, $ascii)
    $signaturePath = Join-Path $work 'index.v1.sig'
    Invoke-OpenSsl -Arguments @('pkeyutl', '-sign', '-rawin', '-inkey', $SigningKeyPath, '-in', $indexPath, '-out', $signaturePath)
    if ((Get-Item -LiteralPath $signaturePath).Length -ne 64) { throw 'The Ed25519 signature is not exactly 64 bytes.' }
    Invoke-OpenSsl -Arguments @('pkeyutl', '-verify', '-rawin', '-pubin', '-inkey', $pemPath, '-in', $indexPath, '-sigfile', $signaturePath)

    $artifactPaths = @('index.v1', 'index.v1.sig', 'repository-public-key.pem', 'repository.ed25519.pub')
    foreach ($package in $packages) { $artifactPaths += $package.Archive }
    $artifactPaths = @(Sort-Ordinal $artifactPaths { param($item) [string]$item })
    $checksumLines = foreach ($relative in $artifactPaths) {
        $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $work $relative.Replace('/', '\'))).Hash.ToLowerInvariant()
        "$hash  $relative"
    }
    [IO.File]::WriteAllText((Join-Path $work 'SHA256SUMS'), (($checksumLines -join "`n") + "`n"), $ascii)

    [IO.Directory]::CreateDirectory($outputParent) | Out-Null
    if (Test-Path -LiteralPath $OutputDirectory) {
        $outputItem = Get-Item -Force -LiteralPath $OutputDirectory
        if (($outputItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw 'OutputDirectory may not be a link.' }
        Remove-Item -Recurse -Force -LiteralPath $OutputDirectory
    }
    [IO.Directory]::Move($work, $OutputDirectory)
    $work = $null
    Write-Host "Created signed repository release: $OutputDirectory"
    Write-Host "Sequence: $Sequence; packages: $($packages.Count)"
} finally {
    if ($null -ne $work -and (Test-Path -LiteralPath $work)) { Remove-Item -Recurse -Force -LiteralPath $work }
}