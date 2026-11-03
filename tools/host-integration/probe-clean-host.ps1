param(
    [Parameter(Mandatory)][string]$ServerRoot,
    [Parameter(Mandatory)][string]$ClientRoot,
    [Parameter(Mandatory)][string]$SdkPrefix,
    [int]$Parallel = 4
)
$ErrorActionPreference = 'Stop'
$env:VSLANG = '1033'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$server = (Resolve-Path -LiteralPath $ServerRoot).Path
$client = (Resolve-Path -LiteralPath $ClientRoot).Path
$sdk = (Resolve-Path -LiteralPath $SdkPrefix).Path
foreach ($command in @('cmake','ctest','cl','node','git','robocopy')) {
    if (!(Get-Command $command -ErrorAction SilentlyContinue)) { throw "Missing $command; run from an x64 MSVC developer environment" }
}
$stage = Join-Path $repo ('build/clean-host-probe/' + [guid]::NewGuid().ToString('N'))
$hostSource = Join-Path $stage 'server'
$clientSource = Join-Path $stage 'client'
$build = Join-Path $stage 'build'
New-Item -ItemType Directory -Path $hostSource,$clientSource | Out-Null
Write-Output "Clean host evidence: $stage"
function Copy-Source([string]$from, [string]$to, [string]$log) {
    robocopy $from $to /E /XD .git build .cache node_modules /XF *.obj *.lib *.exe *.dll /NFL /NDL /NJH /NJS /NP | Tee-Object -FilePath (Join-Path $stage $log)
    if ($LASTEXITCODE -ge 8) { throw "Source copy failed: $from" }
}
Copy-Item -LiteralPath (Join-Path $server 'CMakeLists.txt') -Destination $hostSource
foreach ($directory in @('src','include','third_party','docs','tests')) {
    Copy-Source (Join-Path $server $directory) (Join-Path $hostSource $directory) ('copy-server-' + $directory + '.log')
}
foreach ($directory in @('web','tests')) {
    Copy-Source (Join-Path $client $directory) (Join-Path $clientSource $directory) ('copy-client-' + $directory + '.log')
}
foreach ($target in @($hostSource,$clientSource)) {
    git -C $target init -q
    if ($LASTEXITCODE) { throw "Cannot initialize patch target: $target" }
}
foreach ($patch in @('mexecserver-graphics-v3.patch','mexecserver-definition-metadata.patch',
    'mexecserver-development-acceptance.patch')) {
    git -C $hostSource apply --unidiff-zero --ignore-space-change (Join-Path $PSScriptRoot $patch)
    if ($LASTEXITCODE) { throw "Host patch failed: $patch; see $stage" }
}
foreach ($patch in @('mexecclient-text-labels-candidate.patch','mexecclient-line-clipping-candidate.patch',
    'mexecclient-grid-candidate.patch','mexecclient-legend-candidate.patch')) {
    git -C $clientSource apply --unidiff-zero --ignore-space-change (Join-Path $PSScriptRoot $patch)
    if ($LASTEXITCODE) { throw "Client patch failed: $patch; see $stage" }
}
$node = (Get-Command node).Source
cmake -S $hostSource -B $build -G Ninja -DCMAKE_BUILD_TYPE=Release `
    "-DMParser_DIR=$sdk/lib/cmake/MParser" "-DMEXEC_CLIENT_WEB_DIR=$clientSource/web" `
    "-DMEXEC_NODE_EXECUTABLE=$node" -DMEXEC_BUILD_TESTS=ON -DMEXEC_WARNINGS_AS_ERRORS=ON `
    -DMEXEC_GRAPHICS_V3=ON -DMEXEC_DEFINITION_METADATA=ON `
    -DMEXEC_KERNEL_MILESTONE=development | Tee-Object -FilePath (Join-Path $stage 'configure.log')
if ($LASTEXITCODE) { throw "Clean host configure failed; see $stage" }
cmake --build $build --parallel $Parallel | Tee-Object -FilePath (Join-Path $stage 'build.log')
if ($LASTEXITCODE) { throw "Clean host build failed; see $stage" }
& (Join-Path $repo 'tools/probe-host-compile.ps1') -ServerExe (Join-Path $build 'mexec_server.exe') `
    -KernelDll (Join-Path $sdk 'bin/mparser_c.dll') -GraphicsV3 -DefinitionMetadata -ExpectedProductVersion '1.15.0' |
    Tee-Object -FilePath (Join-Path $stage 'kernel-probe.log')
foreach ($test in @('test-client-text-labels.cjs','test-client-clipping.cjs','test-client-legend.cjs')) {
    node (Join-Path $PSScriptRoot $test) $clientSource
    if ($LASTEXITCODE) { throw "Client candidate failed: $test; see $stage" }
}
ctest --test-dir $build --output-on-failure --parallel $Parallel |
    Tee-Object -FilePath (Join-Path $stage 'ctest.log')
if ($LASTEXITCODE) { throw "Host regression failed; see $stage" }
Write-Output "Clean host build, REST and regression passed: $stage"
