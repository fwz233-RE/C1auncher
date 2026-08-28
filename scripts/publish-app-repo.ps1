[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$ReleaseDirectory,
    [string]$ServerAddressFile,
    [ValidateRange(1, 65535)]
    [int]$Port = 22,
    [string]$PlinkPath = 'plink.exe',
    [string]$PscpPath = 'pscp.exe',
    [string]$OpenSslPath = 'openssl.exe',
    [string]$WslPath = 'wsl.exe',
    [string]$WslDistribution
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repoRoot = '/srv/c1repo'
$ReleaseDirectory = [IO.Path]::GetFullPath($ReleaseDirectory)
if ([string]::IsNullOrWhiteSpace($ServerAddressFile)) {
    $serverFileName = -join ([char[]](0x670D, 0x52A1, 0x5668, 0x5730, 0x5740)) + '.txt'
    $ServerAddressFile = Join-Path 'D:\c1slim' $serverFileName
}
$ServerAddressFile = [IO.Path]::GetFullPath($ServerAddressFile)
$plink = (Get-Command $PlinkPath -ErrorAction Stop).Source
$pscp = (Get-Command $PscpPath -ErrorAction Stop).Source
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

function Invoke-QuietNative {
    param([Parameter(Mandatory)][string]$FilePath, [Parameter(Mandatory)][string[]]$Arguments, [switch]$AllowFailure)
    $oldPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $discarded = & $FilePath @Arguments 2>&1
        $exitCode = $LASTEXITCODE
    } finally { $ErrorActionPreference = $oldPreference }
    if ($exitCode -ne 0 -and -not $AllowFailure) {
        $details = (($discarded | ForEach-Object { $_.ToString() }) -join "`n").Trim()
        if ([string]::IsNullOrEmpty($details)) { $details = 'The command produced no output.' }
        throw "A required external command at script line $($MyInvocation.ScriptLineNumber) failed with exit code $exitCode.`n$details"
    }
    return $exitCode
}

function Protect-SecretFile([string]$Path) {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent().User
    if ($null -eq $identity) { throw 'Unable to identify the current Windows user.' }
    $security = [Security.AccessControl.FileSecurity]::new()
    $security.SetOwner($identity)
    $security.SetAccessRuleProtection($true, $false)
    [void]$security.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new(
        $identity, [Security.AccessControl.FileSystemRights]::FullControl,
        [Security.AccessControl.AccessControlType]::Allow
    ))
    [IO.File]::SetAccessControl($Path, $security)
}

