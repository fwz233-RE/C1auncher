[CmdletBinding()]
param(
    [ValidateSet('Install', 'Verify', 'RemoveOriginal')]
    [string]$Action = 'Install',
    [switch]$Reboot,
    [switch]$EnableAutoSuspend,
    [switch]$DisableAutoSuspend,
    [ValidateRange(30, 600)]
    [int]$ReconnectTimeoutSeconds = 300,
    [string]$CoreEnrollmentBundle,
    [string]$RepositoryProfile,
    [string]$AdbPath = "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe"
)

$ErrorActionPreference = 'Stop'
if ($EnableAutoSuspend -and $DisableAutoSuspend) {
    throw 'EnableAutoSuspend and DisableAutoSuspend are mutually exclusive.'
}
if ($Action -eq 'RemoveOriginal' -and ($EnableAutoSuspend -or $DisableAutoSuspend)) {
    throw 'Suspend preference options apply only to Install or Verify, not RemoveOriginal.'
}
$autoSuspendMode = if ($EnableAutoSuspend) { 'enabled' } elseif ($DisableAutoSuspend) { 'disabled' } else { 'default' }
$projectRoot = Split-Path -Parent $PSScriptRoot
$appPath = Join-Path $projectRoot 'build\C1ancher'
$launcherPath = Join-Path $projectRoot 'build\C1ancher-launcher'
$pkgPath = Join-Path $projectRoot 'build\c1pkg'
$repositoryPublicKeyPath = Join-Path $projectRoot 'config\app-repo\repository.ed25519.pub'
if (-not [string]::IsNullOrWhiteSpace($RepositoryProfile)) {
    $RepositoryProfile = [IO.Path]::GetFullPath($RepositoryProfile)
    $repositoryPublicKeyPath = Join-Path $RepositoryProfile 'repository.ed25519.pub'
    foreach ($profileMember in @('repository.url', 'core-repository.url', 'SHA256SUMS', 'device-repository-config.sh', 'c1-update-check.sh')) {
        if (-not (Test-Path -LiteralPath (Join-Path $RepositoryProfile $profileMember) -PathType Leaf)) { throw "Invalid repository profile: missing $profileMember" }
    }
}
$neofetchRoot = Join-Path $projectRoot 'third_party\neofetch'
$neofetchCommandPath = Join-Path $neofetchRoot 'neofetch'
$neofetchUpstreamPath = Join-Path $neofetchRoot 'neofetch.upstream'
$neofetchConfigPath = Join-Path $neofetchRoot 'c1-config.conf'
$neofetchLogoPath = Join-Path $neofetchRoot 'c1-logo.txt'
$neofetchLicensePath = Join-Path $neofetchRoot 'LICENSE.md'
$deviceScriptPath = Join-Path $PSScriptRoot 'device-default-app.sh'
$runRoot = Join-Path $projectRoot "artifacts\default-app-$([DateTimeOffset]::Now.ToString('yyyyMMdd-HHmmss'))"
$uploadRoot = Join-Path ([System.IO.Path]::GetTempPath()) "c1-default-app-$([guid]::NewGuid().ToString('N'))"
$remoteScript = '/dev/shm/c1-device-default-app.sh'
$remoteShim = '/dev/shm/C1ancher-daemon.shim'
$remoteApp = '/dev/shm/C1ancher.install'
$remoteLauncher = '/dev/shm/C1ancher-launcher.install'
$remotePkg = '/dev/shm/c1pkg.install'
$remoteRepositoryPublicKey = '/dev/shm/c1pkg-repository-key.install'
$remoteNeofetchCommand = '/dev/shm/c1-neofetch.install'
$remoteNeofetchUpstream = '/dev/shm/c1-neofetch-upstream.install'
$remoteNeofetchConfig = '/dev/shm/c1-neofetch-config.install'
$remoteNeofetchLogo = '/dev/shm/c1-neofetch-logo.install'
$remoteNeofetchLicense = '/dev/shm/c1-neofetch-license.install'
$expectedOriginalHash = 'ceb56ddf2ff3c10f7c4c2cd6216b298da1cea799ca7170322f8229d5e9af6ee7'
$expectedPreviousShimHash = 'feff5a9df87fd350cb9fdc56c4d5245c9ca19755e9706a189002c720c9ef9e60'
$openAdbHash = '626e4c5d600b543531337eb67220b0a7520d461211cc7ecafd36666d4f8905cb'
$script:Adb = $null
$script:Serial = $null
$uploaded = $false

