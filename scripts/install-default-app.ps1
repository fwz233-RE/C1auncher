[CmdletBinding()]
param(
    [ValidateSet('Install', 'Verify', 'RemoveOriginal')]
    [string]$Action = 'Install',
    [switch]$Reboot,
    [ValidateRange(30, 600)]
    [int]$ReconnectTimeoutSeconds = 300,
    [string]$AdbPath = "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe"
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$appPath = Join-Path $projectRoot 'build\C1ancher'
$launcherPath = Join-Path $projectRoot 'build\C1ancher-launcher'
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
$remoteHostKey = '/dev/shm/c1-ssh-host-key.install'
$remoteNeofetchCommand = '/dev/shm/c1-neofetch.install'
$remoteNeofetchUpstream = '/dev/shm/c1-neofetch-upstream.install'
$remoteNeofetchConfig = '/dev/shm/c1-neofetch-config.install'
$remoteNeofetchLogo = '/dev/shm/c1-neofetch-logo.install'
$remoteNeofetchLicense = '/dev/shm/c1-neofetch-license.install'
$hostKeyPath = Join-Path $uploadRoot 'ssh_host_ed25519_key'
$expectedOriginalHash = 'ceb56ddf2ff3c10f7c4c2cd6216b298da1cea799ca7170322f8229d5e9af6ee7'
$expectedPreviousShimHash = 'feff5a9df87fd350cb9fdc56c4d5245c9ca19755e9706a189002c720c9ef9e60'
$openAdbHash = '626e4c5d600b543531337eb67220b0a7520d461211cc7ecafd36666d4f8905cb'
$script:Adb = $null
$script:Serial = $null
$uploaded = $false
$hostKeyUploaded = $false
$hostKeyHash = $null

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
$neofetchCommandHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $neofetchCommandPath).Hash.ToLowerInvariant()
$neofetchUpstreamHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $neofetchUpstreamPath).Hash.ToLowerInvariant()
$neofetchConfigHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $neofetchConfigPath).Hash.ToLowerInvariant()
$neofetchLogoHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $neofetchLogoPath).Hash.ToLowerInvariant()
$neofetchLicenseHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $neofetchLicensePath).Hash.ToLowerInvariant()
$script:Adb = Resolve-Adb

try {
    $script:Serial = Get-OnlyDevice
    $baseline = Assert-SystemState
    $hostKeyProbe = Invoke-Remote "if [ -f /usr/data/c1/ssh/ssh_host_ed25519_key ]; then sha256sum /usr/data/c1/ssh/ssh_host_ed25519_key; fi"
    if ($hostKeyProbe.Output -match '^([0-9a-fA-F]{64})\s+') {
        $hostKeyHash = $matches[1].ToLowerInvariant()
    } elseif ($Action -eq 'Install') {
        $sshKeygen = (Get-Command ssh-keygen -ErrorAction Stop).Source
        & $sshKeygen -q -t ed25519 -N '""' -f $hostKeyPath
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $hostKeyPath)) {
            throw 'Failed to generate a unique SSH host key.'
        }
        $hostKeyHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $hostKeyPath).Hash.ToLowerInvariant()
    } else {
        throw 'Persistent SSH host key is missing.'
    }
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
        "ssh_host_key_sha256=$hostKeyHash"
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
    Invoke-Adb -Arguments @('push', $neofetchCommandPath, $remoteNeofetchCommand) | Out-Null
    Invoke-Adb -Arguments @('push', $neofetchUpstreamPath, $remoteNeofetchUpstream) | Out-Null
    Invoke-Adb -Arguments @('push', $neofetchConfigPath, $remoteNeofetchConfig) | Out-Null
    Invoke-Adb -Arguments @('push', $neofetchLogoPath, $remoteNeofetchLogo) | Out-Null
    Invoke-Adb -Arguments @('push', $neofetchLicensePath, $remoteNeofetchLicense) | Out-Null
    if (Test-Path -LiteralPath $hostKeyPath) {
        Invoke-Adb -Arguments @('push', $hostKeyPath, $remoteHostKey) | Out-Null
        $hostKeyUploaded = $true
    }
    Invoke-Remote "chmod 700 $remoteScript; chmod 600 $remoteShim $remoteApp $remoteLauncher $remoteHostKey $remoteNeofetchCommand $remoteNeofetchUpstream $remoteNeofetchConfig $remoteNeofetchLogo $remoteNeofetchLicense 2>/dev/null || true" | Out-Null
    $uploaded = $true

    Invoke-CheckedRemote "sh -n $remoteScript" | Out-Null
    Invoke-CheckedRemote "sh -n $remoteShim" | Out-Null
    $actionName = if ($Action -eq 'RemoveOriginal') { 'remove-original' } else { $Action.ToLowerInvariant() }
    $deviceHashArguments = "$expectedOriginalHash $shimHash $appHash $launcherHash $hostKeyHash $neofetchCommandHash $neofetchUpstreamHash $neofetchConfigHash $neofetchLicenseHash $neofetchLogoHash $expectedPreviousShimHash"
    $operation = Invoke-CheckedRemote "$remoteScript $actionName $deviceHashArguments"
    Write-Evidence 'operation.log' $operation
    Assert-SystemState | Out-Null
    if ($Action -in @('Install', 'Verify', 'RemoveOriginal')) {
        $verification = Invoke-CheckedRemote "$remoteScript verify $deviceHashArguments"
        Write-Evidence 'verification-before-reboot.log' $verification
        Assert-ApplicationState 'C1' | Out-Null
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
        Invoke-Remote "rm -f $remoteScript $remoteShim $remoteApp $remoteLauncher $remoteHostKey $remoteNeofetchCommand $remoteNeofetchUpstream $remoteNeofetchConfig $remoteNeofetchLogo $remoteNeofetchLicense" -AllowFailure | Out-Null
    }
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $uploadRoot
}

Write-Host "$Action completed."
Write-Host "Original app_daemon SHA-256: $expectedOriginalHash"
Write-Host "Installed shim SHA-256: $shimHash"
Write-Host "C1ancher SHA-256: $appHash"
Write-Host "C1ancher launcher SHA-256: $launcherHash"
Write-Host "Neofetch launcher SHA-256: $neofetchCommandHash"
Write-Host "Neofetch 7.1.0 SHA-256: $neofetchUpstreamHash"
Write-Host "Evidence: $runRoot"
if (-not $Reboot) { Write-Warning 'Cold-start application selection has not been verified.' }