function Read-StrictUtf8([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Required file is missing: $Path" }
    return [IO.File]::ReadAllText($Path, [Text.UTF8Encoding]::new($false, $true))
}

function Get-ServerSettings([string]$Path) {
    $text = Read-StrictUtf8 $Path
    $values = @{}
    $fingerprints = [Collections.Generic.List[string]]::new()
    $hostKeys = @(
        'host', 'server',
        (-join ([char[]](0x670D, 0x52A1, 0x5668, 0x5730, 0x5740))),
        (-join ([char[]](0x516C, 0x7F51, 0x5730, 0x5740))),
        (-join ([char[]](0x4E3B, 0x673A)))
    )
    $userKeys = @(
        'user', 'username',
        (-join ([char[]](0x7528, 0x6237, 0x540D))),
        (-join ([char[]](0x7528, 0x6237)))
    )
    $passwordKeys = @('password', (-join ([char[]](0x5BC6, 0x7801))))
    $hostKeyKeys = @(
        'hostkey', 'host_key', 'fingerprint',
        (-join ([char[]](0x4E3B, 0x673A, 0x5BC6, 0x94A5)))
    )
    foreach ($rawLine in ($text -split "`r?`n")) {
        $line = $rawLine.TrimStart().TrimStart([char]0xFEFF)
        if ([string]::IsNullOrWhiteSpace($line) -or $line.TrimEnd() -eq '---' -or $line.StartsWith('#')) { continue }
        foreach ($match in [regex]::Matches($line, '(?<![A-Za-z0-9+/])SHA256:[A-Za-z0-9+/]{43}=?')) {
            if (-not $fingerprints.Contains($match.Value)) { $fingerprints.Add($match.Value) }
        }
        $normalizedLine = $line.Replace([char]0xFF1A, ':')
        $setting = [regex]::Match($normalizedLine, '^\s*([^:=]+?)\s*[:=](.*)$')
        if (-not $setting.Success) {
            if ($line -notmatch 'plink|pscp|SSH|ssh|SHA256') { throw 'The server address file contains an unrecognized line.' }
            continue
        }
        $key = $setting.Groups[1].Value.Trim()
        $englishKey = $key.ToLowerInvariant()
        $value = $setting.Groups[2].Value.TrimStart()
        if ($hostKeys -ccontains $key -or $hostKeys -ccontains $englishKey) {
            $values.Host = $value
        } elseif ($userKeys -ccontains $key -or $userKeys -ccontains $englishKey) {
            $values.User = $value
        } elseif ($passwordKeys -ccontains $key -or $passwordKeys -ccontains $englishKey) {
            $values.Password = $value
        } elseif ($hostKeyKeys -ccontains $key -or $hostKeyKeys -ccontains $englishKey) {
            if ($value -match '^SHA256:[A-Za-z0-9+/]{43}=?$') { $fingerprints.Add($value) }
        } elseif ($line -notmatch 'plink|pscp|SSH|ssh|SHA256') {
            throw 'The server address file contains an unknown setting.'
        }
    }
    if (-not $values.ContainsKey('Host') -or $values.Host -cnotmatch '^[A-Za-z0-9][A-Za-z0-9.-]{0,252}$') { throw 'The server address file does not contain a valid host.' }
    if (-not $values.ContainsKey('User') -or $values.User -cnotmatch '^[A-Za-z_][A-Za-z0-9_-]{0,31}$') { throw 'The server address file does not contain a valid user.' }
    if (-not $values.ContainsKey('Password') -or [string]::IsNullOrEmpty($values.Password) -or $values.Password.Contains("`r") -or $values.Password.Contains("`n")) { throw 'The server address file does not contain a valid password.' }
    $unique = @($fingerprints | Select-Object -Unique)
    if ($unique.Count -ne 1) { throw 'Exactly one pinned SHA-256 SSH host-key fingerprint is required.' }
    return [pscustomobject]@{ Host = $values.Host; User = $values.User; Password = $values.Password; HostKey = $unique[0] }
}

function Test-SafeToken([string]$Value, [int]$Maximum, [bool]$Version) {
    if ([string]::IsNullOrEmpty($Value) -or $Value.Length -gt $Maximum -or $Value.StartsWith('.') -or $Value.EndsWith('.') -or $Value.Contains('..')) { return $false }
    $pattern = if ($Version) { '^[A-Za-z0-9._+-]+$' } else { '^[A-Za-z0-9._-]+$' }
    return $Value -cmatch $pattern
}

function Test-SafeRelativePath([string]$Value) {
    if ([string]::IsNullOrEmpty($Value) -or $Value.Length -gt 192 -or $Value.StartsWith('/') -or $Value.EndsWith('/')) { return $false }
    if ($Value -cnotmatch '^[\x21-\x7e]+$' -or $Value -match '[\\:*?\[]') { return $false }
    foreach ($component in $Value.Split('/')) { if ($component.Length -eq 0 -or $component -eq '.' -or $component -eq '..') { return $false } }
    return $true
}

function Sort-Ordinal([object[]]$Values) {
    $result = @($Values)
    for ($i = 0; $i -lt $result.Count; $i++) {
        for ($j = $i + 1; $j -lt $result.Count; $j++) {
            if ([string]::CompareOrdinal([string]$result[$i], [string]$result[$j]) -gt 0) {
                $temporary = $result[$i]; $result[$i] = $result[$j]; $result[$j] = $temporary
            }
        }
    }
    return $result
}

function Convert-ToWslPath([string]$Path) {
    $match = [regex]::Match([IO.Path]::GetFullPath($Path), '^([A-Za-z]):\\(.*)$')
    if (-not $match.Success) { throw 'WSL validation requires a local Windows drive path.' }
    return '/mnt/' + $match.Groups[1].Value.ToLowerInvariant() + '/' + $match.Groups[2].Value.Replace('\', '/')
}

if (-not (Test-Path -LiteralPath $ReleaseDirectory -PathType Container)) { throw 'ReleaseDirectory is not a directory.' }
$releaseRootItem = Get-Item -Force -LiteralPath $ReleaseDirectory
if (($releaseRootItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw 'ReleaseDirectory may not be a link.' }
$indexPath = Join-Path $ReleaseDirectory 'index.v1'
$signaturePath = Join-Path $ReleaseDirectory 'index.v1.sig'
$pemPath = Join-Path $ReleaseDirectory 'repository-public-key.pem'
$rawKeyPath = Join-Path $ReleaseDirectory 'repository.ed25519.pub'
$checksumPath = Join-Path $ReleaseDirectory 'SHA256SUMS'
$indexBytes = [IO.File]::ReadAllBytes($indexPath)
if ($indexBytes.Length -eq 0 -or $indexBytes.Length -gt 262144 -or $indexBytes[$indexBytes.Length - 1] -ne 10) { throw 'index.v1 has an invalid size or terminator.' }
foreach ($byte in $indexBytes) { if ($byte -eq 13 -or $byte -eq 0 -or ($byte -gt 126 -and $byte -ne 10)) { throw 'index.v1 must be ASCII with LF line endings.' } }
$indexText = $ascii.GetString($indexBytes)
$indexLines = @($indexText.Substring(0, $indexText.Length - 1) -split "`n")
if ($indexLines.Count -lt 2 -or $indexLines[0] -cne 'C1PKG-INDEX 1') { throw 'index.v1 has an invalid header.' }
$sequenceMatch = [regex]::Match($indexLines[1], '^S\t([1-9][0-9]*)$')
[uint64]$sequence = 0
if (-not $sequenceMatch.Success -or -not [uint64]::TryParse($sequenceMatch.Groups[1].Value, [ref]$sequence) -or $sequence -eq 0) { throw 'index.v1 has an invalid sequence.' }
$sequenceText = $sequenceMatch.Groups[1].Value
$packages = @()
$lastId = $null
for ($lineIndex = 2; $lineIndex -lt $indexLines.Count; $lineIndex++) {
    $line = $indexLines[$lineIndex]
    if ($line.Length -gt 1024) { throw 'An index package record is too long.' }
    $fields = @($line.Split("`t"))
    if ($fields.Count -ne 8 -or $fields[0] -cne 'P') { throw 'An index package record has the wrong field count.' }
    $id = $fields[1]; $version = $fields[2]; $displayName = $fields[3]; $archive = $fields[4]; $hash = $fields[5]; $sizeText = $fields[6]; $entry = $fields[7]
    if (-not (Test-SafeToken $id 32 $false) -or -not (Test-SafeToken $version 48 $true)) { throw 'An index ID or version is invalid.' }
    if ($displayName.Length -lt 1 -or $displayName.Length -gt 40 -or $displayName -cnotmatch '^[\x21-\x7e](?:[\x20-\x7e]{0,38}[\x21-\x7e])?$') { throw 'An index display name is invalid.' }
    if (-not (Test-SafeRelativePath $archive) -or -not (Test-SafeRelativePath $entry)) { throw 'An index relative path is invalid.' }
    if ($archive -cne "packages/$id/$version.tar.gz") { throw 'Archive paths must be packages/<id>/<version>.tar.gz.' }
    [uint64]$size = 0
    if ($hash -cnotmatch '^[0-9a-f]{64}$' -or -not [uint64]::TryParse($sizeText, [ref]$size) -or $size -eq 0 -or $size -gt 33554432) { throw 'An index archive hash or size is invalid.' }
    if ($null -ne $lastId -and [string]::CompareOrdinal($lastId, $id) -ge 0) { throw 'Package IDs must be unique and bytewise sorted.' }
    $lastId = $id
    $packages += [pscustomobject]@{ Id = $id; Version = $version; DisplayName = $displayName; Archive = $archive; Hash = $hash; Size = $size; Entry = $entry }
}
if ($packages.Count -gt 128) { throw 'The client supports at most 128 packages.' }

$expectedFiles = @('index.v1', 'index.v1.sig', 'repository-public-key.pem', 'repository.ed25519.pub', 'SHA256SUMS')
foreach ($package in $packages) { $expectedFiles += $package.Archive }
$expectedFiles = @(Sort-Ordinal $expectedFiles)
$expectedLookup = [Collections.Generic.Dictionary[string,bool]]::new([StringComparer]::Ordinal)
foreach ($relative in $expectedFiles) { if ($expectedLookup.ContainsKey($relative)) { throw 'Indexed artifact paths collide.' }; $expectedLookup[$relative] = $true }
foreach ($item in @(Get-ChildItem -Force -LiteralPath $ReleaseDirectory -Recurse)) {
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw 'Release artifacts may not contain links.' }
    $relative = $item.FullName.Substring($ReleaseDirectory.TrimEnd('\').Length + 1).Replace('\', '/')
    if ($item.PSIsContainer) {
        if (@($expectedFiles | Where-Object { $_.StartsWith($relative + '/', [StringComparison]::Ordinal) }).Count -eq 0) { throw "Unexpected release directory: $relative" }
    } elseif (-not $expectedLookup.ContainsKey($relative)) { throw "Unexpected release file: $relative" }
}
foreach ($relative in $expectedFiles) { if (-not (Test-Path -LiteralPath (Join-Path $ReleaseDirectory $relative.Replace('/', '\')) -PathType Leaf)) { throw "Missing release artifact: $relative" } }

if ((Get-Item -LiteralPath $signaturePath).Length -ne 64) { throw 'index.v1.sig must be a raw 64-byte signature.' }
if ((Get-Item -LiteralPath $rawKeyPath).Length -ne 32) { throw 'repository.ed25519.pub must be exactly 32 bytes.' }
$pemText = Read-StrictUtf8 $pemPath
if ($pemText -match 'PRIVATE KEY' -or $pemText -cnotmatch '\A-----BEGIN PUBLIC KEY-----\n[A-Za-z0-9+/=\n]+-----END PUBLIC KEY-----\n\z') { throw 'The repository public PEM is invalid.' }
[void](Invoke-QuietNative -FilePath $openssl -Arguments @('pkeyutl', '-verify', '-rawin', '-pubin', '-inkey', $pemPath, '-in', $indexPath, '-sigfile', $signaturePath))
$derPath = Join-Path ([IO.Path]::GetTempPath()) ('.c1-public-' + [Guid]::NewGuid().ToString('N') + '.der')
try {
    [void](Invoke-QuietNative -FilePath $openssl -Arguments @('pkey', '-pubin', '-in', $pemPath, '-outform', 'DER', '-out', $derPath))
    $der = [IO.File]::ReadAllBytes($derPath)
    $prefix = [byte[]](0x30,0x2a,0x30,0x05,0x06,0x03,0x2b,0x65,0x70,0x03,0x21,0x00)
    if ($der.Length -ne 44) { throw 'The PEM is not canonical Ed25519 SPKI.' }
    for ($i = 0; $i -lt 12; $i++) { if ($der[$i] -ne $prefix[$i]) { throw 'The PEM key is not Ed25519.' } }
    $raw = [IO.File]::ReadAllBytes($rawKeyPath)
    for ($i = 0; $i -lt 32; $i++) { if ($raw[$i] -ne $der[$i + 12]) { throw 'The raw public key does not match the PEM.' } }
} finally { Remove-Item -Force -ErrorAction SilentlyContinue -LiteralPath $derPath }

$artifactFiles = @($expectedFiles | Where-Object { $_ -cne 'SHA256SUMS' })
$artifactFiles = @(Sort-Ordinal $artifactFiles)
$checksumText = Read-StrictUtf8 $checksumPath
if ($checksumText.Contains("`r") -or -not $checksumText.EndsWith("`n") -or $checksumText.EndsWith("`n`n")) { throw 'SHA256SUMS must use LF and one final LF.' }
$checksumLines = @($checksumText.Substring(0, $checksumText.Length - 1) -split "`n")
if ($checksumLines.Count -ne $artifactFiles.Count) { throw 'SHA256SUMS has an unexpected record count.' }
for ($i = 0; $i -lt $artifactFiles.Count; $i++) {
    $match = [regex]::Match($checksumLines[$i], '^([0-9a-f]{64})  ([\x21-\x7e]+)$')
    if (-not $match.Success -or $match.Groups[2].Value -cne $artifactFiles[$i]) { throw 'SHA256SUMS is not the exact sorted artifact list.' }
    $artifactPath = Join-Path $ReleaseDirectory $artifactFiles[$i].Replace('/', '\')
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath $artifactPath).Hash.ToLowerInvariant() -cne $match.Groups[1].Value) { throw 'A release artifact failed SHA-256 verification.' }
}
foreach ($package in $packages) {
    $archivePath = Join-Path $ReleaseDirectory $package.Archive.Replace('/', '\')
    if ((Get-Item -LiteralPath $archivePath).Length -ne $package.Size -or (Get-FileHash -Algorithm SHA256 -LiteralPath $archivePath).Hash.ToLowerInvariant() -cne $package.Hash) { throw "Indexed archive validation failed for $($package.Id)." }
}

$validatorPath = Join-Path ([IO.Path]::GetTempPath()) ('.c1-validate-package-' + [Guid]::NewGuid().ToString('N') + '.sh')
try {
    $validator = @'
#!/usr/bin/env bash
set -euo pipefail
archive=$1
id=$2
version=$3
entry=$4
temporary=$(mktemp -d)
cleanup() { rm -rf -- "$temporary"; }
trap cleanup EXIT HUP INT TERM
tar --version | head -n 1 | grep -q 'GNU tar'
tar -tzf "$archive" >"$temporary/names"
test -s "$temporary/names"
test "$(LC_ALL=C sort "$temporary/names" | uniq -d | head -n 1)" = ''
while IFS= read -r name; do
    case "$name" in manifest.v1|payload|payload/*) ;; *) exit 1 ;; esac
    printf '%s\n' "$name" | LC_ALL=C grep -Eq '^[!-~]+$'
    case "$name" in /*|*/|*\\*|*:*|*\**|*\?*|*\[*|*' '*) exit 1 ;; esac
    case "/$name/" in */./*|*/../*) exit 1 ;; esac
done <"$temporary/names"
tar -tvzf "$archive" >"$temporary/verbose"
test "$(awk 'substr($0,1,1)!="-" && substr($0,1,1)!="d" {print; exit}' "$temporary/verbose")" = ''
awk '{ if ($3 !~ /^[0-9]+$/ || $3 > 16777216) exit 1; total += $3; if (total > 67108864) exit 1 } END { if (total > 67108864) exit 1 }' "$temporary/verbose"
printf 'C1PKG-PACKAGE 1\nid\t%s\nversion\t%s\nentry\t%s\n' "$id" "$version" "$entry" >"$temporary/expected"
tar -xOzf "$archive" manifest.v1 >"$temporary/manifest"
cmp -s "$temporary/expected" "$temporary/manifest"
line=$(tar -tvzf "$archive" "payload/$entry")
test "$(printf '%s\n' "$line" | wc -l)" -eq 1
test "${line#-}" != "$line"
mode=${line%% *}
case "$mode" in *x*) ;; *) exit 1 ;; esac
'@
    [IO.File]::WriteAllText($validatorPath, $validator.Replace("`r`n", "`n") + "`n", [Text.UTF8Encoding]::new($false))
    $validatorWsl = Convert-ToWslPath $validatorPath
    foreach ($package in $packages) {
        $arguments = @()
        if (-not [string]::IsNullOrWhiteSpace($WslDistribution)) { $arguments += @('--distribution', $WslDistribution) }
        $arguments += @('--', 'bash', $validatorWsl, (Convert-ToWslPath (Join-Path $ReleaseDirectory $package.Archive.Replace('/', '\'))), $package.Id, $package.Version, $package.Entry)
        [void](Invoke-QuietNative -FilePath $wsl -Arguments $arguments)
    }
} finally { Remove-Item -Force -ErrorAction SilentlyContinue -LiteralPath $validatorPath }

$settings = Get-ServerSettings $ServerAddressFile
$passwordFile = Join-Path ([IO.Path]::GetTempPath()) ('.c1-putty-password-' + [Guid]::NewGuid().ToString('N'))
$masterOut = $passwordFile + '.out'; $masterErr = $passwordFile + '.err'; $master = $null
$target = '{0}@{1}' -f $settings.User, $settings.Host
$indexHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $indexPath).Hash.ToLowerInvariant()
$releaseId = "$sequenceText-$($indexHash.Substring(0, 16))"
$stage = "$repoRoot/staging/$releaseId-$([Guid]::NewGuid().ToString('N')).upload"
$hostArguments = @('-batch', '-P', $Port.ToString(), '-hostkey', $settings.HostKey, $target)
function Invoke-PlinkCommand([string]$Command, [switch]$AllowFailure) {
    return Invoke-QuietNative -FilePath $plink -Arguments (@('-share') + $hostArguments + @($Command)) -AllowFailure:$AllowFailure
}

try {
    [IO.File]::WriteAllBytes($passwordFile, [byte[]]@()); Protect-SecretFile $passwordFile
    [IO.File]::WriteAllText($passwordFile, $settings.Password, [Text.UTF8Encoding]::new($false))
    $masterArguments = @('-share', '-N', '-batch', '-P', $Port.ToString(), '-hostkey', $settings.HostKey, '-pwfile', $passwordFile, $target)
    $quoted = ($masterArguments | ForEach-Object { if ($_.Contains('"')) { throw 'Unsafe PuTTY argument.' }; '"' + $_ + '"' }) -join ' '
    $master = Start-Process -FilePath $plink -ArgumentList $quoted -PassThru -WindowStyle Hidden -RedirectStandardOutput $masterOut -RedirectStandardError $masterErr
    $ready = $false
    for ($attempt = 0; $attempt -lt 30; $attempt++) {
        if ($master.HasExited) { break }
        Start-Sleep -Milliseconds 500
        if ((Invoke-QuietNative -FilePath $plink -Arguments @('-batch', '-P', $Port.ToString(), '-hostkey', $settings.HostKey, '-shareexists', $target) -AllowFailure) -eq 0) { $ready = $true; break }
    }
    if (-not $ready) { throw 'The pinned shared PuTTY connection could not be established.' }

    $directories = @()
    foreach ($relative in $expectedFiles) {
        $parent = [IO.Path]::GetDirectoryName($relative.Replace('/', '\'))
        while (-not [string]::IsNullOrEmpty($parent)) {
            $normalized = $parent.Replace('\', '/')
            if ($directories -cnotcontains $normalized) { $directories += $normalized }
            $parent = [IO.Path]::GetDirectoryName($parent)
        }
    }
    $directories = @(Sort-Ordinal $directories)
    $mkdirCommands = foreach ($directory in $directories) { "mkdir -p -- '$stage/$directory'" }
    $prepare = "set -eu; test -d '$repoRoot' -a ! -L '$repoRoot'; test -d '$repoRoot/releases' -a ! -L '$repoRoot/releases'; test -d '$repoRoot/staging' -a ! -L '$repoRoot/staging'; test `"`$(readlink -f '$repoRoot')`" = '$repoRoot'; mkdir -- '$stage'; $($mkdirCommands -join '; ')"
    [void](Invoke-PlinkCommand $prepare)
    foreach ($relative in $expectedFiles) {
        $local = Join-Path $ReleaseDirectory $relative.Replace('/', '\')
        $remote = '{0}:{1}/{2}' -f $target, $stage, $relative
        [void](Invoke-QuietNative -FilePath $pscp -Arguments (@('-share') + $hostArguments[0..4] + @($local, $remote)))
    }

    $expectedList = ($expectedFiles -join "`n") + "`n"
    $remoteScript = @"
set -eu
root='$repoRoot'; stage='$stage'; release_id='$releaseId'; destination="`$root/releases/`$release_id"
test -d "`$root" -a ! -L "`$root"; test "`$(readlink -f "`$root")" = "`$root"
test -d "`$stage" -a ! -L "`$stage"; test "`$(readlink -f "`$stage")" = "`$stage"
actual=`$(find "`$stage" -type f -printf '%P\n' | LC_ALL=C sort)
expected=`$(printf '%s' '$expectedList' | LC_ALL=C sort)
test "`$actual" = "`$expected"
test "`$(find "`$stage" -mindepth 1 ! -type f ! -type d -print -quit)" = ''
cd "`$stage"; sha256sum -c SHA256SUMS >/dev/null
test "`$(wc -c < index.v1.sig)" -eq 64; test "`$(wc -c < repository.ed25519.pub)" -eq 32
openssl pkeyutl -verify -rawin -pubin -inkey repository-public-key.pem -in index.v1 -sigfile index.v1.sig >/dev/null 2>&1
der=`$(mktemp); trap 'rm -f -- "`$der"' EXIT HUP INT TERM
openssl pkey -pubin -in repository-public-key.pem -outform DER -out "`$der" >/dev/null 2>&1
test "`$(wc -c < "`$der")" -eq 44
test "`$(head -c 12 "`$der" | od -An -tx1 | tr -d ' \n')" = '302a300506032b6570032100'
tail -c 32 "`$der" | cmp -s - repository.ed25519.pub
rm -f -- "`$der"; trap - EXIT HUP INT TERM
test "`$(sed -n '1p' index.v1)" = 'C1PKG-INDEX 1'
new_sequence=`$(sed -n '2s/^S\t//p' index.v1); test "`$new_sequence" = '$sequenceText'
lock="`$root/.publish-lock"; link=
cleanup_publish() { test -z "`$link" || rm -f -- "`$link"; rmdir -- "`$lock" 2>/dev/null || true; }
mkdir -- "`$lock"; trap cleanup_publish EXIT HUP INT TERM
if test -L "`$root/current"; then
 current=`$(readlink "`$root/current"); case "`$current" in releases/*) ;; *) exit 1 ;; esac
 current_dir="`$root/`$current"; test -d "`$current_dir" -a ! -L "`$current_dir"
 old_sequence=`$(sed -n '2s/^S\t//p' "`$current_dir/index.v1"); case "`$old_sequence" in ''|0*|*[!0-9]*) exit 1 ;; esac
 if test "`${#new_sequence}" -lt "`${#old_sequence}"; then exit 1; fi
 if test "`${#new_sequence}" -eq "`${#old_sequence}"; then
  first=`$(printf '%s\n%s\n' "`$new_sequence" "`$old_sequence" | LC_ALL=C sort | head -n 1)
  test "`$first" = "`$old_sequence"
  if test "`$new_sequence" = "`$old_sequence"; then test "`$current" = "releases/`$release_id"; fi
 fi
elif test -e "`$root/current"; then exit 1
fi
if test -e "`$destination"; then
 test -d "`$destination" -a ! -L "`$destination"; (cd "`$destination" && sha256sum -c SHA256SUMS >/dev/null); rm -rf -- "`$stage"
else
 mv -- "`$stage" "`$destination"; find "`$destination" -type f -exec chmod 0444 {} +; find "`$destination" -type d -exec chmod 0555 {} +
fi
link="`$root/.current-`$release_id-`$`$"
ln -s "releases/`$release_id" "`$link"; mv -Tf -- "`$link" "`$root/current"; link=
rmdir -- "`$lock"; trap - EXIT HUP INT TERM
"@
    $encoded = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($remoteScript.Replace("`r`n", "`n")))
    [void](Invoke-PlinkCommand "printf '%s' '$encoded' | base64 -d | sh")
    Write-Host "Published release $releaseId atomically."
} catch {
    if ($null -ne $master -and -not $master.HasExited) { [void](Invoke-PlinkCommand "case '$stage' in '$repoRoot/staging/'*.upload) rm -rf -- '$stage' ;; *) exit 1 ;; esac" -AllowFailure) }
    throw
} finally {
    if ($null -ne $master -and -not $master.HasExited) { $master.Kill(); $master.WaitForExit() }
    $settings.Password = $null
    Remove-Item -Force -ErrorAction SilentlyContinue -LiteralPath $passwordFile, $masterOut, $masterErr
}