function Resolve-Adb {
    if (Test-Path -LiteralPath $AdbPath) { return (Resolve-Path -LiteralPath $AdbPath).Path }
    $command = Get-Command adb -ErrorAction SilentlyContinue
    if ($null -eq $command) { throw 'ADB was not found. Pass -AdbPath explicitly.' }
    return $command.Source
}

function Invoke-Adb {
    param([Parameter(Mandatory)] [string[]]$Arguments, [switch]$AllowFailure, [switch]$WithoutSerial)
    [string[]]$allArguments = if ($WithoutSerial) { @($Arguments) } else { @('-s', $script:Serial) + @($Arguments) }
    $previousPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $lines = & $script:Adb @allArguments 2>&1
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousPreference
    }
    $output = ($lines | ForEach-Object { $_.ToString() }) -join [Environment]::NewLine
    if ($exitCode -ne 0 -and -not $AllowFailure) {
        throw "ADB failed ($exitCode): adb $($allArguments -join ' ')`n$output"
    }
    return [pscustomobject]@{ ExitCode = $exitCode; Output = $output }
}

function Invoke-Remote([string]$Command, [switch]$AllowFailure) {
    return Invoke-Adb -Arguments @('shell', $Command) -AllowFailure:$AllowFailure
}

function Invoke-CheckedRemote([string]$Command) {
    $marker = '__C1_REMOTE_EXIT='
    $wrapped = "$Command; c1_status=`$?; echo ${marker}`$c1_status"
    $result = Invoke-Remote $wrapped -AllowFailure
    $match = [regex]::Match($result.Output, "(?m)^$([regex]::Escape($marker))(\d+)\r?$")
    if (-not $match.Success) { throw "Remote command did not return an exit marker: $Command`n$($result.Output)" }
    $remoteExitCode = [int]$match.Groups[1].Value
    $cleanOutput = [regex]::Replace($result.Output, "(?m)^$([regex]::Escape($marker))\d+\r?$", '').TrimEnd()
    if ($remoteExitCode -ne 0) { throw "Remote command failed ($remoteExitCode): $Command`n$cleanOutput" }
    return $cleanOutput
}

function Get-OnlyDevice {
    $devices = Invoke-Adb -Arguments @('devices') -WithoutSerial
    $deviceLines = @($devices.Output -split "`r?`n" | Where-Object { $_ -match '^(\S+)\s+device$' })
    if ($deviceLines.Count -ne 1) { throw "Exactly one connected ADB device is required; found $($deviceLines.Count)." }
    return [regex]::Match($deviceLines[0], '^(\S+)').Groups[1].Value
}

function Get-ProcessCount([ValidateSet('app_daemon', 'mpenMain', 'C1ancher', 'c1-app')] [string]$Name) {
    $pattern = switch ($Name) {
        'app_daemon' { '\{app_daemon\}|/etc/app_daemon|/usr/data/c1/bin/app_daemon' }
        'mpenMain' { '/usr/bin/d261/mpenMain(?:\s|$)' }
        'C1ancher' { '/usr/data/c1/bin/C1ancher(?:\s|$)' }
        'c1-app' { '/usr/data/c1/bin/c1-app(?:\s|$)' }
    }
    return @((Invoke-Remote 'ps -ef').Output -split "`r?`n" | Where-Object { $_ -match $pattern }).Count
}

function Assert-SystemState {
    $identity = Invoke-Remote 'id'
    if ($identity.Output -notmatch 'uid=0\(root\)') { throw 'A root ADB shell is required.' }
    $mounts = (Invoke-Remote 'mount').Output
    $rootMount = $mounts -split "`r?`n" | Where-Object { $_ -match '\son\s/\stype\s' } | Select-Object -First 1
    if (-not $rootMount -or $rootMount -notmatch '\(ro(?:,|\))') { throw "Root filesystem is not read-only: $rootMount" }
    $storageMount = $mounts -split "`r?`n" | Where-Object { $_ -match '\son\s/storage\stype\s' } | Select-Object -First 1
    if (-not $storageMount -or $storageMount -notmatch '\(rw(?:,|\))') { throw "Storage is not writable: $storageMount" }
    $adbHash = (Invoke-Remote 'sha256sum /etc/init.d/S90usb').Output.Split()[0].ToLowerInvariant()
    if ($adbHash -ne $openAdbHash) { throw "Open root ADB startup hash changed: $adbHash" }
    return [pscustomobject]@{ Identity = $identity.Output; RootMount = $rootMount; StorageMount = $storageMount }
}

