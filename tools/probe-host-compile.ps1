param(
    [Parameter(Mandatory)][string]$ServerExe,
    [Parameter(Mandatory)][string]$KernelDll,
    [switch]$MetadataOnly,
    [switch]$GraphicsV3,
    [switch]$DefinitionMetadata
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
            $body = @{command=$case.command; include_workspace_values=[bool]$GraphicsV3} | ConvertTo-Json
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
                    if ((!$GraphicsV3 -and $graph.version -ne 1) -or $legends.Count -ne 0) { throw "Unexpected implicit legend/version; see $root" }
                } else {
                    $title = @($graph.objects | Where-Object id -eq $axes[0].labels.Title)
                    if ((!$GraphicsV3 -and $graph.version -ne 2) -or $title.Count -ne 1 -or $title[0].type -ne 'Text' -or
                        $title[0].properties.String -ne 'Gain' -or $axes[0].properties.XGrid -ne 'on' -or
                        $lines[0].properties.Clipping -ne 'off' -or $lines[1].properties.Clipping -ne 'on' -or
                        $legends.Count -ne 1 -or $legends[0].axes -ne $axes[0].id -or
                        $legends[0].entries.Count -ne 2 -or $legends[0].entries[0].line -ne $lines[0].id -or
                        $legends[0].entries[0].label -ne 'Measured' -or $legends[0].entries[1].line -ne $lines[1].id) {
                        throw "Incorrect annotated graph; see $root"
                    }
                }
            }
            if ($GraphicsV3) {
                if ($graph.version -ne 3 -or !$graph.graphIdentity) { throw "Missing v3 graph: $root" }
                if ($case.name -eq 'matrix') { $graphId = $graph.graphIdentity; $lineId = $lines[0].objectIdentity }
                if ($graph.graphIdentity -cne $graphId) { throw "Graph identity changed: $root" }
                if ($case.name -ne 'closed') {
                    $name = if ($case.name -eq 'recreated') { 'fresh' } else { 'lines' }
                    $array = ($result.workspace | Where-Object name -eq $name).value.graphics
                    $g = $array.graphs[0]
                    $node = @($g.objects | Where-Object id -eq $array.elements[0].reference)
                    if ($g.version -ne 3 -or $g.graphIdentity -cne $graphId -or $node.Count -ne 1 -or
                        !$node[0].objectIdentity -or $node[0].objectIdentity -cne $lines[0].objectIdentity -or
                        (($case.name -eq 'recreated') -eq ($lines[0].objectIdentity -ceq $lineId))) {
                        throw "Result/workspace identity mismatch: $root"
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
    if ($DefinitionMetadata) {
        $unicode = [char]0xE9
        $nested = "% UTF-8: $unicode`r`nfunction y=first(x)`r`ny=helper(x);`r`nfunction z=helper(x)`r`nz=x;`r`nend`r`nend`r`nfunction y=second(x)`r`ny=helper(x);`r`nfunction z=helper(x)`r`nz=x+1;`r`nend`r`nend`r`n"
        foreach ($case in @(
            @{name='nested'; source=$nested; names=@('first','helper','second','helper'); kinds=@('primary-function','nested-function','local-function','nested-function')},
            @{name='class'; source="classdef RangeClass`nmethods`nfunction y=first(obj)`ny=1;`nend`nfunction y=second(obj)`ny=2;`nend`nend`nend`n"; names=@('first','second'); kinds=@('method','method')},
            @{name='no_execution'; source="error('must not execute');`nfunction y=helper(x)`ny=x;`nend`n"; names=@('helper'); kinds=@('local-function')},
            @{name='invalid'; source='a=;'; names=@(); kinds=@()}
        )) {
            $sourceName = 'definition_' + $case.name + '.m'
            $body = @{source=$case.source; source_name=$sourceName} | ConvertTo-Json
            $response = Invoke-WebRequest "$base/compile" -UseBasicParsing -Method Post -ContentType 'application/json' -Body $body
            [IO.File]::WriteAllText((Join-Path $root ('definitions_' + $case.name + '.json')), [string]$response.Content)
            $result = $response.Content | ConvertFrom-Json
            $directory = $result.definition_directory
            $valid = $case.name -ne 'invalid'
            if ($result.valid -ne $valid -or $directory.complete -ne $valid -or
                $directory.schema -ne 'mexec.definition-directory' -or $directory.version -ne 1 -or
                $directory.source_name -cne $sourceName -or $directory.source_utf8 -cne $case.source -or
                $directory.offset_unit -ne 'utf8-byte' -or $directory.column_unit -ne 'utf8-byte' -or
                $directory.line_base -ne 1 -or $directory.column_base -ne 1 -or !$directory.end_exclusive -or
                $directory.definitions.Count -ne $case.names.Count) { throw "Incorrect definition directory: $($case.name); see $root" }
            $bytes = [Text.Encoding]::UTF8.GetBytes($case.source)
            for ($i = 0; $i -lt $case.names.Count; ++$i) {
                $entry = $directory.definitions[$i]
                if ($entry.index -ne $i -or $entry.name -cne $case.names[$i] -or $entry.kind -ne $case.kinds[$i] -or
                    $entry.source_index -ne 0 -or $entry.source.name -cne $sourceName -or $entry.declaration_only -ne $false -or
                    $entry.source.end.offset -le $entry.source.begin.offset -or $entry.source.end.offset -gt $bytes.Length) {
                    throw "Incorrect definition record $i in $($case.name); see $root"
                }
                foreach ($position in @($entry.source.begin, $entry.source.end)) {
                    $prefix = [Text.Encoding]::UTF8.GetString($bytes, 0, [int]$position.offset)
                    $lines = $prefix.Split("`n")
                    if ($position.line -ne $lines.Length -or
                        $position.column -ne (1 + [Text.Encoding]::UTF8.GetByteCount($lines[-1]))) {
                        throw "Incorrect byte/line/column units: $($case.name); see $root"
                    }
                }
                $token = [Text.Encoding]::UTF8.GetString($bytes, [int]$entry.source.begin.offset, 8)
                if ($token -cne 'function') { throw "Definition does not start at function token; see $root" }
                if ($case.name -eq 'class' -and $entry.class_name -cne 'RangeClass') { throw "Missing owning class; see $root" }
            }
            if ($case.name -eq 'nested') {
                $d = $directory.definitions
                if ($null -ne $d[0].parent_function -or $d[1].parent_function -ne 0 -or
                    $null -ne $d[2].parent_function -or $d[3].parent_function -ne 2 -or
                    $d[1].source.end.offset -gt $d[0].source.end.offset -or
                    $d[3].source.end.offset -gt $d[2].source.end.offset) { throw "Incorrect nested hierarchy; see $root" }
            }
            Write-Output "definitions/$($case.name): valid=$($result.valid), records=$($directory.definitions.Count)"
        }
    }
    Write-Output "Kernel SHA256: $expected"
    Write-Output "Evidence: $root"
} finally {
    if (!$server.HasExited) { $server.Kill(); $server.WaitForExit() }
    $server.Dispose()
}
