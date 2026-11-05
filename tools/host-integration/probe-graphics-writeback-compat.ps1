param(
    [Parameter(Mandatory)][string]$Stage,
    [Parameter(Mandatory)][string]$OldSdk,
    [Parameter(Mandatory)][string]$NewSdk
)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$stagePath = (Resolve-Path -LiteralPath $Stage).Path
$old = (Resolve-Path -LiteralPath $OldSdk).Path
$new = (Resolve-Path -LiteralPath $NewSdk).Path
$source = Join-Path $stagePath 'server'
$build = Join-Path $stagePath 'build'
$nodeProbe = Join-Path $PSScriptRoot 'test-graphics-writeback.cjs'
$saved = Join-Path $stagePath 'on-binaries'
New-Item -ItemType Directory -Force -Path $saved | Out-Null
Copy-Item -LiteralPath (Join-Path $build 'mexec_server.exe'),(Join-Path $build 'mparser_c.dll') -Destination $saved
cmake -S $source -B $build "-DMParser_DIR=$old/lib/cmake/MParser" -DMEXEC_GRAPHICS_WRITEBACK=OFF -DMEXEC_GRAPHICS_LAYOUT_ENVIRONMENT=OFF
if ($LASTEXITCODE) { throw 'Old SDK OFF configure failed' }
cmake --build $build --target mexec_server --parallel 4
if ($LASTEXITCODE) { throw 'Old SDK OFF build failed' }
node $nodeProbe (Join-Path $build 'mexec_server.exe') (Join-Path $stagePath 'writeback-off-old-sdk') off
if ($LASTEXITCODE) { throw 'Old SDK OFF probe failed' }
Copy-Item -LiteralPath (Join-Path $new 'bin/mparser_c.dll') -Destination (Join-Path $build 'mparser_c.dll')
node $nodeProbe (Join-Path $build 'mexec_server.exe') (Join-Path $stagePath 'writeback-off-old-sdk-new-dll') off
if ($LASTEXITCODE) { throw 'Old SDK host with new DLL probe failed' }
cmake -S $source -B $build "-DMParser_DIR=$old/lib/cmake/MParser" -DMEXEC_GRAPHICS_WRITEBACK=ON
if (!$LASTEXITCODE) { throw 'Old SDK incorrectly accepted identity writeback' }
Write-Output 'PASS old SDK rejects writeback ON during configuration'
cmake -S $source -B $build "-DMParser_DIR=$new/lib/cmake/MParser" -DMEXEC_GRAPHICS_WRITEBACK=ON -DMEXEC_GRAPHICS_LAYOUT_ENVIRONMENT=ON
if ($LASTEXITCODE) { throw 'Restoring new SDK ON configure failed' }
cmake --build $build --parallel 4
if ($LASTEXITCODE) { throw 'Restoring new SDK ON build failed' }
node $nodeProbe (Join-Path $build 'mexec_server.exe') (Join-Path $stagePath 'writeback-restored-on')
if ($LASTEXITCODE) { throw 'Restored new SDK ON probe failed' }
ctest --test-dir $build --output-on-failure --parallel 4
if ($LASTEXITCODE) { throw 'Restored host regression failed' }
Write-Output "PASS writeback compatibility and restored ON host: $stagePath"
