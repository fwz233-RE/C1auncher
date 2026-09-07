# Local public-key validation tests. Never dot-source the device installer:
# only selected function ASTs are loaded, so ADB and maintenance cannot run.
[CmdletBinding()]
param([string]$OpenSslPath = 'C:/Program Files/Git/usr/bin/openssl.exe')
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$scriptPath = Join-Path $PSScriptRoot '../scripts/install-core-enrollment.ps1'
$tokens = $null
$parseErrors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile($scriptPath, [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count -ne 0) { throw "Installer syntax errors: $parseErrors" }
foreach ($name in @('Invoke-Native', 'Assert-EnrollmentPublicKeys', 'Assert-MaintenanceKeyHash')) {
    $function = $ast.FindAll({ param($node) $node -is [System.Management.Automation.Language.FunctionDefinitionAst] }, $false) | Where-Object Name -eq $name
    if (@($function).Count -ne 1) { throw "Missing validation function $name" }
    . ([scriptblock]::Create($function.Extent.Text))
}
function Expect-Rejected([scriptblock]$Operation, [string]$Label) {
    $rejected = $false
    try { & $Operation } catch { $rejected = $true }
    if (-not $rejected) { throw "FAIL: $Label was accepted" }
}
$root = Join-Path ([IO.Path]::GetTempPath()) ('c1-public-validation-' + [Guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory($root)
try {
    $raw = Join-Path $root 'test-public.raw'
    $other = Join-Path $root 'different-public.raw'
    $short = Join-Path $root 'short-public.raw'
    $der = Join-Path $root 'test-public.der'
    $pem = Join-Path $root 'test-public.pem'
    $prefix = [byte[]]@(0x30,0x2a,0x30,0x05,0x06,0x03,0x2b,0x65,0x70,0x03,0x21,0x00)
    $public = [byte[]](0..31)
    [IO.File]::WriteAllBytes($raw, $public)
    [IO.File]::WriteAllBytes($other, [byte[]](1..32))
    [IO.File]::WriteAllBytes($short, [byte[]](0..30))
    [IO.File]::WriteAllBytes($der, [byte[]]($prefix + $public))
    [void](Invoke-Native $OpenSslPath @('pkey', '-pubin', '-inform', 'DER', '-in', $der, '-out', $pem))
    Assert-EnrollmentPublicKeys $OpenSslPath $pem $raw
    Expect-Rejected { Assert-EnrollmentPublicKeys $OpenSslPath $pem $other } 'unrelated PEM/raw pair'
    Expect-Rejected { Assert-EnrollmentPublicKeys $OpenSslPath $pem $short } 'short raw key'
    $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $raw).Hash.ToLowerInvariant()
    Assert-MaintenanceKeyHash $raw $hash
    Expect-Rejected { Assert-MaintenanceKeyHash $other $hash } 'replaced maintenance trust anchor'
    Expect-Rejected { Assert-MaintenanceKeyHash $raw 'invalid' } 'invalid device key digest'
    Write-Output 'PASS: six local enrollment public-key checks; no ADB or installer execution'
} finally {
    Remove-Item -LiteralPath $root -Recurse -Force
}
