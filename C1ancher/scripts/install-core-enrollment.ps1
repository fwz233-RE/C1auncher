[CmdletBinding()]
param(
    [ValidateSet('Install', 'Maintenance', 'Verify', 'Uninstall')]
    [string]$Action = 'Install',
    [string]$BundleDirectory,
    [switch]$Reboot,
    [ValidateRange(30, 900)]
    [int]$TimeoutSeconds = 300,
    [string]$AdbPath = "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe",
    [string]$OpenSslPath = 'openssl.exe',
    [string]$WslPath = 'wsl.exe',
    [string]$WslDistribution
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$script:Adb = $null
$script:Serial = $null

function Resolve-Executable([string]$Requested, [string[]]$Candidates) {
    $command = Get-Command $Requested -ErrorAction SilentlyContinue
    if ($null -ne $command) { return $command.Source }
    $candidate = $Candidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
    if ([string]::IsNullOrWhiteSpace($candidate)) { throw "Required executable was not found: $Requested" }
    return $candidate
}

function Invoke-Native {
    param([Parameter(Mandatory)][string]$FilePath, [Parameter(Mandatory)][string[]]$Arguments, [switch]$AllowFailure)
    $oldPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $lines = & $FilePath @Arguments 2>&1
        $exitCode = $LASTEXITCODE
    } finally { $ErrorActionPreference = $oldPreference }
    $output = ($lines | ForEach-Object { $_.ToString() }) -join [Environment]::NewLine
    if ($exitCode -ne 0 -and -not $AllowFailure) { throw "External command failed ($exitCode).`n$output" }
    return [pscustomobject]@{ ExitCode = $exitCode; Output = $output }
}

function Invoke-Adb([string[]]$Arguments, [switch]$AllowFailure, [switch]$WithoutSerial) {
    $all = if ($WithoutSerial) { $Arguments } else { @('-s', $script:Serial) + $Arguments }
    return Invoke-Native $script:Adb $all -AllowFailure:$AllowFailure
}

function Invoke-Remote([string]$Command, [switch]$AllowFailure) {
    return Invoke-Adb @('shell', $Command) -AllowFailure:$AllowFailure
}

function Invoke-ProbedRemote([string]$Command) {
    $marker = '__C1_ENROLL_EXIT='
    $result = Invoke-Remote "$Command; status=`$?; echo ${marker}`$status" -AllowFailure
    $match = [regex]::Match($result.Output, "(?m)^$([regex]::Escape($marker))(\d+)\r?$")
    $clean = [regex]::Replace($result.Output, "(?m)^$([regex]::Escape($marker))\d+\r?$", '').TrimEnd()
    return [pscustomobject]@{
        MarkerFound = $match.Success
        RemoteExitCode = if ($match.Success) { [int]$match.Groups[1].Value } else { $null }
        Output = $clean
    }
}

function Invoke-CheckedRemote([string]$Command) {
    $result = Invoke-ProbedRemote $Command
    if (-not $result.MarkerFound) { throw "Remote command returned no status marker.`n$($result.Output)" }
    if ($result.RemoteExitCode -ne 0) { throw "Remote command failed ($($result.RemoteExitCode)).`n$($result.Output)" }
    return $result.Output
}

function Get-OnlyDevice {
    $devices = Invoke-Adb @('devices') -WithoutSerial
    $matches = @($devices.Output -split "`r?`n" | ForEach-Object { $_.Trim() } | Where-Object { $_ -match '^(\S+)\s+device$' })
    if ($matches.Count -ne 1) { throw "Exactly one connected ADB device is required; found $($matches.Count)." }
    return [regex]::Match($matches[0], '^(\S+)').Groups[1].Value
}

