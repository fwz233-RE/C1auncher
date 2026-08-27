[CmdletBinding()]
param(
    [string]$CrossCompile = 'mipsel-linux-gnu-'
)

$ErrorActionPreference = 'Stop'

if ($CrossCompile -notmatch '^[A-Za-z0-9_./+-]+$') {
    throw 'CrossCompile contains unsupported characters.'
}

$projectRoot = Split-Path -Parent $PSScriptRoot
$fullRoot = [System.IO.Path]::GetFullPath($projectRoot)
$drive = $fullRoot.Substring(0, 1).ToLowerInvariant()
$relative = $fullRoot.Substring(3).Replace('\', '/')
$wslRoot = "/mnt/$drive/$relative"

$buildStdout = Join-Path ([System.IO.Path]::GetTempPath()) ("C1ancher-build-{0}.out" -f [Guid]::NewGuid())
$buildStderr = Join-Path ([System.IO.Path]::GetTempPath()) ("C1ancher-build-{0}.err" -f [Guid]::NewGuid())
try {
    $buildProcess = Start-Process `
        -FilePath 'wsl.exe' `
        -ArgumentList @('make', '-j1', '-C', $wslRoot, 'clean', 'host-test', 'all', "CROSS_COMPILE=$CrossCompile") `
        -NoNewWindow `
        -PassThru `
        -Wait `
        -RedirectStandardOutput $buildStdout `
        -RedirectStandardError $buildStderr
    if ($buildProcess.ExitCode -ne 0) {
        if (Test-Path -LiteralPath $buildStdout) {
            Get-Content -LiteralPath $buildStdout | Write-Host
        }
        if (Test-Path -LiteralPath $buildStderr) {
            Get-Content -LiteralPath $buildStderr | ForEach-Object {
                Write-Host $_ -ForegroundColor Red
            }
        }
        throw "Build failed with exit code $($buildProcess.ExitCode)."
    }
    Write-Host 'Host tests and MIPS cross-build passed.'
} finally {
    Remove-Item -Force -ErrorAction SilentlyContinue -LiteralPath $buildStdout, $buildStderr
}

& (Join-Path $PSScriptRoot 'validate-keyboard.ps1')

$targets = @(
    [pscustomobject]@{
        Name = 'C1ancher'
        AbiPath = Join-Path $projectRoot 'build\abi.txt'
        BinaryPath = Join-Path $projectRoot 'build\C1ancher'
    },
    [pscustomobject]@{
        Name = 'C1ancher-launcher'
        AbiPath = Join-Path $projectRoot 'build\launcher-abi.txt'
        BinaryPath = Join-Path $projectRoot 'build\C1ancher-launcher'
    }
)
$requiredPatterns = @(
    'Class:\s+ELF32',
    'Data:\s+2''s complement, little endian',
    'Flags:.*o32.*mips32r2',
    'FP ABI:\s+Hard float \(double precision\)'
)

$buildInfo = @(
    "built_at=$([DateTimeOffset]::Now.ToString('o'))"
    "cross_compile=$CrossCompile"
    'target=mips32r2-little-o32-hard-float-double-static'
    'mode=app'
)
foreach ($target in $targets) {
    $abi = Get-Content -Raw -Path $target.AbiPath
    foreach ($pattern in $requiredPatterns) {
        if ($abi -notmatch $pattern) {
            throw "$($target.Name) ABI verification failed: missing pattern $pattern"
        }
    }
    if ($abi -match 'Requesting program interpreter' -or $abi -notmatch 'There is no dynamic section') {
        throw "$($target.Name) is not fully static."
    }
    $hash = (Get-FileHash -Algorithm SHA256 -Path $target.BinaryPath).Hash.ToLowerInvariant()
    $size = (Get-Item -Path $target.BinaryPath).Length
    $buildInfo += "$($target.Name)_sha256=$hash"
    $buildInfo += "$($target.Name)_size_bytes=$size"
    Write-Host "Built $($target.BinaryPath)"
    Write-Host "SHA-256 $hash"
    Write-Host "Size $size bytes"
}

$neofetchRoot = Join-Path $projectRoot 'third_party\neofetch'
foreach ($name in @('neofetch', 'neofetch.upstream', 'c1-config.conf', 'c1-logo.txt', 'LICENSE.md')) {
    $path = Join-Path $neofetchRoot $name
    if (-not (Test-Path -LiteralPath $path)) {
        throw "Bundled Neofetch file is missing: $path"
    }
    $key = $name.Replace('.', '_').Replace('-', '_')
    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash.ToLowerInvariant()
    $buildInfo += "neofetch_${key}_sha256=$hash"
}
$buildInfo += 'neofetch_version=7.1.0'
$buildInfo | Set-Content -Encoding utf8 -Path (Join-Path $projectRoot 'build\build-info.txt')
