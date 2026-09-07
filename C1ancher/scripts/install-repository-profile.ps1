[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$ProfileDirectory,
    [string]$Serial,
    [switch]$SkipNetworkVerification,
    [string]$AdbPath = "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe"
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$ProfileDirectory = [IO.Path]::GetFullPath($ProfileDirectory)
foreach ($file in @('repository.url', 'core-repository.url', 'repository.ed25519.pub', 'device-repository-config.sh', 'c1-update-check.sh', 'SHA256SUMS')) {
    if (-not (Test-Path -LiteralPath (Join-Path $ProfileDirectory $file) -PathType Leaf)) { throw "Missing profile member: $file" }
}
if (-not (Test-Path -LiteralPath $AdbPath)) { $AdbPath = (Get-Command adb -ErrorAction Stop).Source }
function Invoke-Adb([string[]]$Arguments) {
    $preference = $ErrorActionPreference
    try {
        # adb reports successful transfer progress on stderr under PowerShell 5.
        $ErrorActionPreference = 'Continue'
        $lines = & $AdbPath @Arguments 2>&1
        $result = $LASTEXITCODE
    } finally { $ErrorActionPreference = $preference }
    if ($result -ne 0) { throw "ADB operation failed: $($lines -join [Environment]::NewLine)" }
    return ($lines -join "`n")
}
if ([string]::IsNullOrWhiteSpace($Serial)) {
    $devices = Invoke-Adb @('devices')
    $ready = @($devices -split "`r?`n" | Where-Object { $_ -match '^(\S+)\s+device$' })
    if ($ready.Count -ne 1) { throw 'Pass -Serial when more than one device is connected.' }
    $Serial = [regex]::Match($ready[0], '^(\S+)').Groups[1].Value
}
function Remote([string]$Command) {
    $output = Invoke-Adb @('-s', $Serial, 'shell', "$Command; result=`$?; echo __C1_PROFILE_EXIT=`$result")
    if ($output -notmatch '(?m)^__C1_PROFILE_EXIT=0\r?$') { throw "Device operation failed: $output" }
    return ($output -replace '(?m)^__C1_PROFILE_EXIT=0\r?$', '').Trim()
}
Remote 'test "$(id -u)" = 0 && test -x /usr/data/c1/bin/c1pkg' | Out-Null
$remote = '/dev/shm/c1-profile-' + [Guid]::NewGuid().ToString('N')
try {
    Remote "mkdir -m 700 $remote" | Out-Null
    foreach ($file in @('repository.url', 'core-repository.url', 'repository.ed25519.pub', 'device-repository-config.sh', 'c1-update-check.sh', 'SHA256SUMS')) {
        Invoke-Adb @('-s', $Serial, 'push', (Join-Path $ProfileDirectory $file), "$remote/$file") | Out-Null
    }
    Remote "sh $remote/device-repository-config.sh $remote" | Write-Output
    if (-not $SkipNetworkVerification) { Remote '/usr/data/c1/bin/c1pkg refresh' | Write-Output }
    Write-Output "Repository configured on device $Serial. Application files and user data were preserved."
} finally { Remote "rm -rf $remote" | Out-Null }
