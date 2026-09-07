[CmdletBinding()]
param(
    [ValidateRange(0, 86400)]
    [int]$DurationSeconds = 60,
    [ValidateRange(1, 3600)]
    [int]$IntervalSeconds = 1,
    [switch]$SuspendProbe,
    [ValidateRange(0, 100)]
    [int]$SuspendCycles = 0,
    [switch]$ConfirmSuspend,
    [ValidateRange(10, 600)]
    [int]$ReconnectTimeoutSeconds = 120,
    [string]$AdbPath = "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe"
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$deviceScriptSource = Join-Path $PSScriptRoot 'device-power-sample.sh'
$runRoot = Join-Path $projectRoot "artifacts\power-$([DateTimeOffset]::Now.ToString('yyyyMMdd-HHmmss'))"
$uploadRoot = Join-Path ([System.IO.Path]::GetTempPath()) "C1ancher-power-$([guid]::NewGuid().ToString('N'))"
$remoteScript = '/dev/shm/c1-device-power-sample.sh'
$remoteSuspendResult = '/dev/shm/c1-suspend-result'
$remoteSuspendProbeProof = '/usr/data/c1/suspend-probe-passed'
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
    return $cleanOutput
}

function Get-OnlyDevice {
    $devices = Invoke-Adb -Arguments @('devices') -WithoutSerial
    $deviceLines = @($devices.Output -split "`r?`n" | Where-Object { $_ -match '^(\S+)\s+device$' })
    if ($deviceLines.Count -ne 1) {
        throw "Exactly one connected ADB device is required; found $($deviceLines.Count)."
    }
    return [regex]::Match($deviceLines[0], '^(\S+)').Groups[1].Value
}

function Write-Evidence([string]$Name, [string]$Content) {
    [System.IO.File]::WriteAllText(
        (Join-Path $runRoot $Name),
        $Content,
        [System.Text.UTF8Encoding]::new($false)
    )
}

function Wait-ForSuspendCompletion([int]$TimeoutSeconds) {
    $started = [DateTimeOffset]::UtcNow
    $deadline = $started.AddSeconds($TimeoutSeconds)
    do {
        $devices = Invoke-Adb -Arguments @('devices') -WithoutSerial -AllowFailure
        $connected = @($devices.Output -split "`r?`n" | Where-Object { $_ -match "^$([regex]::Escape($script:Serial))\s+device$" }).Count -eq 1
        if ($connected) {
            $identity = Invoke-Adb -Arguments @('-s', $script:Serial, 'shell', 'id') -WithoutSerial -AllowFailure
            if ($identity.ExitCode -eq 0) {
                $result = Invoke-Adb -Arguments @('-s', $script:Serial, 'shell', "cat $remoteSuspendResult 2>/dev/null || echo unavailable") -WithoutSerial -AllowFailure
                $deviceResult = $result.Output.Trim()
                if ($result.ExitCode -eq 0 -and $deviceResult -in @('ok', 'failed')) {
                    return [pscustomobject]@{
                        ElapsedSeconds = [math]::Round(([DateTimeOffset]::UtcNow - $started).TotalSeconds, 3)
                        Identity = $identity.Output.Trim()
                        DeviceResult = $deviceResult
                    }
                }
            }
        }
        Start-Sleep -Milliseconds 500
    } while ([DateTimeOffset]::UtcNow -lt $deadline)
    throw "Device $($script:Serial) did not complete suspend and recover over ADB within $TimeoutSeconds seconds."
}

function Get-RemoteValue([string]$Path) {
    $result = Invoke-Remote "if [ -r '$Path' ]; then head -n 1 '$Path'; else echo unavailable; fi" -AllowFailure
    if ($result.ExitCode -ne 0 -or [string]::IsNullOrWhiteSpace($result.Output)) { return 'unavailable' }
    return ($result.Output -replace "`r?`n", ' ').Trim()
}