function Sync-DeviceClock {
    $hostEpochBefore = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
    Invoke-CheckedRemote "date -u -s '@$hostEpochBefore' >/dev/null; hwclock -w -u" | Out-Null
    $deviceEpochText = Invoke-CheckedRemote 'date +%s'
    [long]$deviceEpoch = 0
    if (-not [long]::TryParse($deviceEpochText.Trim(), [ref]$deviceEpoch)) {
        throw "Device clock returned an invalid epoch: $deviceEpochText"
    }
    $hostEpochAfter = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
    $skewSeconds = [Math]::Min(
        [Math]::Abs($deviceEpoch - $hostEpochBefore),
        [Math]::Abs($deviceEpoch - $hostEpochAfter)
    )
    if ($skewSeconds -gt 5) {
        throw "Device clock synchronization failed; skew is $skewSeconds seconds."
    }
    $rtc = (Invoke-Remote 'hwclock -r -u').Output.Trim()
    return @(
        "host_epoch_before=$hostEpochBefore"
        "device_epoch=$deviceEpoch"
        "host_epoch_after=$hostEpochAfter"
        "skew_seconds=$skewSeconds"
        "rtc=$rtc"
    ) -join [Environment]::NewLine
}

function Get-SuspendCapability {
    # Keep the predicate identical to the two device helpers. Historical probe
    # evidence is not required or fabricated; physical USB resume is a release gate.
    $command = @'
auto_suspend_supported() {
    [ -r /proc/cpuinfo ] &&
    awk -F ':' '$1 ~ /^[ \t]*machine[ \t]*$/ {
        value=$2; sub(/^[ \t]+/, "", value); sub(/[ \t]+$/, "", value)
        count++; if (NF != 2 || value != "ingenic,halley6_v20") bad=1
    } END { exit !(count == 1 && !bad) }' /proc/cpuinfo &&
    [ -d /sys/devices/platform/mpenbatt ] &&
    [ -r /sys/devices/platform/gpio_keys/power/wakeup ] &&
    [ "$(cat /sys/devices/platform/gpio_keys/power/wakeup)" = enabled ] &&
    [ -r /sys/power/state ] && [ -w /sys/power/state ] &&
    grep -Eq '(^|[[:space:]])mem([[:space:]]|$)' /sys/power/state
}
if auto_suspend_supported; then echo automatic_suspend_supported=yes; else echo automatic_suspend_supported=no; fi
'@
    return Invoke-CheckedRemote ($command.Replace("`r`n", "`n"))
}

function Assert-SuspendCapability([string]$Probe) {
    if ($Probe -notmatch '(?m)^automatic_suspend_supported=yes\r?$') {
        throw "Automatic suspend requires supported C1-Slim hardware, enabled gpio_keys wakeup, and writable mem suspend.`n$Probe"
    }
}

function Assert-ApplicationState([ValidateSet('Original', 'C1', 'C1OrLegacy')] [string]$Expected) {
    $deadline = [DateTimeOffset]::UtcNow.AddSeconds(20)
    do {
        $daemonCount = Get-ProcessCount 'app_daemon'
        $mainCount = Get-ProcessCount 'mpenMain'
        $appCount = Get-ProcessCount 'C1ancher'
        $legacyAppCount = Get-ProcessCount 'c1-app'
        $matched = switch ($Expected) {
            'Original' {
                $daemonCount -eq 1 -and $mainCount -eq 1 -and $appCount -eq 0 -and $legacyAppCount -eq 0
            }
            'C1' {
                $daemonCount -eq 1 -and $mainCount -eq 0 -and $appCount -eq 1 -and $legacyAppCount -eq 0
            }
            'C1OrLegacy' {
                $daemonCount -eq 1 -and $mainCount -eq 0 -and ($appCount + $legacyAppCount) -eq 1
            }
        }
        if ($matched) {
            return [pscustomobject]@{
                AppDaemonCount = $daemonCount
                MpenMainCount = $mainCount
                C1ancherCount = $appCount
                LegacyC1AppCount = $legacyAppCount
            }
        }
        Start-Sleep -Seconds 1
    } while ([DateTimeOffset]::UtcNow -lt $deadline)
    throw "Application state did not become ${Expected}: app_daemon=$daemonCount mpenMain=$mainCount C1ancher=$appCount legacy_c1_app=$legacyAppCount"
}

