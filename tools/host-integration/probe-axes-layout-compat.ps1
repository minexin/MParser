param(
    [Parameter(Mandatory)][string]$Stage,
    [Parameter(Mandatory)][string]$OldSdk,
    [Parameter(Mandatory)][string]$NewSdk
)
$ErrorActionPreference = 'Stop'
$stagePath = (Resolve-Path -LiteralPath $Stage).Path
$old = (Resolve-Path -LiteralPath $OldSdk).Path
$new = (Resolve-Path -LiteralPath $NewSdk).Path
$source = Join-Path $stagePath 'server'
$build = Join-Path $stagePath 'build'
$probe = Join-Path $PSScriptRoot 'test-axes-layout.cjs'
$writeback = Join-Path $PSScriptRoot 'test-graphics-writeback.cjs'
$server = Join-Path $build 'mexec_server.exe'
try {
    cmake -S $source -B $build "-DMParser_DIR=$old/lib/cmake/MParser" -DMEXEC_GRAPHICS_WRITEBACK=ON -DMEXEC_GRAPHICS_LAYOUT_ENVIRONMENT=OFF
    if ($LASTEXITCODE) { throw 'Old SDK layout OFF configure failed' }
    cmake --build $build --target mexec_server --parallel 4
    if ($LASTEXITCODE) { throw 'Old SDK layout OFF build failed' }
    node $probe $server (Join-Path $stagePath 'layout-off-old-sdk') off
    if ($LASTEXITCODE) { throw 'Old SDK layout OFF probe failed' }
    node $writeback $server (Join-Path $stagePath 'layout-old-sdk-writeback')
    if ($LASTEXITCODE) { throw 'Old SDK legacy writeback probe failed' }
    Copy-Item -LiteralPath (Join-Path $new 'bin/mparser_c.dll') -Destination (Join-Path $build 'mparser_c.dll')
    node $probe $server (Join-Path $stagePath 'layout-off-old-sdk-new-dll') off
    if ($LASTEXITCODE) { throw 'Old-header host with new DLL layout OFF probe failed' }
    node $writeback $server (Join-Path $stagePath 'layout-old-sdk-new-dll-writeback')
    if ($LASTEXITCODE) { throw 'Old-header host with new DLL writeback probe failed' }
    cmake -S $source -B $build "-DMParser_DIR=$old/lib/cmake/MParser" -DMEXEC_GRAPHICS_LAYOUT_ENVIRONMENT=ON
    if (!$LASTEXITCODE) { throw 'Old SDK incorrectly accepted layout environment ON' }
    Write-Output 'PASS old SDK rejects explicit layout environment ON during configuration'
} finally {
    cmake -S $source -B $build "-DMParser_DIR=$new/lib/cmake/MParser" -DMEXEC_GRAPHICS_WRITEBACK=ON -DMEXEC_GRAPHICS_LAYOUT_ENVIRONMENT=ON
    if ($LASTEXITCODE) { throw 'Restoring new SDK layout ON configure failed' }
    cmake --build $build --parallel 4
    if ($LASTEXITCODE) { throw 'Restoring new SDK layout ON build failed' }
}
node $probe $server (Join-Path $stagePath 'layout-restored-on')
if ($LASTEXITCODE) { throw 'Restored layout ON probe failed' }
ctest --test-dir $build --output-on-failure --parallel 4
if ($LASTEXITCODE) { throw 'Restored layout ON host regression failed' }
Write-Output "PASS layout environment compatibility and restored ON host: $stagePath"
