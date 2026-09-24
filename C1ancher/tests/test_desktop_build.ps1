# End-to-end build smoke with throwaway keys OUTSIDE the repository.
# The resulting bundle is deliberately NOT trusted by any device. No ADB.
[CmdletBinding()]
param([string]$OpenSslPath = 'C:\Program Files\Git\usr\bin\openssl.exe')
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$project = Split-Path -Parent $PSScriptRoot
$temporary = Join-Path ([IO.Path]::GetTempPath()) ('c1-desktop-testkeys-' + [Guid]::NewGuid().ToString('N'))
$output = Join-Path $project ('build\desktop-test-bundle-' + [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($temporary) | Out-Null
try {
    $core = Join-Path $temporary 'core.pem'
    $app = Join-Path $temporary 'app.pem'
    foreach ($key in @($core, $app)) {
        & $OpenSslPath genpkey -algorithm ED25519 -out $key
        if ($LASTEXITCODE -ne 0) { throw 'Cannot create ephemeral test key.' }
    }
    & (Join-Path $project 'scripts\build-desktop-bundle.ps1') -Sequence 1 -SecurityEpoch 1 `
        -SourceDateEpoch 1700000000 -OutputDirectory $output -CoreSigningKey $core -AppSigningKey $app `
        -OpenSslPath $OpenSslPath -AllowDirtySource
    [IO.File]::WriteAllText((Join-Path $output 'TEST-KEYS-DO-NOT-INSTALL.txt'),
        "TEST ONLY. These throwaway signing keys are unrelated to device trust roots. Never install or publish this bundle.`n")
    Write-Host "PASS: full build, QEMU, package/core signing and verification with ephemeral test keys: $output"
} finally {
    Remove-Item -LiteralPath $temporary -Recurse -Force
}
