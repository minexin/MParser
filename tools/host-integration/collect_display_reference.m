function collect_display_reference(outputFile)
% Capture real R2024b console behavior without reconstructing workspace history.
cases = {
    'assignment', 'a=1';
    'suppressed', 'a=2;';
    'comment-semicolon', 'a=3 %;';
    'reassigned', sprintf('a=4\na=5');
    'interleaved', sprintf('a=6\ndisp(''between'')\na=7');
    'partial-failure', sprintf('a=8\nerror(''DisplayProbe:Expected'',''stop'')');
    'indexed', 'a=[1,2]; a(2)=9';
    'field', 's=struct(''v'',1); s.v=2';
    'nested-index', 's=struct(''v'',[1,2]); s.v(2)=9';
    'cell', 'c={1,2}; c{1}=5';
    'multi-output', '[left,right]=display_probe_pair()';
    'repeated-target', '[a,a]=display_probe_pair()';
    'ignored-target', '[~,right]=display_probe_pair()';
    'loop', sprintf('for k=1:3\na=k\nend');
    'function-body', 'answer=display_probe_inner();';
    'bare-name', 'named=9; named';
    'warning-order', sprintf('a=1\nwarning(''DisplayProbe:Warning'',''middle'')\na=2');
};
results = struct('name',{},'source',{},'console',{},'error',{});
for index = 1:size(cases,1)
    [console, identifier] = run_case(cases{index,2});
    results(end+1) = struct('name',cases{index,1},'source',cases{index,2}, ...
        'console',console,'error',identifier); %#ok<AGROW>
end
payload = struct('version',version,'release',version('-release'),'cases',results);
file = fopen(outputFile,'w','n','UTF-8');
assert(file >= 0);
cleanup = onCleanup(@() fclose(file));
fprintf(file,'%s\n',jsonencode(payload));
fprintf('DISPLAY_REFERENCE_COMPLETE %d\n',numel(results));
end

function [text, identifier] = run_case(source)
identifier = '';
text = evalc(sprintf('try\n%s\ncatch caught\nidentifier=caught.identifier;\nend',source));
end

function [left,right] = display_probe_pair()
left=11;
right=12;
end

function y = display_probe_inner()
y=41
y=42;
end
