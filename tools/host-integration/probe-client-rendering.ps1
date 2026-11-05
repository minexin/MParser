param([Parameter(Mandatory)][string]$ClientRoot)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$source = (Resolve-Path -LiteralPath $ClientRoot).Path
$stage = Join-Path $root ('build/client-rendering-probe/' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path (Join-Path $stage 'web/js'),(Join-Path $stage 'web/css'),(Join-Path $stage 'tests') | Out-Null
Copy-Item -LiteralPath (Join-Path $source 'web/js/plot.js') -Destination (Join-Path $stage 'web/js/plot.js')
Copy-Item -LiteralPath (Join-Path $source 'web/js/app.js') -Destination (Join-Path $stage 'web/js/app.js')
Copy-Item -LiteralPath (Join-Path $source 'web/js/api.js') -Destination (Join-Path $stage 'web/js/api.js')
Copy-Item -LiteralPath (Join-Path $source 'web/js/plot-view.js') -Destination (Join-Path $stage 'web/js/plot-view.js')
Copy-Item -LiteralPath (Join-Path $source 'web/js/layout.js') -Destination (Join-Path $stage 'web/js/layout.js')
Copy-Item -LiteralPath (Join-Path $source 'web/css/mexec.css') -Destination (Join-Path $stage 'web/css/mexec.css')
Copy-Item -LiteralPath (Join-Path $source 'web/index.html') -Destination (Join-Path $stage 'web/index.html')
Copy-Item -LiteralPath (Join-Path $source 'tests/graphics_snapshot.test.js') -Destination (Join-Path $stage 'tests/graphics_snapshot.test.js')
git -C $stage init -q
if ($LASTEXITCODE) { throw 'Unable to initialize isolated patch target' }
foreach ($patch in @('mexecclient-text-labels-candidate.patch','mexecclient-line-clipping-candidate.patch',
    'mexecclient-grid-candidate.patch','mexecclient-legend-candidate.patch','mexecclient-color-candidate.patch',
    'mexecclient-runtime-identity-candidate.patch','mexecclient-axes-layout-candidate.patch','mexecclient-layout-sync-candidate.patch',
    'mexecclient-legend-references-candidate.patch','mexecclient-compact-layout-candidate.patch')) {
    git -C $stage apply --unidiff-zero --ignore-space-change (Join-Path $PSScriptRoot $patch)
    if ($LASTEXITCODE) { throw "Candidate does not apply: $patch; see $stage" }
}
foreach ($test in @('test-client-text-labels.cjs','test-client-clipping.cjs','test-client-legend.cjs','test-client-colors.cjs',
    'test-client-runtime-identity.cjs','test-client-axes-layout.cjs','test-client-layout-bootstrap.cjs',
    'test-client-compact-layout.cjs','test-client-compact-canvas.cjs')) {
    node (Join-Path $PSScriptRoot $test) $stage
    if ($LASTEXITCODE) { throw "Candidate failed: $test; see $stage" }
}
Write-Output "Isolated client: $stage"
Write-Output 'Simulated canvas only; deployed client was not modified.'
