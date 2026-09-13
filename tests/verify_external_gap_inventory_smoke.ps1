#requires -Version 7.0
$ErrorActionPreference = 'Stop'
$verifier = Join-Path $PSScriptRoot '../tools/verify_external_gap_inventory.ps1'
$plan = Join-Path $PSScriptRoot '../docs/v1.x-external-gap-plan.md'
$inventory = [regex]::Match((Get-Content $plan -Raw),
    '(?s)## Capability Inventory\s+(.*?)## V1.x Coverage Tracks')
$ids = @([regex]::Matches($inventory.Groups[1].Value, '`(cap_[A-Za-z0-9_]+)`') |
    ForEach-Object { $_.Groups[1].Value })
if ($ids.Count -ne 89) { throw 'Unexpected baseline inventory' }
# Synthetic records test validator behavior only; they are not engine evidence.
$cases = @($ids + @(1..134 | ForEach-Object { "synthetic_control_$_" }) |
    ForEach-Object { [ordered]@{
        id = $_; verdict = 'match'; matlab_status = 'ok'
        mparser_ok = $true; mparser_exit = 0
    } })
$fixture = [ordered]@{
    summary = @{ total = 223; counts = @{ match = 223 }; matlab_ok = 223
        mparser = @{ total = 223; ok = 223; failed = 0 } }
    cases = $cases
} | ConvertTo-Json -Depth 10
$temporary = Join-Path ([IO.Path]::GetTempPath()) ("mparser-inventory-" + [guid]::NewGuid() + '.json')
try {
    foreach ($mode in 'valid', 'missing', 'duplicate', 'failure', 'summary', 'substitution') {
        $report = $fixture | ConvertFrom-Json
        switch ($mode) {
            'missing' { $report.cases = @($report.cases | Select-Object -Skip 1) }
            'duplicate' { $report.cases[1].id = $report.cases[0].id }
            'failure' { $report.cases[0].mparser_ok = $false }
            'summary' { $report.summary.counts.match = 222 }
            'substitution' { $report.cases[0].id = 'synthetic_replacement' }
        }
        $report | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $temporary
        $rejected = $false
        try { & $verifier -Comparison $temporary -Plan $plan | Out-Null }
        catch { $rejected = $true }
        if ($rejected -eq ($mode -eq 'valid')) {
            throw "Unexpected inventory validator outcome: $mode"
        }
        Write-Output "PASS: $mode"
    }
} finally {
    Remove-Item -LiteralPath $temporary -ErrorAction SilentlyContinue
}
