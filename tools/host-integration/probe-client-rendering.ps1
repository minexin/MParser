param([Parameter(Mandatory)][string]$ClientRoot)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$source = (Resolve-Path -LiteralPath $ClientRoot).Path
$stage = Join-Path $root ('build/client-rendering-probe/' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path (Join-Path $stage 'web/js'),(Join-Path $stage 'tests') | Out-Null
Copy-Item -LiteralPath (Join-Path $source 'web/js/plot.js') -Destination (Join-Path $stage 'web/js/plot.js')
Copy-Item -LiteralPath (Join-Path $source 'tests/graphics_snapshot.test.js') -Destination (Join-Path $stage 'tests/graphics_snapshot.test.js')
git -C $stage init -q
if ($LASTEXITCODE) { throw 'Unable to initialize isolated patch target' }
foreach ($patch in @('mexecclient-text-labels-candidate.patch','mexecclient-line-clipping-candidate.patch',
    'mexecclient-grid-candidate.patch','mexecclient-legend-candidate.patch')) {
    git -C $stage apply --unidiff-zero --ignore-space-change (Join-Path $PSScriptRoot $patch)
    if ($LASTEXITCODE) { throw "Candidate does not apply: $patch; see $stage" }
}
foreach ($test in @('test-client-text-labels.cjs','test-client-clipping.cjs','test-client-legend.cjs')) {
    node (Join-Path $PSScriptRoot $test) $stage
    if ($LASTEXITCODE) { throw "Candidate failed: $test; see $stage" }
}
Write-Output "Isolated client: $stage"
Write-Output 'Simulated canvas only; deployed client was not modified.'