function Convert-ToWslPath([string]$Path) {
    $match = [regex]::Match([IO.Path]::GetFullPath($Path), '^([A-Za-z]):\\(.*)$')
    if (-not $match.Success) { throw 'WSL validation requires a local Windows drive path.' }
    return '/mnt/' + $match.Groups[1].Value.ToLowerInvariant() + '/' + $match.Groups[2].Value.Replace('\', '/')
}

function Assert-EnrollmentPublicKeys([string]$OpenSsl, [string]$Pem, [string]$Raw) {
    # Compare public keys only; no production private key is read here.
    $publicDer = [IO.Path]::GetTempFileName()
    try {
        [void](Invoke-Native $OpenSsl @('pkey', '-pubin', '-in', $Pem, '-outform', 'DER', '-out', $publicDer))
        $derBytes = [IO.File]::ReadAllBytes($publicDer)
        $rawBytes = [IO.File]::ReadAllBytes($Raw)
        $prefix = [byte[]]@(0x30,0x2a,0x30,0x05,0x06,0x03,0x2b,0x65,0x70,0x03,0x21,0x00)
        if ($derBytes.Length -ne 44 -or $rawBytes.Length -ne 32) { throw 'Enrollment keys must be Ed25519 public keys.' }
        for ($i = 0; $i -lt 12; $i++) {
            if ($derBytes[$i] -ne $prefix[$i]) { throw 'Enrollment PEM public key encoding is invalid.' }
        }
        for ($i = 0; $i -lt 32; $i++) {
            if ($derBytes[$i + 12] -ne $rawBytes[$i]) { throw 'Enrollment PEM and raw public keys differ.' }
        }
    } finally { Remove-Item -LiteralPath $publicDer -Force -ErrorAction SilentlyContinue }
}

function Assert-MaintenanceKeyHash([string]$Raw, [string]$ExistingHash) {
    $bundleKeyHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $Raw).Hash.ToLowerInvariant()
    if ($ExistingHash -cnotmatch '^[0-9a-f]{64}$' -or $ExistingHash -cne $bundleKeyHash) {
        throw 'Maintenance bundle does not match the existing device public key; no bundle code was executed.'
    }
}

function Assert-LocalBundle([string]$Path) {
    $required = @(
        'bootstrap.v1', 'bootstrap.v1.sig', 'core.ed25519.pem', 'core.ed25519.pub',
        'app-daemon-bootstrap.sh', 'device-core-enroll.sh', 'enroll.sh',
        'release/manifest.v1', 'release/manifest.v1.sig', 'release/artifacts/C1ancher',
        'release/artifacts/c1pkg', 'release/artifacts/C1ancher-launcher', 'release/artifacts/c1updater'
    )
    $items = @(Get-ChildItem -Force -LiteralPath $Path -Recurse)
    foreach ($item in $items) {
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw 'Enrollment bundle may not contain links.' }
    }
    $files = @($items | Where-Object { -not $_.PSIsContainer })
    if ($files.Count -ne $required.Count) { throw 'Enrollment bundle has an unexpected file count.' }
    foreach ($relative in $required) {
        if (-not (Test-Path -LiteralPath (Join-Path $Path $relative.Replace('/', '\')) -PathType Leaf)) {
            throw "Enrollment bundle member is missing: $relative"
        }
    }
    $openssl = Resolve-Executable $OpenSslPath @(
        (Join-Path $env:ProgramFiles 'Git\usr\bin\openssl.exe'),
        (Join-Path $env:ProgramFiles 'Git\mingw64\bin\openssl.exe')
    )
    $manifest = Join-Path $Path 'bootstrap.v1'
    $signature = Join-Path $Path 'bootstrap.v1.sig'
    $pem = Join-Path $Path 'core.ed25519.pem'
    if ((Get-Item -LiteralPath $signature).Length -ne 64 -or (Get-Item -LiteralPath (Join-Path $Path 'core.ed25519.pub')).Length -ne 32) {
        throw 'Enrollment signature or public key has an invalid size.'
    }
    # Bind the signature PEM to the raw trust anchor before verification.
    Assert-EnrollmentPublicKeys $openssl $pem (Join-Path $Path 'core.ed25519.pub')
    [void](Invoke-Native $openssl @('pkeyutl', '-verify', '-rawin', '-pubin', '-inkey', $pem, '-in', $manifest, '-sigfile', $signature))
    $validator = Join-Path $PSScriptRoot 'validate-core-release.sh'
    $wsl = (Get-Command $WslPath -ErrorAction Stop).Source
    $arguments = @()
    if (-not [string]::IsNullOrWhiteSpace($WslDistribution)) { $arguments += @('--distribution', $WslDistribution) }
    $arguments += @('--', 'bash', (Convert-ToWslPath $validator), (Convert-ToWslPath (Join-Path $Path 'release')), (Convert-ToWslPath $pem))
    [void](Invoke-Native $wsl $arguments)
    # Authenticate scripts and the fixed recovery verifier before any bundle
    # code is uploaded/executed. Signature-only verification was insufficient.
    $records = [IO.File]::ReadAllLines($manifest)
    if ($records.Count -lt 11 -or $records.Count -gt 26 -or
        $records[0] -cne 'C1CORE-BOOTSTRAP 1' -or $records[1] -cne "V`t1.1.0") {
        throw 'Unsupported trusted enrollment format/version.'
    }
    $members = @(
        @('K', 'core.ed25519.pub'), @('P', 'core.ed25519.pem'),
        @('B', 'app-daemon-bootstrap.sh'), @('D', 'device-core-enroll.sh'),
        @('L', 'enroll.sh'), @('M', 'release/manifest.v1'),
        @('G', 'release/manifest.v1.sig'), @('U', 'release/artifacts/c1updater')
    )
    for ($i = 0; $i -lt $members.Count; $i++) {
        $fields = $records[$i + 2].Split("`t")
        if ($fields.Count -ne 2 -or $fields[0] -cne $members[$i][0] -or $fields[1] -cnotmatch '^[0-9a-f]{64}$') {
            throw 'Invalid signed enrollment member record.'
        }
        $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $Path $members[$i][1])).Hash.ToLowerInvariant()
        if ($hash -cne $fields[1]) { throw "Signed enrollment member hash mismatch: $($members[$i][1])" }
    }
    return $required
}

