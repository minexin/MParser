#requires -Version 7.0
param(
    [Parameter(Mandatory)][string]$Root,
    [Parameter(Mandatory)][string]$MparserExe,
    [Parameter(Mandatory)][string]$ResultsDir,
    [ValidateRange(1, 600)][int]$TimeoutSeconds = 30
)

$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path -LiteralPath $Root).Path
$MparserExe = (Resolve-Path -LiteralPath $MparserExe).Path
$manifestPath = Join-Path $Root 'tests/manifest.json'
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if ($manifest.cases.Count -ne $manifest.case_count) {
    throw 'Manifest case count does not match its entries'
}
if (Test-Path -LiteralPath $ResultsDir) {
    throw 'Choose a new results directory to preserve earlier evidence'
}
$ResultsDir = (New-Item -ItemType Directory -Path $ResultsDir).FullName
$rawDir = (New-Item -ItemType Directory -Path (Join-Path $ResultsDir 'raw')).FullName
$scratch = (New-Item -ItemType Directory -Path (Join-Path $ResultsDir 'workspace')).FullName
$temporary = (New-Item -ItemType Directory -Path (Join-Path $scratch 'temp')).FullName
$utf8 = [System.Text.UTF8Encoding]::new($false)
$results = foreach ($case in $manifest.cases) {
    $source = Join-Path $Root $case.file
    $info = [System.Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $MparserExe
    $info.WorkingDirectory = $scratch
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.RedirectStandardInput = $true
    $info.StandardOutputEncoding = $utf8
    $info.StandardErrorEncoding = $utf8
    $info.Environment['TMP'] = $temporary
    $info.Environment['TEMP'] = $temporary
    $info.ArgumentList.Add('--run')
    foreach ($path in $case.paths) {
        $info.ArgumentList.Add('--path=' + (Join-Path $Root $path))
    }
    $info.ArgumentList.Add($source)
    $process = [System.Diagnostics.Process]::Start($info)
    try {
        $process.StandardInput.Close()
        $outTask = $process.StandardOutput.ReadToEndAsync()
        $errTask = $process.StandardError.ReadToEndAsync()
        $timedOut = -not $process.WaitForExit($TimeoutSeconds * 1000)
        if ($timedOut) { $process.Kill($true) }
        $process.WaitForExit()
        $stdout = $outTask.GetAwaiter().GetResult()
        $stderr = $errTask.GetAwaiter().GetResult()
        $exitCode = $process.ExitCode
    } finally {
        $process.Dispose()
    }
    [System.IO.File]::WriteAllText((Join-Path $rawDir "$($case.id).stdout.txt"), $stdout, $utf8)
    [System.IO.File]::WriteAllText((Join-Path $rawDir "$($case.id).stderr.txt"), $stderr, $utf8)
    [ordered]@{
        id = $case.id; file = $case.file; category = $case.category
        scope = $case.scope; focus = $case.focus; note = $case.note
        source_sha256 = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
        exit = $exitCode; timed_out = $timedOut
        ok = ($exitCode -eq 0 -and -not $timedOut)
        stdout = $stdout; stderr = $stderr
    }
}
$summary = [ordered]@{
    engine = 'mparser'; version = (& $MparserExe --version | Select-Object -First 1)
    exe = $MparserExe
    exe_sha256 = (Get-FileHash -LiteralPath $MparserExe -Algorithm SHA256).Hash
    manifest_sha256 = (Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256).Hash
    run_at = (Get-Date).ToString('o'); total = $results.Count
    ok = @($results | Where-Object { $_.ok }).Count
    failed = @($results | Where-Object { -not $_.ok }).Count
}
$json = [ordered]@{ summary = $summary; cases = @($results) } | ConvertTo-Json -Depth 8
[System.IO.File]::WriteAllText((Join-Path $ResultsDir 'mparser_results.json'), $json, $utf8)
Write-Output "External catalog: $($summary.ok)/$($summary.total) passed; results: $ResultsDir"