function Wait-ForReconnect([int]$TimeoutSeconds) {
    $deadline = [DateTimeOffset]::UtcNow.AddSeconds($TimeoutSeconds)
    do {
        Start-Sleep -Seconds 2
        $devices = Invoke-Adb -Arguments @('devices') -WithoutSerial -AllowFailure
        $connected = @($devices.Output -split "`r?`n" | Where-Object { $_ -match "^$([regex]::Escape($script:Serial))\s+device$" }).Count -eq 1
        if ($connected) {
            $probe = Invoke-Adb -Arguments @('-s', $script:Serial, 'shell', 'id') -WithoutSerial -AllowFailure
            if ($probe.ExitCode -eq 0 -and $probe.Output -match 'uid=0\(root\)') { return }
        }
    } while ([DateTimeOffset]::UtcNow -lt $deadline)
    throw "Device did not reconnect within $TimeoutSeconds seconds. Restore with /storage/c1/recovery/default-app/device-default-app.sh uninstall."
}

function Write-Evidence([string]$Name, [string]$Content) {
    $Content | Set-Content -Encoding utf8 -Path (Join-Path $runRoot $Name)
}

foreach ($required in @(
    $appPath,
    $launcherPath,
    $pkgPath,
    $repositoryPublicKeyPath,
    $deviceScriptPath,
    $neofetchCommandPath,
    $neofetchUpstreamPath,
    $neofetchConfigPath,
    $neofetchLogoPath,
    $neofetchLicensePath
)) {
    if (-not (Test-Path -LiteralPath $required)) { throw "Required project file is missing: $required" }
}

New-Item -ItemType Directory -Force -Path $runRoot, $uploadRoot | Out-Null
$deviceScriptUpload = Join-Path $uploadRoot 'device-default-app.sh'
$shimPath = Join-Path $uploadRoot 'app_daemon'
[System.IO.File]::WriteAllText(
    $deviceScriptUpload,
    [System.IO.File]::ReadAllText($deviceScriptPath).Replace("`r`n", "`n"),
    [System.Text.UTF8Encoding]::new($false)
)
$shimText = @'
#!/bin/sh
while [ ! -x /usr/data/c1/bin/app_daemon ]; do
    sleep 1
done
exec /usr/data/c1/bin/app_daemon
'@.Replace("`r`n", "`n")
[System.IO.File]::WriteAllText($shimPath, $shimText, [System.Text.UTF8Encoding]::new($false))
$shimHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $shimPath).Hash.ToLowerInvariant()
$appHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $appPath).Hash.ToLowerInvariant()
$launcherHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $launcherPath).Hash.ToLowerInvariant()
$pkgHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $pkgPath).Hash.ToLowerInvariant()
$repositoryPublicKeyHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $repositoryPublicKeyPath).Hash.ToLowerInvariant()
if ((Get-Item -LiteralPath $repositoryPublicKeyPath).Length -ne 32) { throw 'Repository Ed25519 public key must be exactly 32 bytes.' }
$neofetchCommandHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $neofetchCommandPath).Hash.ToLowerInvariant()
$neofetchUpstreamHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $neofetchUpstreamPath).Hash.ToLowerInvariant()
$neofetchConfigHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $neofetchConfigPath).Hash.ToLowerInvariant()
$neofetchLogoHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $neofetchLogoPath).Hash.ToLowerInvariant()
$neofetchLicenseHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $neofetchLicensePath).Hash.ToLowerInvariant()
$script:Adb = Resolve-Adb