function Wait-ForCondition([scriptblock]$Condition, [string]$Failure) {
    $deadline = [DateTimeOffset]::UtcNow.AddSeconds($TimeoutSeconds)
    do {
        if (& $Condition) { return }
        Start-Sleep -Seconds 2
    } while ([DateTimeOffset]::UtcNow -lt $deadline)
    throw $Failure
}

function Wait-ForReboot([string]$BootIdBefore) {
    $deadline = [DateTimeOffset]::UtcNow.AddSeconds($TimeoutSeconds)
    do {
        $probe = Invoke-ProbedRemote 'cat /proc/sys/kernel/random/boot_id'
        $bootIdAfter = $probe.Output.Trim()
        if ($probe.MarkerFound -and $probe.RemoteExitCode -eq 0 -and
            -not [string]::IsNullOrWhiteSpace($bootIdAfter) -and $bootIdAfter -cne $BootIdBefore) {
            return
        }
        Start-Sleep -Seconds 2
    } while ([DateTimeOffset]::UtcNow -lt $deadline)
    throw 'Device did not complete a reboot before timeout.'
}

$script:Adb = Resolve-Executable $AdbPath @()
$script:Serial = Get-OnlyDevice
$identity = Invoke-CheckedRemote 'id'
if ($identity -notmatch 'uid=0\(root\)') { throw 'A root ADB shell is required.' }
$mounts = Invoke-CheckedRemote 'mount'
$rootMount = $mounts -split "`r?`n" | Where-Object { $_ -match '\son\s/\stype\s' } | Select-Object -First 1
if (-not $rootMount -or $rootMount -notmatch '\(ro(?:,|\))') { throw 'Root filesystem must begin read-only.' }
$disableBefore = (Invoke-CheckedRemote 'if [ -e /usr/data/c1/disable-auto-suspend ]; then sha256sum /usr/data/c1/disable-auto-suspend; else echo absent; fi').Trim()

