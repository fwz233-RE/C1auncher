[CmdletBinding()]
param(
    [ValidateSet('Install', 'Verify', 'Uninstall')]
    [string]$Action = 'Install',
    [switch]$Reboot,
    [ValidateRange(30, 600)]
    [int]$ReconnectTimeoutSeconds = 300,
    [string]$AdbPath = "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe"
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$workspaceRoot = Split-Path -Parent $projectRoot
$originalPath = Join-Path $workspaceRoot 'firmware-analysis\system-rootfs\etc\init.d\S90usb'
$deviceScriptPath = Join-Path $PSScriptRoot 'device-open-adb.sh'
$backupManifest = Join-Path $workspaceRoot 'C1Slim-System-Backup\20260814-201924\SHA256SUMS'
$runRoot = Join-Path $projectRoot "artifacts\open-adb-$([DateTimeOffset]::Now.ToString('yyyyMMdd-HHmmss'))"
$uploadRoot = Join-Path ([System.IO.Path]::GetTempPath()) "c1-open-adb-$([guid]::NewGuid().ToString('N'))"
$remoteScript = '/dev/shm/c1-device-open-adb.sh'
$remoteCandidate = '/dev/shm/c1-S90usb.open-root-adb'
$expectedOriginalHash = 'c2b278b283e9bf851461d9e8f6edfd207cec3b120585f0e091777d163562e965'
$script:Adb = $null
$script:Serial = $null
$uploaded = $false

function Resolve-Adb {
    if (Test-Path -LiteralPath $AdbPath) {
        return (Resolve-Path -LiteralPath $AdbPath).Path
    }
    $command = Get-Command adb -ErrorAction SilentlyContinue
    if ($null -eq $command) {
        throw 'ADB was not found. Pass -AdbPath explicitly.'
    }
    return $command.Source
}

function Invoke-Adb {
    param(
        [Parameter(Mandatory)] [string[]]$Arguments,
        [switch]$AllowFailure,
        [switch]$WithoutSerial
    )

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
    if (-not $match.Success) {
        throw "Remote command did not return an exit marker: $Command`n$($result.Output)"
    }
    $remoteExitCode = [int]$match.Groups[1].Value
    $cleanOutput = [regex]::Replace($result.Output, "(?m)^$([regex]::Escape($marker))\d+\r?$", '').TrimEnd()
    if ($remoteExitCode -ne 0) {
        throw "Remote command failed ($remoteExitCode): $Command`n$cleanOutput"
    }
    return [pscustomobject]@{ ExitCode = 0; Output = $cleanOutput }
}

function Get-OnlyDevice {
    $devices = Invoke-Adb -Arguments @('devices') -WithoutSerial
    $deviceLines = @($devices.Output -split "`r?`n" | Where-Object { $_ -match '^(\S+)\s+device$' })
    if ($deviceLines.Count -ne 1) {
        throw "Exactly one connected ADB device is required; found $($deviceLines.Count)."
    }
    return [regex]::Match($deviceLines[0], '^(\S+)').Groups[1].Value
}

function Assert-SafeBaseline {
    $identity = Invoke-Remote 'id'
    if ($identity.Output -notmatch 'uid=0\(root\)') {
        throw 'A root ADB shell is required.'
    }
    $mounts = (Invoke-Remote 'mount').Output
    $rootMount = $mounts -split "`r?`n" | Where-Object { $_ -match '\son\s/\stype\s' } | Select-Object -First 1
    if (-not $rootMount -or $rootMount -notmatch '\(ro(?:,|\))') {
        throw "Root filesystem is not read-only: $rootMount"
    }
    $storageMount = $mounts -split "`r?`n" | Where-Object { $_ -match '\son\s/storage\stype\s' } | Select-Object -First 1
    if (-not $storageMount -or $storageMount -notmatch '\(rw(?:,|\))') {
        throw "Storage is not writable: $storageMount"
    }
    return [pscustomobject]@{ Identity = $identity.Output; RootMount = $rootMount; StorageMount = $storageMount }
}

function Get-ProcessCount([ValidateSet('app_daemon', 'mpenMain')] [string]$Name) {
    $pattern = if ($Name -eq 'app_daemon') { '\{app_daemon\}|/etc/app_daemon' } else { '/usr/bin/d261/mpenMain(?:\s|$)' }
    return @((Invoke-Remote 'ps -ef').Output -split "`r?`n" | Where-Object { $_ -match $pattern }).Count
}

function Assert-OriginalApplication {
    $daemonCount = Get-ProcessCount 'app_daemon'
    $mainCount = Get-ProcessCount 'mpenMain'
    if ($daemonCount -ne 1 -or $mainCount -ne 1) {
        throw "Original application is not in its single-instance baseline: app_daemon=$daemonCount mpenMain=$mainCount"
    }
    return [pscustomobject]@{ AppDaemonCount = $daemonCount; MpenMainCount = $mainCount }
}

function Wait-ForReconnect([int]$TimeoutSeconds) {
    $deadline = [DateTimeOffset]::UtcNow.AddSeconds($TimeoutSeconds)
    do {
        Start-Sleep -Seconds 2
        $devices = Invoke-Adb -Arguments @('devices') -WithoutSerial -AllowFailure
        $deviceLines = @($devices.Output -split "`r?`n")
        $connected = @($deviceLines | Where-Object { $_ -match "^$([regex]::Escape($script:Serial))\s+device$" }).Count -eq 1
        if ($connected) {
            $probe = Invoke-Adb -Arguments @('-s', $script:Serial, 'shell', 'id') -WithoutSerial -AllowFailure
            if ($probe.ExitCode -eq 0 -and $probe.Output -match 'uid=0\(root\)') {
                return
            }
        }
    } while ([DateTimeOffset]::UtcNow -lt $deadline)
    throw "Device did not reconnect as root ADB within $TimeoutSeconds seconds. Use UART or recovery and run /storage/c1/recovery/open-adb/device-open-adb.sh uninstall."
}

function Write-Evidence([string]$Name, [string]$Content) {
    $Content | Set-Content -Encoding utf8 -Path (Join-Path $runRoot $Name)
}

foreach ($required in @($originalPath, $deviceScriptPath, $backupManifest)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Required safety input is missing: $required"
    }
}

$actualOriginalHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $originalPath).Hash.ToLowerInvariant()
if ($actualOriginalHash -ne $expectedOriginalHash) {
    throw "Static S90usb hash mismatch: expected $expectedOriginalHash, got $actualOriginalHash"
}
$manifestText = [System.IO.File]::ReadAllText($backupManifest)
if ($manifestText -notmatch '(?m)^ff58d43b5ba385a6245e5433b54d0727abb9e0312b30e68cadac5c53d87f69e7\s+system\.img\.gz$') {
    throw 'The verified original system image is missing from SHA256SUMS.'
}

New-Item -ItemType Directory -Force -Path $runRoot, $uploadRoot | Out-Null
$candidatePath = Join-Path $uploadRoot 'S90usb.open-root-adb'
$uploadScript = Join-Path $uploadRoot 'device-open-adb.sh'
$originalText = [System.IO.File]::ReadAllText($originalPath)
$disabledLine = "`t#/etc/init.d/usb/adb`t`$1"
$enabledLine = "`t/etc/init.d/usb/adb`t`$1"
$matches = ([regex]::Matches($originalText, [regex]::Escape($disabledLine))).Count
if ($matches -ne 1) {
    throw "Expected exactly one disabled ADB startup line; found $matches."
}
$candidateText = $originalText.Replace($disabledLine, $enabledLine)
[System.IO.File]::WriteAllText($candidatePath, $candidateText, [System.Text.UTF8Encoding]::new($false))
$deviceScriptText = [System.IO.File]::ReadAllText($deviceScriptPath).Replace("`r`n", "`n")
[System.IO.File]::WriteAllText($uploadScript, $deviceScriptText, [System.Text.UTF8Encoding]::new($false))
$installedHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $candidatePath).Hash.ToLowerInvariant()

