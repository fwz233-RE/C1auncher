[CmdletBinding()]
param(
    [string]$Path
)

$ErrorActionPreference = 'Stop'
if ([string]::IsNullOrWhiteSpace($Path)) {
    $Path = Join-Path (Split-Path -Parent $PSScriptRoot) 'config\c1-slim\keyboard.csv'
}
$rows = @(Import-Csv -LiteralPath $Path)

if ($rows.Count -ne 40) {
    throw "Expected 40 keyboard entries, found $($rows.Count)."
}

$duplicates = @($rows | Group-Object source_node, key_code | Where-Object Count -ne 1)
if ($duplicates.Count -ne 0) {
    throw "Duplicate source/key mappings found: $($duplicates.Name -join ', ')"
}

$matrix = @($rows | Where-Object source_node -eq '/dev/input/event0')
$gpio = @($rows | Where-Object source_node -eq '/dev/input/event1')
if ($matrix.Count -ne 30 -or $gpio.Count -ne 10) {
    throw "Expected matrix/gpio counts 30/10, found $($matrix.Count)/$($gpio.Count)."
}

if (@($matrix | Where-Object { $_.scan_code -notmatch '^\d+$' }).Count -ne 0) {
    throw 'Every matrix key must have a numeric scan code.'
}
if (@($matrix | Group-Object scan_code | Where-Object Count -ne 1).Count -ne 0) {
    throw 'Matrix scan codes must be unique.'
}
if (@($gpio | Where-Object { -not [string]::IsNullOrWhiteSpace($_.scan_code) }).Count -ne 0) {
    throw 'GPIO keys must not invent scan codes.'
}
if (@($rows | Where-Object verified_run -ne '20260814-212736').Count -ne 0) {
    throw 'Every mapping must reference the verified capture run.'
}

$required = @(
    '/dev/input/event0|16|3',
    '/dev/input/event0|57|40',
    '/dev/input/event1|25|',
    '/dev/input/event1|28|',
    '/dev/input/event1|352|'
)
$actual = @($rows | ForEach-Object { "$($_.source_node)|$($_.key_code)|$($_.scan_code)" })
foreach ($mapping in $required) {
    if ($actual -notcontains $mapping) {
        throw "Required mapping is missing: $mapping"
    }
}

Write-Host 'Keyboard profile validation passed.'