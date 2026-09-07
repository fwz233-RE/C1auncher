[CmdletBinding()]
param(
    [string]$AdbPath = "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe"
)

$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$testSource = Join-Path $PSScriptRoot 'device-launcher-test.sh'
$runRoot = Join-Path $projectRoot "artifacts\launcher-restart-$([DateTimeOffset]::Now.ToString('yyyyMMdd-HHmmss'))"
$uploadRoot = Join-Path ([System.IO.Path]::GetTempPath()) "C1ancher-launcher-test-$([guid]::NewGuid().ToString('N'))"
$remoteTest = '/dev/shm/C1ancher-launcher-test.sh'
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
    $result = Invoke-Remote "$Command; c1_status=`$?; echo ${marker}`$c1_status" -AllowFailure
    $match = [regex]::Match($result.Output, "(?m)^$([regex]::Escape($marker))(\d+)\r?$")
    if (-not $match.Success) { throw "Remote command did not return an exit marker: $Command`n$($result.Output)" }
    $cleanOutput = [regex]::Replace($result.Output, "(?m)^$([regex]::Escape($marker))\d+\r?$", '').TrimEnd()
    if ([int]$match.Groups[1].Value -ne 0) { throw "Remote command failed: $Command`n$cleanOutput" }
    return $cleanOutput
}

if (-not (Test-Path -LiteralPath $testSource)) { throw "Required input is missing: $testSource" }
New-Item -ItemType Directory -Force -Path $runRoot, $uploadRoot | Out-Null
$testUpload = Join-Path $uploadRoot 'device-launcher-test.sh'
[System.IO.File]::WriteAllText(
    $testUpload,
    [System.IO.File]::ReadAllText($testSource).Replace("`r`n", "`n"),
    [System.Text.UTF8Encoding]::new($false)
)
$script:Adb = Resolve-Adb

try {
    $devices = Invoke-Adb -Arguments @('devices') -WithoutSerial
    $deviceLines = @($devices.Output -split "`r?`n" | Where-Object { $_ -match '^(\S+)\s+device$' })
    if ($deviceLines.Count -ne 1) { throw "Exactly one connected ADB device is required; found $($deviceLines.Count)." }
    $script:Serial = [regex]::Match($deviceLines[0], '^(\S+)').Groups[1].Value
    if ((Invoke-Remote 'id').Output -notmatch 'uid=0\(root\)') { throw 'A root ADB shell is required.' }
    $rootMount = (Invoke-Remote 'mount').Output -split "`r?`n" | Where-Object { $_ -match '\son\s/\stype\s' } | Select-Object -First 1
    if ($rootMount -notmatch '\(ro(?:,|\))') { throw "Root filesystem is not read-only: $rootMount" }
    $adbHash = (Invoke-Remote 'sha256sum /etc/init.d/S90usb').Output.Split()[0].ToLowerInvariant()
    if ($adbHash -ne $openAdbHash) { throw "Open root ADB startup hash changed: $adbHash" }

    Invoke-Adb -Arguments @('push', $testUpload, $remoteTest) | Out-Null
    Invoke-Remote "chmod 700 $remoteTest" | Out-Null
    $uploaded = $true
    Invoke-CheckedRemote "sh -n $remoteTest" | Out-Null
    $result = Invoke-CheckedRemote "$remoteTest crash-restart"
    $result | Set-Content -Encoding utf8 -Path (Join-Path $runRoot 'result.txt')
} finally {
    if ($uploaded -and $null -ne $script:Serial) { Invoke-Remote "rm -f $remoteTest" -AllowFailure | Out-Null }
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $uploadRoot
}

Write-Host 'Repeated C1ancher crash restart passed.'
Write-Host "Evidence: $runRoot"