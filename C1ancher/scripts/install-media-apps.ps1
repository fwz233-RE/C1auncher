[CmdletBinding()]
param(
    [string]$BookPath,
    [string]$MusicPath,
    [switch]$InstallSampleContent,
    [string]$AdbPath = "$env:LOCALAPPDATA\Android\Sdk\platform-tools\adb.exe"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$workspaceRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))

function Resolve-Adb {
    if (Test-Path -LiteralPath $AdbPath -PathType Leaf) {
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
        [switch]$WithoutSerial
    )
    [string[]]$allArguments = if ($WithoutSerial) { $Arguments } else { @('-s', $script:Serial) + $Arguments }
    $previousPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $lines = & $script:Adb @allArguments 2>&1
        $exitCode = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousPreference
    }
    if ($exitCode -ne 0) {
        $output = ($lines | ForEach-Object { $_.ToString() }) -join [Environment]::NewLine
        throw "ADB failed: adb $($allArguments -join ' ')`n$output"
    }
    return (($lines | ForEach-Object { $_.ToString() }) -join [Environment]::NewLine).Trim()
}

function Invoke-Remote([string]$Command) {
    return Invoke-Adb -Arguments @('shell', $Command)
}

function Resolve-OnlyContent([string]$Path, [string]$Directory, [string]$Filter) {
    if (-not [string]::IsNullOrWhiteSpace($Path)) {
        if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
            throw "Content file is missing: $Path"
        }
        return (Resolve-Path -LiteralPath $Path).Path
    }
    $matches = @(Get-ChildItem -LiteralPath $Directory -File -Filter $Filter)
    if ($matches.Count -ne 1) {
        throw "Expected exactly one $Filter file in $Directory; found $($matches.Count)."
    }
    return $matches[0].FullName
}

function Push-ContentFile([string]$Source, [string]$RemotePath) {
    $metadata = [IO.Path]::GetTempFileName()
    try {
        [IO.File]::WriteAllText($metadata, [IO.Path]::GetFileName($Source), [Text.UTF8Encoding]::new($false))
        Invoke-Adb -Arguments @('push', $Source, $RemotePath) | Write-Host
        Invoke-Adb -Arguments @('push', $metadata, "$RemotePath.name") | Write-Host
    } finally {
        Remove-Item -Force -ErrorAction SilentlyContinue -LiteralPath $metadata
    }
}

if ($InstallSampleContent) {
    $BookPath = Resolve-OnlyContent $BookPath (Join-Path $workspaceRoot 'Book') '*.txt'
    $MusicPath = Resolve-OnlyContent $MusicPath (Join-Path $workspaceRoot 'Music') '*.mp3'
}

$script:Adb = Resolve-Adb
$devices = Invoke-Adb -Arguments @('devices') -WithoutSerial
$deviceLines = @($devices -split "`r?`n" | Where-Object { $_ -match '^(\S+)\s+device$' })
if ($deviceLines.Count -ne 1) {
    throw "Exactly one connected ADB device is required; found $($deviceLines.Count)."
}
$script:Serial = [regex]::Match($deviceLines[0], '^(\S+)').Groups[1].Value

$identity = Invoke-Remote 'id'
if ($identity -notmatch 'uid=0\(root\)') {
    throw 'A root ADB shell is required.'
}

if ($InstallSampleContent) {
    Invoke-Remote 'mkdir -p /storage/mtp/Book /storage/mtp/Music && test ! -e /storage/mtp/Book/book-001.txt && test ! -e /storage/mtp/Music/track-001.mp3' | Out-Null
    Push-ContentFile $BookPath '/storage/mtp/Book/book-001.txt'
    Push-ContentFile $MusicPath '/storage/mtp/Music/track-001.mp3'
}

Invoke-Remote '/usr/data/c1/bin/c1pkg refresh' | Write-Host
$available = Invoke-Remote '/usr/data/c1/bin/c1pkg available'
if ($available -notmatch '(?m)^book-reader\s' -or $available -notmatch '(?m)^music-player\s') {
    throw "The refreshed repository does not expose both media applications.`n$available"
}
Invoke-Remote '/usr/data/c1/bin/c1pkg install book-reader' | Write-Host
Invoke-Remote '/usr/data/c1/bin/c1pkg install music-player' | Write-Host
$installed = Invoke-Remote '/usr/data/c1/bin/c1pkg list'
$installedVersions = @{}
foreach ($line in ($installed -split "`r?`n")) {
    $fields = @($line -split '\s+')
    if ($fields.Count -ge 2) { $installedVersions[$fields[0]] = $fields[1] }
}
foreach ($id in @('book-reader', 'music-player')) {
    if (-not $installedVersions.ContainsKey($id)) { throw "Installation did not verify for $id." }
}
$bookVersion = Invoke-Remote '/usr/data/c1/bin/c1pkg launch book-reader --version'
$musicVersion = Invoke-Remote '/usr/data/c1/bin/c1pkg launch music-player --version'
if ($bookVersion.Trim() -cne "c1book-reader $($installedVersions['book-reader'])" -or
    $musicVersion.Trim() -cne "c1music-player $($installedVersions['music-player'])") {
    throw "Package metadata and executable versions disagree.`n$bookVersion`n$musicVersion"
}
# Installation never removes or overwrites the user's old book/music directories.

Write-Output "Installed book-reader and music-player on $($script:Serial)."
Write-Output $bookVersion
Write-Output $musicVersion