function Invoke-SuspendCycle([int]$Cycle) {
    Write-Warning "Suspend cycle $Cycle will stop ADB and enter mem suspend. Wake the device with the physical power key."
    $startedUtc = [DateTimeOffset]::UtcNow.ToString('o')
    $beforeUptime = Get-RemoteValue '/proc/uptime'
    $beforeWakeup = Get-RemoteValue '/sys/power/wakeup_count'
    $command = "rm -f $remoteSuspendResult; /bin/busybox start-stop-daemon -S -b -x /bin/sh -- -c 'sleep 1; probe_result=failed; adb_was_enabled=0; adb_stop_ok=1; if [ -e /sys/kernel/config/usb_gadget/demo/configs/c.1/ffs.adb ]; then adb_was_enabled=1; CONFIGFS_HOME=/sys/kernel/config /etc/init.d/usb/adb stop || adb_stop_ok=0; fi; if [ `$adb_stop_ok -eq 1 ] && echo mem > /sys/power/state; then probe_result=ok; fi; if [ `$adb_was_enabled -eq 1 ]; then CONFIGFS_HOME=/sys/kernel/config /etc/init.d/usb/adb start || probe_result=failed; fi; echo `$probe_result > $remoteSuspendResult'"
    $suspendCommand = Invoke-Remote $command -AllowFailure
    if ($suspendCommand.ExitCode -ne 0) {
        throw "Failed to schedule suspend cycle ${Cycle}: $($suspendCommand.Output)"
    }

    # Suspending USB can disconnect the transport. The detached device-side
    # command records completion, while this loop waits for a fresh ADB shell.
    $recovery = Wait-ForSuspendCompletion $ReconnectTimeoutSeconds
    $deviceResult = $recovery.DeviceResult
    $afterUptime = Get-RemoteValue '/proc/uptime'
    $afterWakeup = Get-RemoteValue '/sys/power/wakeup_count'
    $cycleLog = @(
        "cycle=$Cycle"
        "started_utc=$startedUtc"
        "suspend_adb_exit_code=$($suspendCommand.ExitCode)"
        "suspend_adb_output=$($suspendCommand.Output -replace "`r?`n", ' ')"
        "device_result=$deviceResult"
        "adb_recovery_seconds=$($recovery.ElapsedSeconds)"
        "recovery_identity=$($recovery.Identity)"
        "uptime_before=$beforeUptime"
        "uptime_after=$afterUptime"
        "wakeup_count_before=$beforeWakeup"
        "wakeup_count_after=$afterWakeup"
    ) -join [Environment]::NewLine
    Write-Evidence ("suspend-cycle-{0:D2}.txt" -f $Cycle) $cycleLog
    if ($deviceResult -ne 'ok') {
        throw "Suspend cycle $Cycle failed on the device (result: $deviceResult)."
    }

    $postSample = Invoke-CheckedRemote "$remoteScript 0 1"
    Write-Evidence ("post-suspend-cycle-{0:D2}.tsv" -f $Cycle) ($postSample + [Environment]::NewLine)
}

if ($SuspendProbe -and $SuspendCycles -gt 0) {
    throw 'Use either -SuspendProbe for one cycle or -SuspendCycles for repeated cycles, not both.'
}
$suspendRequested = $SuspendProbe -or $SuspendCycles -gt 0
if ($suspendRequested -and -not $ConfirmSuspend) {
    throw 'Suspend was requested but not confirmed. Add -ConfirmSuspend to allow writes of mem to /sys/power/state.'
}
if (-not (Test-Path -LiteralPath $deviceScriptSource)) {
    throw "Required input is missing: $deviceScriptSource"
}

New-Item -ItemType Directory -Force -Path $runRoot, $uploadRoot | Out-Null
$deviceScriptUpload = Join-Path $uploadRoot 'device-power-sample.sh'
[System.IO.File]::WriteAllText(
    $deviceScriptUpload,
    [System.IO.File]::ReadAllText($deviceScriptSource).Replace("`r`n", "`n"),
    [System.Text.UTF8Encoding]::new($false)
)
$script:Adb = Resolve-Adb