$script:Adb = Resolve-Adb
try {
    $script:Serial = Get-OnlyDevice
    $baseline = Assert-SafeBaseline
    $baselineApplication = Assert-OriginalApplication
    $remoteHash = (Invoke-Remote 'sha256sum /etc/init.d/S90usb').Output.Split()[0].ToLowerInvariant()
    $allowedHashes = @($expectedOriginalHash, $installedHash)
    if ($remoteHash -notin $allowedHashes) {
        throw "Device S90usb has an unexpected hash: $remoteHash"
    }
    Write-Evidence 'baseline.txt' (@(
        "action=$Action"
        "identity=$($baseline.Identity)"
        "root_mount=$($baseline.RootMount)"
        "storage_mount=$($baseline.StorageMount)"
        "app_daemon_count=$($baselineApplication.AppDaemonCount)"
        "mpenMain_count=$($baselineApplication.MpenMainCount)"
        "original_sha256=$expectedOriginalHash"
        "installed_sha256=$installedHash"
        "device_before_sha256=$remoteHash"
        'authentication=none'
    ) -join [Environment]::NewLine)

    Invoke-Adb -Arguments @('push', $uploadScript, $remoteScript) | Out-Null
    Invoke-Adb -Arguments @('push', $candidatePath, $remoteCandidate) | Out-Null
    Invoke-Remote "chmod 700 $remoteScript; chmod 600 $remoteCandidate" | Out-Null
    $uploaded = $true

    $remoteScriptHash = (Invoke-Remote "sha256sum $remoteScript").Output.Split()[0].ToLowerInvariant()
    $localScriptHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $uploadScript).Hash.ToLowerInvariant()
    if ($remoteScriptHash -ne $localScriptHash) {
        throw 'Uploaded device script hash mismatch.'
    }
    $remoteCandidateHash = (Invoke-Remote "sha256sum $remoteCandidate").Output.Split()[0].ToLowerInvariant()
    if ($remoteCandidateHash -ne $installedHash) {
        throw 'Uploaded S90usb candidate hash mismatch.'
    }
    $syntax = Invoke-CheckedRemote "sh -n $remoteScript"
    Write-Evidence 'syntax-check.txt' "exit_code=$($syntax.ExitCode)"

    $actionName = $Action.ToLowerInvariant()
    $operation = Invoke-CheckedRemote "$remoteScript $actionName $expectedOriginalHash $installedHash"
    Write-Evidence 'operation.log' $operation.Output
    $postOperation = Assert-SafeBaseline

    if ($Action -eq 'Install' -or $Action -eq 'Verify') {
        $verification = Invoke-CheckedRemote "$remoteScript verify $expectedOriginalHash $installedHash"
        Write-Evidence 'verification-before-reboot.log' $verification.Output
    }

    if ($Reboot) {
        $uptimeBefore = (Invoke-Remote "cut -d' ' -f1 /proc/uptime").Output.Trim()
        Invoke-Adb -Arguments @('reboot') -AllowFailure | Out-Null
        Wait-ForReconnect $ReconnectTimeoutSeconds
        $uptimeAfter = (Invoke-Remote "cut -d' ' -f1 /proc/uptime").Output.Trim()
        $postBoot = Assert-SafeBaseline
        $postBootApplication = Assert-OriginalApplication
        if ($Action -eq 'Install' -or $Action -eq 'Verify') {
            $persistentVerifier = '/storage/c1/recovery/open-adb/device-open-adb.sh'
            $postBootVerification = Invoke-CheckedRemote "$persistentVerifier verify $expectedOriginalHash $installedHash"
            Write-Evidence 'verification-after-reboot.log' $postBootVerification.Output
        }
        $processes = Invoke-Remote 'pidof adbd; pidof app_daemon; pidof mpenMain'
        Write-Evidence 'reboot.log' (@(
            "uptime_before=$uptimeBefore"
            "uptime_after=$uptimeAfter"
            "identity=$($postBoot.Identity)"
            "root_mount=$($postBoot.RootMount)"
            "app_daemon_count=$($postBootApplication.AppDaemonCount)"
            "mpenMain_count=$($postBootApplication.MpenMainCount)"
            "processes=$($processes.Output -replace "`r?`n", ',')"
        ) -join [Environment]::NewLine)
    }
} finally {
    if ($uploaded -and $null -ne $script:Serial) {
        Invoke-Remote "rm -f $remoteScript $remoteCandidate" -AllowFailure | Out-Null
    }
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $uploadRoot
}

Write-Host "$Action completed."
Write-Host "Original S90usb SHA-256: $expectedOriginalHash"
Write-Host "Installed S90usb SHA-256: $installedHash"
Write-Host "Evidence: $runRoot"
if ($Action -eq 'Install' -and -not $Reboot) {
    Write-Warning 'Installation is written but cold-start ADB has not been verified. Re-run with -Action Verify -Reboot.'
}