param([switch]$Play)
$ErrorActionPreference = 'Stop'
$linuxPath = (& wsl.exe -d Ubuntu-22.04 -- wslpath -a $PSScriptRoot).Trim()
if ($LASTEXITCODE -ne 0) { throw 'WSL Ubuntu-22.04 is required.' }
& wsl.exe -d Ubuntu-22.04 -- bash -lc "cd '$linuxPath' && make host test target verify package"
if ($LASTEXITCODE -ne 0) { throw 'Build or tests failed.' }
if ($Play) {
    Write-Host 'Open http://127.0.0.1:8768 in your browser. Ctrl+C stops the host preview.'
    & wsl.exe -d Ubuntu-22.04 -- bash -lc "cd '$linuxPath' && python3 tools/host_viewer.py"
}