try {
    $script:Serial = Get-OnlyDevice
    if ($Action -eq 'Install' -and [string]::IsNullOrWhiteSpace($RepositoryProfile)) {
        Invoke-CheckedRemote 'test -s /usr/data/c1/pkg/repository.url' | Out-Null
        # Fresh devices must receive an explicit common profile; never silently
        # install a package manager that still depends on the retired server.
    }
    $baseline = Assert-SystemState
    if ($Action -eq 'Install') {
        Invoke-CheckedRemote "if test -e /usr/data/c1/pkg/repository.ed25519.pub; then test ! -L /usr/data/c1/pkg/repository.ed25519.pub && test `"`$(sha256sum /usr/data/c1/pkg/repository.ed25519.pub | cut -d ' ' -f 1)`" = '$repositoryPublicKeyHash'; fi" | Out-Null
    }
    $suspendCapability = Get-SuspendCapability
    Write-Evidence 'suspend-capability.txt' ("automatic_suspend_mode=$autoSuspendMode" + [Environment]::NewLine + $suspendCapability + [Environment]::NewLine)
    if ($EnableAutoSuspend) { Assert-SuspendCapability $suspendCapability }
    $clockSync = if ($Action -eq 'Install') { Sync-DeviceClock } else { 'clock_sync=not_requested' }
    Write-Evidence 'clock-sync.txt' ($clockSync + [Environment]::NewLine)
    $currentHash = (Invoke-Remote 'sha256sum /etc/app_daemon').Output.Split()[0].ToLowerInvariant()
    if ($currentHash -notin @($expectedOriginalHash, $expectedPreviousShimHash, $shimHash)) { throw "Unexpected device app_daemon hash: $currentHash" }
    $expectedBefore = if ($currentHash -eq $expectedOriginalHash) { 'Original' } else { 'C1OrLegacy' }
    $applicationBefore = Assert-ApplicationState $expectedBefore
    Write-Evidence 'baseline.txt' (@(
        "action=$Action"
        "identity=$($baseline.Identity)"
        "root_mount=$($baseline.RootMount)"
        "storage_mount=$($baseline.StorageMount)"
        "device_before_sha256=$currentHash"
        "original_sha256=$expectedOriginalHash"
        "shim_sha256=$shimHash"
        "app_sha256=$appHash"
        "launcher_sha256=$launcherHash"
        "c1pkg_sha256=$pkgHash"
        "repository_public_key_sha256=$repositoryPublicKeyHash"
        "neofetch_command_sha256=$neofetchCommandHash"
        "neofetch_upstream_sha256=$neofetchUpstreamHash"
        "neofetch_config_sha256=$neofetchConfigHash"
        "neofetch_logo_sha256=$neofetchLogoHash"
        "neofetch_license_sha256=$neofetchLicenseHash"
        "app_daemon_count=$($applicationBefore.AppDaemonCount)"
        "mpenMain_count=$($applicationBefore.MpenMainCount)"
        "c1ancher_count=$($applicationBefore.C1ancherCount)"
        "legacy_c1_app_count=$($applicationBefore.LegacyC1AppCount)"
    ) -join [Environment]::NewLine)

    Invoke-Adb -Arguments @('push', $deviceScriptUpload, $remoteScript) | Out-Null
    Invoke-Adb -Arguments @('push', $shimPath, $remoteShim) | Out-Null
    Invoke-Adb -Arguments @('push', $appPath, $remoteApp) | Out-Null
    Invoke-Adb -Arguments @('push', $launcherPath, $remoteLauncher) | Out-Null
    Invoke-Adb -Arguments @('push', $pkgPath, $remotePkg) | Out-Null
    Invoke-Adb -Arguments @('push', $repositoryPublicKeyPath, $remoteRepositoryPublicKey) | Out-Null
    Invoke-Adb -Arguments @('push', $neofetchCommandPath, $remoteNeofetchCommand) | Out-Null
    Invoke-Adb -Arguments @('push', $neofetchUpstreamPath, $remoteNeofetchUpstream) | Out-Null
    Invoke-Adb -Arguments @('push', $neofetchConfigPath, $remoteNeofetchConfig) | Out-Null
    Invoke-Adb -Arguments @('push', $neofetchLogoPath, $remoteNeofetchLogo) | Out-Null
    Invoke-Adb -Arguments @('push', $neofetchLicensePath, $remoteNeofetchLicense) | Out-Null
    Invoke-Remote "chmod 700 $remoteScript; chmod 600 $remoteShim $remoteApp $remoteLauncher $remotePkg $remoteRepositoryPublicKey $remoteNeofetchCommand $remoteNeofetchUpstream $remoteNeofetchConfig $remoteNeofetchLogo $remoteNeofetchLicense 2>/dev/null || true" | Out-Null
    $uploaded = $true

    Invoke-CheckedRemote "sh -n $remoteScript" | Out-Null
    Invoke-CheckedRemote "sh -n $remoteShim" | Out-Null
    $actionName = if ($Action -eq 'RemoveOriginal') { 'remove-original' } else { $Action.ToLowerInvariant() }
    $deviceHashArguments = "$expectedOriginalHash $shimHash $appHash $launcherHash $pkgHash $repositoryPublicKeyHash $neofetchCommandHash $neofetchUpstreamHash $neofetchConfigHash $neofetchLicenseHash $neofetchLogoHash $expectedPreviousShimHash $autoSuspendMode"
    $operation = Invoke-CheckedRemote "$remoteScript $actionName $deviceHashArguments"
    Write-Evidence 'operation.log' $operation
    Assert-SystemState | Out-Null
    if ($Action -in @('Install', 'Verify', 'RemoveOriginal')) {
        $verification = Invoke-CheckedRemote "$remoteScript verify $deviceHashArguments"
        Write-Evidence 'verification-before-reboot.log' $verification
        Assert-ApplicationState 'C1' | Out-Null
    }

    if (-not [string]::IsNullOrWhiteSpace($CoreEnrollmentBundle) -and $Action -in @('Install', 'Verify')) {
        $enrollmentAction = if ($Action -eq 'Install') { 'Install' } else { 'Verify' }
        $enrollmentArguments = @{
            Action = $enrollmentAction
            AdbPath = $script:Adb
            TimeoutSeconds = $ReconnectTimeoutSeconds
        }
        if ($enrollmentAction -eq 'Install') { $enrollmentArguments.BundleDirectory = $CoreEnrollmentBundle }
        & (Join-Path $PSScriptRoot 'install-core-enrollment.ps1') @enrollmentArguments
        if (-not $?) { throw 'Core enrollment failed.' }
    }

    if ($Action -eq 'Install' -and -not [string]::IsNullOrWhiteSpace($RepositoryProfile)) {
        & (Join-Path $PSScriptRoot 'install-repository-profile.ps1') -ProfileDirectory $RepositoryProfile -Serial $script:Serial -AdbPath $script:Adb
        if (-not $?) { throw 'Repository provisioning or network verification failed.' }
    }

    if ($Reboot) {
        $uptimeBefore = (Invoke-Remote "cut -d' ' -f1 /proc/uptime").Output.Trim()
        Invoke-Adb -Arguments @('reboot') -AllowFailure | Out-Null
        Wait-ForReconnect $ReconnectTimeoutSeconds
        $uptimeAfter = (Invoke-Remote "cut -d' ' -f1 /proc/uptime").Output.Trim()
        $postBoot = Assert-SystemState
        $expectedAfter = 'C1'
        $applicationAfter = Assert-ApplicationState $expectedAfter
        if ($Action -in @('Install', 'Verify', 'RemoveOriginal')) {
            $persistentVerifier = '/storage/c1/recovery/default-app/device-default-app.sh'
            $postBootVerification = Invoke-CheckedRemote "$persistentVerifier verify $deviceHashArguments"
            Write-Evidence 'verification-after-reboot.log' $postBootVerification
        }
        Write-Evidence 'reboot.log' (@(
            "uptime_before=$uptimeBefore"
            "uptime_after=$uptimeAfter"
            "identity=$($postBoot.Identity)"
            "root_mount=$($postBoot.RootMount)"
            "expected_application=$expectedAfter"
            "app_daemon_count=$($applicationAfter.AppDaemonCount)"
            "mpenMain_count=$($applicationAfter.MpenMainCount)"
            "c1ancher_count=$($applicationAfter.C1ancherCount)"
            "legacy_c1_app_count=$($applicationAfter.LegacyC1AppCount)"
        ) -join [Environment]::NewLine)
    }
} finally {
    if ($uploaded -and $null -ne $script:Serial) {
        Invoke-Remote "rm -f $remoteScript $remoteShim $remoteApp $remoteLauncher $remotePkg $remoteRepositoryPublicKey $remoteNeofetchCommand $remoteNeofetchUpstream $remoteNeofetchConfig $remoteNeofetchLogo $remoteNeofetchLicense" -AllowFailure | Out-Null
    }
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $uploadRoot
}

Write-Host "$Action completed."
Write-Host "Original app_daemon SHA-256: $expectedOriginalHash"
Write-Host "Installed shim SHA-256: $shimHash"
Write-Host "C1ancher SHA-256: $appHash"
Write-Host "C1ancher launcher SHA-256: $launcherHash"
Write-Host "c1pkg SHA-256: $pkgHash"
Write-Host "Repository public key SHA-256: $repositoryPublicKeyHash"
Write-Host "Neofetch launcher SHA-256: $neofetchCommandHash"
Write-Host "Neofetch 7.1.0 SHA-256: $neofetchUpstreamHash"
Write-Host "Evidence: $runRoot"
if (-not $Reboot) { Write-Warning 'Cold-start application selection has not been verified.' }