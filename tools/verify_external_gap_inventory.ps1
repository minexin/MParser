param(
    [Parameter(Mandatory = $true)][string]$Comparison,
    [string]$Plan = "$PSScriptRoot/../docs/v1.x-external-gap-plan.md"
)
$ErrorActionPreference = 'Stop'
$planText = Get-Content -LiteralPath $Plan -Raw
$inventory = [regex]::Match($planText, '(?s)## Capability Inventory\s+(.*?)## V1.x Coverage Tracks')
if (-not $inventory.Success) { throw 'Capability inventory section missing' }
$ids = @([regex]::Matches($inventory.Groups[1].Value, '`(cap_[A-Za-z0-9_]+)`') | ForEach-Object { $_.Groups[1].Value })
if ($ids.Count -ne 89 -or @($ids | Sort-Object -Unique).Count -ne 89) {
    throw 'Expected exactly 89 unique baseline gap IDs'
}
$report = Get-Content -LiteralPath $Comparison -Raw | ConvertFrom-Json
$cases = @($report.cases)
if ($cases.Count -ne 223 -or @($cases.id | Sort-Object -Unique).Count -ne 223) {
    throw 'Expected exactly 223 unique comparison cases'
}
$byId = @{}
foreach ($case in $cases) {
    $byId[$case.id] = $case
    if ($case.verdict -ne 'match' -or $case.matlab_status -ne 'ok' -or
        $case.mparser_ok -ne $true -or $case.mparser_exit -ne 0) {
        throw "Unmatched or unsuccessful comparison case: $($case.id)"
    }
}
foreach ($id in $ids) {
    if (-not $byId.ContainsKey($id)) { throw "Missing baseline gap: $id" }
}
if ($report.summary.total -ne 223 -or $report.summary.counts.match -ne 223 -or
    $report.summary.matlab_ok -ne 223 -or $report.summary.mparser.total -ne 223 -or
    $report.summary.mparser.ok -ne 223 -or $report.summary.mparser.failed -ne 0) {
    throw 'Comparison summary disagrees with case-level evidence'
}
Write-Output 'Verified 89/89 imported gaps and 223/223 successful comparison cases.'
Write-Output 'This validates the recorded comparison, not current executable identity or broader MATLAB equivalence.'
