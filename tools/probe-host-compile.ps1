param(
    [Parameter(Mandatory)][string]$ServerExe,
    [Parameter(Mandatory)][string]$KernelDll,
    [switch]$MetadataOnly
)
$ErrorActionPreference = 'Stop'
$root = Join-Path $PSScriptRoot ('../build/host-compile-probe/' + [guid]::NewGuid().ToString('N'))
$root = (New-Item -ItemType Directory -Path $root).FullName
$workspace = (New-Item -ItemType Directory -Path (Join-Path $root 'workspace')).FullName
$exe = Join-Path $root 'mexec_server.exe'
$dll = Join-Path $root 'mparser_c.dll'
Copy-Item -LiteralPath $ServerExe -Destination $exe
Copy-Item -LiteralPath $KernelDll -Destination $dll
$expected = (Get-FileHash -LiteralPath $KernelDll).Hash
$listener = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
$listener.Start()
$port = $listener.LocalEndpoint.Port
$listener.Stop()
$base = "http://127.0.0.1:$port/api/v1"
$server = Start-Process -FilePath $exe -WorkingDirectory $root -WindowStyle Hidden -PassThru -ArgumentList @(
    '--host', '127.0.0.1', '--port', $port, '--file-root', ('"' + $workspace + '"')
) -RedirectStandardOutput (Join-Path $root 'stdout.log') -RedirectStandardError (Join-Path $root 'stderr.log')
try {
    $health = $null
    for ($i = 0; $i -lt 80 -and !$health; ++$i) {
        if ($server.HasExited) { throw "Server exited: $($server.ExitCode); see $root" }
        try { $health = Invoke-RestMethod "$base/health" -TimeoutSec 2 } catch { Start-Sleep -Milliseconds 100 }
    }
    if (!$health) { throw "No health response; see $root" }
    $module = (Get-Process -Id $server.Id).Modules | Where-Object ModuleName -eq 'mparser_c.dll'
    if (!$module -or $module.FileName -ne $dll -or (Get-FileHash -LiteralPath $module.FileName).Hash -ne $expected) {
        throw 'Loaded kernel does not match requested DLL'
    }
    if (!$MetadataOnly) {
    $session = Invoke-RestMethod "$base/sessions" -Method Post -ContentType 'application/json' -Body '{"name":"compile-probe"}'
    foreach ($case in @(
        @{name='valid'; source='a=1;'},
        @{name='missing_rhs'; source="a=1;`nb=;"; line=2; column=2},
        @{name='reserved_lhs'; source='end=0;'; line=1; column=1}
    )) {
        $body = @{source=$case.source; source_name=($case.name + '.m'); compile_only=$true} | ConvertTo-Json
        $response = Invoke-WebRequest "$base/sessions/$($session.id)/execute" -UseBasicParsing -Method Post -ContentType 'application/json' -Body $body
        [IO.File]::WriteAllText((Join-Path $root ($case.name + '.json')), [string]$response.Content)
        $result = $response.Content | ConvertFrom-Json
        if ($null -ne $result.execution -or $result.session.variable_count -ne 0) {
            throw "Compile-only request unexpectedly executed or changed workspace: $($case.name); see $root"
        }
        if ($case.name -eq 'valid') {
            if ($result.status -ne 'succeeded' -or $result.diagnostics.Count -ne 0) { throw "Valid control failed; see $root" }
        } else {
            $diagnostic = @($result.diagnostics | Where-Object {
                $_.identifier -eq 'MParser:CompilationFailed' -and $_.severity -eq 'error' -and
                $_.phase -eq 'compilation' -and $_.source.name -eq ($case.name + '.m') -and
                $_.source.begin.line -eq $case.line -and $_.source.begin.column -eq $case.column
            })
            if ($result.status -ne 'compilation-failed' -or !$diagnostic.Count) {
                throw "Incorrect rejection or diagnostic for $($case.name); see $root"
            }
        }
        Write-Output "$($case.name): $($result.status)"
    }
    $identifiers = @(
        @{name='seed'; command='original=42;'},
        @{name='ordinary'; predicate="isvarname('new_name2')"},
        @{name='reserved'; predicate="~isvarname('end') && iskeyword('end')"},
        @{name='contextual'; predicate="isvarname('arguments') && ~iskeyword('arguments')"},
        @{name='initial'; predicate="~isvarname('_bad')"},
        @{name='limit'; predicate="namelengthmax()==63 && isvarname('$('a'*63)') && ~isvarname('$('a'*64)')"}
    )
    foreach ($case in $identifiers) {
        $command = $case.command
        if ($case.predicate) {
            $command = "if ~($($case.predicate)) || original~=42; error('identifier probe failed'); end;"
        }
        $body = @{command=$command} | ConvertTo-Json
        $response = Invoke-WebRequest "$base/sessions/$($session.id)/execute" -UseBasicParsing -Method Post -ContentType 'application/json' -Body $body
        [IO.File]::WriteAllText((Join-Path $root ("identifier_" + $case.name + '.json')), [string]$response.Content)
        $result = $response.Content | ConvertFrom-Json
        if ($result.status -ne 'succeeded' -or $result.diagnostics.Count -ne 0 -or
            $result.session.variable_count -ne 1 -or $result.workspace_summary[0].name -ne 'original') {
            throw "Incorrect identifier result or workspace change: $($case.name); see $root"
        }
        Write-Output "identifier/$($case.name): $($result.status)"
    }
    }
    $metadataSession = Invoke-RestMethod "$base/sessions" -Method Post -ContentType 'application/json' -Body '{"name":"metadata-export-probe"}'
    $source = Get-Content -LiteralPath (Join-Path $PSScriptRoot '../samples/dynamic_properties_demo.m') -Raw
    $source += "`nnested_descriptor={computed_property};"
    foreach ($case in @(
        @{name='metadata_export'; request=@{source=$source; source_name='dynamic_properties_demo.m'; include_workspace_values=$true}},
        @{name='metadata_followup'; request=@{command="d=nested_descriptor{1}; if summary~=68 || ~strcmp(d.Name,'Computed') || ~strcmp(class(d),'matlab.metadata.DynamicProperty'); error('metadata readback failed'); end;"}}
    )) {
        $body = $case.request | ConvertTo-Json
        $response = Invoke-WebRequest "$base/sessions/$($metadataSession.id)/execute" -UseBasicParsing -Method Post -ContentType 'application/json' -Body $body
        [IO.File]::WriteAllText((Join-Path $root ($case.name + '.json')), [string]$response.Content)
        $result = $response.Content | ConvertFrom-Json
        if ($result.status -ne 'succeeded' -or $result.workspace_export_failed -ne $false -or
            $result.workspace_summary.name -notcontains 'computed_property' -or
            $result.workspace_summary.name -notcontains 'nested_descriptor' -or
            $result.workspace_summary.name -notcontains 'summary') {
            throw "Metadata export/readback failed: $($case.name); see $root"
        }
        if ($case.name -eq 'metadata_export') {
            $values = @{}
            foreach ($item in $result.workspace) { $values[$item.name] = $item.value }
            foreach ($descriptor in @($values.computed_property, $values.nested_descriptor.data[0])) {
                if ($descriptor.class -ne 'matlab.metadata.DynamicProperty' -or !$descriptor.handle -or
                    $descriptor.representation -ne 'opaque') { throw "Incorrect exported descriptor; see $root" }
            }
            if ($values.summary.class -ne 'double' -or $values.summary.data[0] -ne 68) {
                throw "Incorrect exported summary; see $root"
            }
        }
        Write-Output "$($case.name): $($result.status)"
    }
    if (!$MetadataOnly) {
        $gs = Invoke-RestMethod "$base/sessions" -Method Post -ContentType 'application/json' -Body '{"name":"graphics-probe"}'
        foreach ($case in @(
            @{name='matrix'; command="lines=plot([1,10;2,20;3,30]); ax=gca(); first=lines(1); first.DisplayName='Measured';"},
            @{name='annotated'; command="title(ax,'Gain'); grid(ax,'on'); ax.YLim=[0,5]; first.Clipping='off'; lg=legend(ax);"},
            @{name='closed'; command="close all; if isvalid(lg) || isvalid(first); error('closed handles still live'); end;"},
            @{name='recreated'; command="fresh=plot([1,10;2,20;3,30]); if isvalid(first) || ~isvalid(fresh(1)); error('incorrect recreated handle lifetime'); end;"}
        )) {
            $body = @{command=$case.command} | ConvertTo-Json
            $response = Invoke-WebRequest "$base/sessions/$($gs.id)/execute" -UseBasicParsing -Method Post -ContentType 'application/json' -Body $body
            [IO.File]::WriteAllText((Join-Path $root ("graphics_" + $case.name + '.json')), [string]$response.Content)
            $result = $response.Content | ConvertFrom-Json
            $graph = $result.graphics
            if ($result.status -ne 'succeeded' -or $result.workspace_export_failed -ne $false -or $graph.schema -ne 'mparser.graphics') {
                throw "Graphics request/transport failed: $($case.name); see $root"
            }
            if ($case.name -eq 'closed') {
                if ($graph.objects.Count -ne 0) { throw "Expected empty graph; see $root" }
            } else {
                $axes = @($graph.objects | Where-Object type -eq 'Axes')
                $lines = @($graph.objects | Where-Object type -eq 'Line')
                $legends = @($graph.objects | Where-Object type -eq 'Legend')
                if ($axes.Count -ne 1 -or $lines.Count -ne 2 -or ($lines[0].properties.YData -join ',') -ne '1,2,3' -or
                    ($lines[1].properties.YData -join ',') -ne '10,20,30') { throw "Incorrect matrix plot; see $root" }
                Write-Output "graphics/$($case.name) first Line snapshot id: $($lines[0].id)"
                if ($case.name -in @('matrix','recreated')) {
                    if ($graph.version -ne 1 -or $legends.Count -ne 0) { throw "Unexpected implicit legend/version; see $root" }
                } else {
                    $title = @($graph.objects | Where-Object id -eq $axes[0].labels.Title)
                    if ($graph.version -ne 2 -or $title.Count -ne 1 -or $title[0].type -ne 'Text' -or
                        $title[0].properties.String -ne 'Gain' -or $axes[0].properties.XGrid -ne 'on' -or
                        $lines[0].properties.Clipping -ne 'off' -or $lines[1].properties.Clipping -ne 'on' -or
                        $legends.Count -ne 1 -or $legends[0].axes -ne $axes[0].id -or
                        $legends[0].entries.Count -ne 2 -or $legends[0].entries[0].line -ne $lines[0].id -or
                        $legends[0].entries[0].label -ne 'Measured' -or $legends[0].entries[1].line -ne $lines[1].id) {
                        throw "Incorrect annotated graph; see $root"
                    }
                }
            }
            Write-Output "graphics/$($case.name): $($result.status)"
        }
        foreach ($export in @('print','saveas')) {
            $target = Join-Path $workspace ("export_" + $export + '.pdf')
            $quoted = $target.Replace('\','/').Replace("'","''")
            $command = if ($export -eq 'print') { "print(gcf(),'$quoted','-dpdf');" }
                else { "saveas(gcf(),'$quoted','pdf');" }
            $body = @{command=$command} | ConvertTo-Json
            $response = Invoke-WebRequest "$base/sessions/$($gs.id)/execute" -UseBasicParsing -Method Post -ContentType 'application/json' -Body $body
            [IO.File]::WriteAllText((Join-Path $root ("graphics_export_" + $export + '.json')), [string]$response.Content)
            $result = $response.Content | ConvertFrom-Json
            if ($result.status -ne 'runtime-failed' -or (Test-Path -LiteralPath $target) -or
                $result.diagnostics[0].identifier -ne 'MParser:GraphicsRendererUnavailable') {
                throw "Expected explicit export failure and no PDF for $export; see $root"
            }
            Write-Output "graphics/export/$export : $($result.status); $($result.diagnostics[0].message)"
        }
        $sentinel = Join-Path $workspace 'existing.pdf'
        [IO.File]::WriteAllText($sentinel, 'existing file must survive')
        $hash = (Get-FileHash -LiteralPath $sentinel).Hash
        $q = $sentinel.Replace('\','/').Replace("'","''")
        foreach ($case in @(
            @{name='existing'; cmd="saveas(gcf(),'$q');"; id='GraphicsRendererUnavailable'},
            @{name='deleted'; cmd="saveas(first,'$q');"; id='InvalidExportFigure'},
            @{name='axes'; cmd="print(gca(),'$q','-dpdf');"; id='InvalidExportFigure'},
            @{name='path'; cmd="saveas(gcf(),'','pdf');"; id='InvalidGraphicsExport'},
            @{name='format'; cmd="print(gcf(),'$q','-dunknown');"; id='UnsupportedExportFormat'}
        )) {
            $body = @{command=$case.cmd} | ConvertTo-Json
            $response = Invoke-WebRequest "$base/sessions/$($gs.id)/execute" -UseBasicParsing -Method Post -ContentType 'application/json' -Body $body
            [IO.File]::WriteAllText((Join-Path $root ("graphics_export_" + $case.name + '.json')), [string]$response.Content)
            $result = $response.Content | ConvertFrom-Json
            if ($result.status -ne 'runtime-failed' -or $result.diagnostics[0].identifier -ne ('MParser:' + $case.id) -or
                (Get-FileHash -LiteralPath $sentinel).Hash -ne $hash) { throw "Unexpected export result: $($case.name); see $root" }
            Write-Output "graphics/export/$($case.name): $($result.diagnostics[0].identifier)"
        }
    }
    Write-Output "Kernel SHA256: $expected"
    Write-Output "Evidence: $root"
} finally {
    if (!$server.HasExited) { $server.Kill(); $server.WaitForExit() }
    $server.Dispose()
}