try {
    $script:Serial = Get-OnlyDevice
    $identity = (Invoke-Remote 'id').Output.Trim()
    $isRoot = $identity -match 'uid=0\(root\)'
    if ($suspendRequested) {
        if (-not $isRoot) { throw 'Suspend requires a root ADB shell.' }
        Invoke-CheckedRemote "rm -f $remoteSuspendProbeProof" | Out-Null
    }
    $powerProbeCommand = @'
state_exists=0; state_readable=0; state_writable=0; wakeup_exists=0; wakeup_readable=0; [ -e /sys/power/state ] && state_exists=1; [ -r /sys/power/state ] && state_readable=1; [ -w /sys/power/state ] && state_writable=1; [ -e /sys/power/wakeup_count ] && wakeup_exists=1; [ -r /sys/power/wakeup_count ] && wakeup_readable=1; echo state_exists=$state_exists; echo state_readable=$state_readable; echo state_writable=$state_writable; echo wakeup_count_exists=$wakeup_exists; echo wakeup_count_readable=$wakeup_readable; printf 'power_state='; if [ -r /sys/power/state ]; then head -n 1 /sys/power/state; else echo unavailable; fi; printf 'mem_sleep='; if [ -r /sys/power/mem_sleep ]; then head -n 1 /sys/power/mem_sleep; else echo unavailable; fi; printf 'power_supply_count='; c=0; for d in /sys/class/power_supply/*; do [ -d "$d" ] && c=$((c + 1)); done; echo $c; input_wakeup_count=0; enabled_input_wakeup_count=0; for node in /sys/class/input/event*/device/power/wakeup; do [ -r "$node" ] || continue; input_wakeup_count=$((input_wakeup_count + 1)); [ "$(cat "$node" 2>/dev/null)" = enabled ] && enabled_input_wakeup_count=$((enabled_input_wakeup_count + 1)); echo input_wakeup_node=$node:$(cat "$node" 2>/dev/null); done; echo input_wakeup_count=$input_wakeup_count; echo enabled_input_wakeup_count=$enabled_input_wakeup_count
'@
    $powerProbe = Invoke-CheckedRemote $powerProbeCommand
    Write-Evidence 'baseline.txt' (@(
        "serial=$($script:Serial)"
        "adb=$($script:Adb)"
        "identity=$identity"
        "root=$($isRoot.ToString().ToLowerInvariant())"
        "duration_seconds=$DurationSeconds"
        "interval_seconds=$IntervalSeconds"
        "suspend_requested=$($suspendRequested.ToString().ToLowerInvariant())"
        $powerProbe
    ) -join [Environment]::NewLine)

    Invoke-Adb -Arguments @('push', $deviceScriptUpload, $remoteScript) | Out-Null
    Invoke-Remote "chmod 700 $remoteScript" | Out-Null
    $uploaded = $true
    Invoke-CheckedRemote "sh -n $remoteScript" | Out-Null
    Write-Evidence 'syntax-check.txt' "remote_sh_n=passed$([Environment]::NewLine)"

    $samples = Invoke-CheckedRemote "$remoteScript $DurationSeconds $IntervalSeconds"
    Write-Evidence 'samples.tsv' ($samples + [Environment]::NewLine)

    if ($suspendRequested) {
        $powerState = Get-RemoteValue '/sys/power/state'
        if (@($powerState -split '\s+') -notcontains 'mem') {
            throw "The device does not advertise mem suspend in /sys/power/state: $powerState"
        }
        $stateWritable = Invoke-CheckedRemote "if [ -w /sys/power/state ]; then echo yes; else echo no; fi"
        if ($stateWritable.Trim() -ne 'yes') { throw '/sys/power/state is not writable.' }

        $cycleCount = if ($SuspendProbe) { 1 } else { $SuspendCycles }
        for ($cycle = 1; $cycle -le $cycleCount; $cycle++) {
            Invoke-SuspendCycle $cycle
        }
        $proofTimestamp = [DateTimeOffset]::UtcNow.ToString('o')
        $proofCommand = "proof=${remoteSuspendProbeProof}.new.`$`$; printf 'result=passed\ncycles=$cycleCount\nutc=$proofTimestamp\n' > `$proof; chmod 600 `$proof; mv `$proof $remoteSuspendProbeProof; sync"
        Invoke-CheckedRemote $proofCommand | Out-Null
        Write-Evidence 'suspend-probe-proof.txt' "path=$remoteSuspendProbeProof$([Environment]::NewLine)result=passed$([Environment]::NewLine)cycles=$cycleCount$([Environment]::NewLine)utc=$proofTimestamp$([Environment]::NewLine)"
    }
} catch {
    Write-Evidence 'failure.txt' ($_.Exception.ToString() + [Environment]::NewLine)
    throw
} finally {
    if ($uploaded -and $null -ne $script:Serial) {
        Invoke-Remote "rm -f $remoteScript $remoteSuspendResult" -AllowFailure | Out-Null
    }
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $uploadRoot
}

Write-Host 'Power sampling completed.'
if ($suspendRequested) { Write-Host 'Requested suspend cycle(s) completed and ADB recovery was verified.' }
Write-Host "Evidence: $runRoot"