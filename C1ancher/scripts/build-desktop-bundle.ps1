[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateScript({ $_ -gt 0 })][uint64]$Sequence,
    [ValidateScript({ $_ -gt 0 })][uint64]$SecurityEpoch = 1,
    [ValidatePattern('^[0-9]+\.[0-9]+\.[0-9]+$')][string]$ImeVersion = '0.1.0',
    [Parameter(Mandatory)][ValidateRange(1, 253402300799)][long]$SourceDateEpoch,
    [string]$OutputDirectory,
    [string]$CoreSigningKey = (Join-Path $env:LOCALAPPDATA 'C1ancher\secrets\core-repo-ed25519.pem'),
    [string]$AppSigningKey = (Join-Path $env:LOCALAPPDATA 'C1ancher\secrets\app-repo-ed25519.pem'),
    [string]$OpenSslPath = 'openssl.exe',
    [string]$WslDistribution = 'Ubuntu-22.04',
    # Private, local development only. Never use this bundle as a production release.
    [switch]$AllowDirtySource
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$project = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$repo = Split-Path -Parent $project
$version = [IO.File]::ReadAllText((Join-Path $project 'VERSION')).Trim()
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $project "build\desktop-bundle\$Sequence-$version"
}
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $OutputDirectory) { throw 'Output already exists. Choose a new immutable bundle directory.' }
$prefix = $repo.TrimEnd('\') + '\'
foreach ($key in @($CoreSigningKey, $AppSigningKey)) {
    $full = [IO.Path]::GetFullPath($key)
    if ($full.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'Private keys must be outside the whole repository.' }
    if (-not (Test-Path -LiteralPath $full -PathType Leaf)) { throw 'Existing external signing keys are required; no automatic key generation.' }
}
$dirty = @(& git -C $repo status --porcelain --untracked-files=all)
if ($LASTEXITCODE -ne 0) { throw 'Cannot inspect source identity.' }
if ($dirty.Count -and -not $AllowDirtySource) { throw 'Commit intended source first, or explicitly select -AllowDirtySource for private development.' }
if ($dirty.Count) { Write-Warning 'Building a PRIVATE DEVELOPMENT bundle from uncommitted source; not for publication.' }
function WslPath([string]$Path) {
    $full = [IO.Path]::GetFullPath($Path)
    if ($full -notmatch '^([A-Za-z]):\\(.*)$') { throw 'A local Windows drive path is required.' }
    return '/mnt/' + $Matches[1].ToLowerInvariant() + '/' + $Matches[2].Replace('\', '/')
}
function Linux([string[]]$Arguments) {
    & wsl.exe -d $WslDistribution -- @Arguments
    if ($LASTEXITCODE -ne 0) { throw "Local Linux build/test failed ($LASTEXITCODE). No device was contacted." }
}
# An isolated build directory avoids deleting existing installers, previews, or
# the developer's previous outputs. No make clean and no automatic deployment.
$work = Join-Path $project ('build\desktop-work-' + [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($work) | Out-Null
$coreBuild = Join-Path $work 'core-build'
$runtime = Join-Path $work 'runtime'
$trust = Join-Path $work 'trust'
[IO.Directory]::CreateDirectory($trust) | Out-Null
Linux @('python3', (WslPath (Join-Path $repo 'term-ime\integration\prepare-runtime.py')),
        '--output', (WslPath $runtime))
$report = [IO.File]::ReadAllText((Join-Path $runtime 'reports\report.json')) | ConvertFrom-Json
if (-not $report.success -or $report.device_tested) { throw 'Expected successful host-only MIPS runtime report.' }
Linux @('make', '-j2', '-C', (WslPath $project), ('BUILD_DIR=' + (WslPath $coreBuild)), 'host-test', 'all')
Linux @('make', '-j2', '-C', (WslPath $project), ('BUILD_DIR=' + (WslPath $coreBuild)), 'lifecycle-test')
Linux @('python3', (WslPath (Join-Path $project 'tests\test_desktop_bundle.py')))
# build-core-release still checks dirty identity, exact VERSION, ABI, size,
# signatures and immutable output; SkipBuild uses only the fresh tested build.
& (Join-Path $PSScriptRoot 'build-core-release.ps1') -Sequence $Sequence -Version $version `
    -SecurityEpoch $SecurityEpoch -SourceDateEpoch $SourceDateEpoch -MinimumBootstrap '1.1.0' `
    -MinimumUpdater '1.1.0' -BuildDirectory $coreBuild -OutputDirectory (Join-Path $OutputDirectory 'core') `
    -SigningKeyPath $CoreSigningKey -PublicKeyRawOutputPath (Join-Path $trust 'core.pub') `
    -PublicKeyPemOutputPath (Join-Path $trust 'core.pem') -OpenSslPath $OpenSslPath `
    -WslDistribution $WslDistribution -SkipBuild -AllowDirtySource:$AllowDirtySource
$catalog = @{ packages = @(@{ id = 'c1-ime'; version = $ImeVersion; displayName = 'Chinese Input Service';
    entry = 'bin/c1-ime-service'; payloadDirectory = (Join-Path $runtime 'payload') }) }
$catalogPath = Join-Path $work 'ime-catalog.json'
[IO.File]::WriteAllText($catalogPath, ($catalog | ConvertTo-Json -Depth 4), [Text.UTF8Encoding]::new($false))
& (Join-Path $PSScriptRoot 'build-app-package.ps1') -CatalogPath $catalogPath -Sequence $Sequence `
    -OutputDirectory (Join-Path $OutputDirectory 'apps') -SigningKeyPath $AppSigningKey `
    -OpenSslPath $OpenSslPath -WslDistribution $WslDistribution -SourceDateEpoch $SourceDateEpoch
Linux @('python3', (WslPath (Join-Path $PSScriptRoot 'prepare-desktop-install.py')), '--bundle', (WslPath $OutputDirectory),
    '--core-key', (WslPath (Join-Path $trust 'core.pub')), '--app-key',
    (WslPath (Join-Path $OutputDirectory 'apps\repository.ed25519.pub')))
[IO.File]::Copy((Join-Path $runtime 'reports\report.json'), (Join-Path $OutputDirectory 'runtime-report.json'))
[IO.File]::Copy((Join-Path $project 'docs\desktop-install.md'), (Join-Path $OutputDirectory 'README.zh-CN.md'))
Write-Host "Verified local bundle: $OutputDirectory"
Write-Host "Build evidence and derived public keys: $work"
Write-Host 'No ADB, device installation, reboot, publication, Git commit or push was performed.'
Write-Warning 'Physical keyboard/display, memory, suspend/resume and cold-boot acceptance remain pending. Public redistribution also requires completing corresponding-source/license obligations.'