if ($Action -eq 'Install' -or $Action -eq 'Maintenance') {
    $remoteAction = if ($Action -eq 'Maintenance') { 'maintenance-install' } else { 'install' }
    if ([string]::IsNullOrWhiteSpace($BundleDirectory)) { throw 'BundleDirectory is required for Install.' }
    $BundleDirectory = [IO.Path]::GetFullPath($BundleDirectory)
    if (-not (Test-Path -LiteralPath $BundleDirectory -PathType Container)) { throw 'BundleDirectory is missing.' }
    $required = Assert-LocalBundle $BundleDirectory
    if ($Action -eq 'Maintenance') {
        # Compare against the existing device trust anchor before uploading or
        # executing any code supplied by the maintenance bundle.
        $existingKeyHash = (Invoke-CheckedRemote 'sha256sum /etc/c1updater/core.ed25519.pub').Trim().Split()[0].ToLowerInvariant()
        Assert-MaintenanceKeyHash (Join-Path $BundleDirectory 'core.ed25519.pub') $existingKeyHash
    }
    $remoteRoot = '/storage/c1/update/incoming-' + [Guid]::NewGuid().ToString('N')
    try {
        [void](Invoke-CheckedRemote "mkdir -p '$remoteRoot/release/artifacts'; chmod 700 '$remoteRoot' '$remoteRoot/release' '$remoteRoot/release/artifacts'")
        foreach ($relative in $required) {
            $local = Join-Path $BundleDirectory $relative.Replace('/', '\')
            $remote = "$remoteRoot/$relative"
            [void](Invoke-Adb @('push', $local, $remote))
            $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $local).Hash.ToLowerInvariant()
            $actual = (Invoke-CheckedRemote "sha256sum '$remote'").Split()[0].ToLowerInvariant()
            if ($actual -ne $hash) { throw "Device upload digest mismatch: $relative" }
        }
        [void](Invoke-CheckedRemote "chmod 700 '$remoteRoot/device-core-enroll.sh' '$remoteRoot/enroll.sh'; chmod 600 '$remoteRoot/bootstrap.v1' '$remoteRoot/bootstrap.v1.sig' '$remoteRoot/core.ed25519.pem' '$remoteRoot/core.ed25519.pub' '$remoteRoot/app-daemon-bootstrap.sh' '$remoteRoot/release/manifest.v1' '$remoteRoot/release/manifest.v1.sig' '$remoteRoot/release/artifacts/C1ancher' '$remoteRoot/release/artifacts/c1pkg' '$remoteRoot/release/artifacts/C1ancher-launcher' '$remoteRoot/release/artifacts/c1updater'; '$remoteRoot/device-core-enroll.sh' '$remoteAction' '$remoteRoot'")
        Wait-ForCondition {
            $probe = Invoke-ProbedRemote 'test -f /usr/data/c1/update/enrolled.v1 && /storage/c1/update/enrollment/bundle/device-core-enroll.sh verify'
            return $probe.MarkerFound -and $probe.RemoteExitCode -eq 0 -and $probe.Output -match 'core enrollment verified'
        } 'Core enrollment did not become healthy before timeout.'
    } finally {
        [void](Invoke-ProbedRemote "rm -rf '$remoteRoot'")
    }
} elseif ($Action -eq 'Verify') {
    [void](Invoke-CheckedRemote '/storage/c1/update/enrollment/bundle/device-core-enroll.sh verify')
} else {
    [void](Invoke-CheckedRemote '/storage/c1/update/enrollment/bundle/device-core-enroll.sh uninstall')
}

$disableAfter = (Invoke-CheckedRemote 'if [ -e /usr/data/c1/disable-auto-suspend ]; then sha256sum /usr/data/c1/disable-auto-suspend; else echo absent; fi').Trim()
if ($disableAfter -cne $disableBefore) { throw '/usr/data/c1/disable-auto-suspend changed during enrollment.' }
$rootAfter = (Invoke-CheckedRemote 'mount') -split "`r?`n" | Where-Object { $_ -match '\son\s/\stype\s' } | Select-Object -First 1
if ($rootAfter -notmatch '\(ro(?:,|\))') { throw 'Root filesystem was not restored read-only.' }

if ($Reboot) {
    $bootIdBefore = (Invoke-CheckedRemote 'cat /proc/sys/kernel/random/boot_id').Trim()
    if ([string]::IsNullOrWhiteSpace($bootIdBefore)) { throw 'Device returned an empty boot ID before reboot.' }
    [void](Invoke-Adb @('reboot') -AllowFailure)
    Wait-ForReboot $bootIdBefore
    if ($Action -ne 'Uninstall') {
        [void](Invoke-CheckedRemote '/storage/c1/update/enrollment/bundle/device-core-enroll.sh verify')
    }
}
Write-Host "Core enrollment action completed: $Action"
if (-not $Reboot) { Write-Warning 'Cold-start bootstrap selection has not been verified.' }