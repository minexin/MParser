function collect_expression_reference(outputFile)
% Record console and ans state directly from MATLAB, not inferred from source.
cases = {
    'bare-no-ans', 'named=9; named';
    'bare-preserves-ans', 'ans=100; named=9; named';
    'bare-suppressed', 'ans=100; named=9; named;';
    'parenthesized', 'ans=100; named=9; (named)';
    'double-parenthesized', 'ans=100; named=9; ((named))';
    'parenthesized-suppressed', 'ans=100; named=9; (named);';
    'literal', 'ans=100; 4';
    'literal-suppressed', 'ans=100; 4;';
    'binary', 'ans=100; named=9; named+1';
    'index', 'ans=100; named=[8,9]; named(2)';
    'field', 'ans=100; s=struct(''v'',7); s.v';
    'cell', 'ans=100; c={7,8}; c{1}';
    'comma-list', 'ans=100; c={7,8}; c{:}';
    'transpose', 'ans=100; named=[1,2]; named''';
    'constant-function', 'ans=100; pi';
    'shadowed-builtin', 'ans=100; pi=9; pi';
    'bare-handle', 'ans=100; callback=@sin; callback';
    'handle-call', 'ans=100; callback=@sin; callback(0)';
    'value-call', 'ans=100; expression_probe_value()';
    'value-bare', 'ans=100; expression_probe_value';
    'value-call-suppressed', 'ans=100; expression_probe_value();';
    'no-output-call', 'ans=100; expression_probe_no_output();';
    'varargout-call', 'ans=100; expression_probe_varargout()';
    'function-body', 'ans=100; report=expression_probe_scope();';
    'function-body-bare-call', 'ans=100; expression_probe_scope()';
    'named-ans', 'ans=100; ans';
    'disp-preserves-ans', 'ans=100; disp(''visible'');';
    'loop-body', sprintf('ans=100; for k=1:2\nk\nk+10\nend');
    'eval-bare', 'ans=100; named=9; eval(''named'');';
    'evalc-bare', 'ans=100; named=9; text=evalc(''named'');';
    'parenthesized-call', 'ans=100; (expression_probe_value())';
    'parenthesized-bare-call', 'ans=100; ((expression_probe_value))';
    'bare-no-output', 'ans=100; expression_probe_no_output';
    'parenthesized-no-output', 'ans=100; (expression_probe_no_output())';
    'varargout-produced', 'ans=100; expression_probe_optional()';
    'fixed-output-missing', 'ans=100; expression_probe_missing()';
    'empty-comma-list', 'ans=100; c={}; c{:}';
    'suppressed-comma-list', 'ans=100; c={7,8}; c{:};';
    'global-ans', 'global ans; ans=100; expression_probe_global(); report=ans; clear global ans;';
};
results = struct('name',{},'source',{},'console',{},'error',{},'ansPresent',{},'answer',{},'report',{},'captured',{});
for index=1:size(cases,1)
    [console,identifier,present,answer,report,captured] = run_case(cases{index,2});
    results(end+1) = struct('name',cases{index,1},'source',cases{index,2}, ...
        'console',console,'error',identifier,'ansPresent',present,'answer',answer, ...
        'report',report,'captured',captured); %#ok<AGROW>
end
payload = struct('version',version,'release',version('-release'),'cases',results);
file=fopen(outputFile,'w','n','UTF-8'); assert(file>=0);
cleanup=onCleanup(@() fclose(file));
fprintf(file,'%s\n',jsonencode(payload));
fprintf('EXPRESSION_REFERENCE_COMPLETE %d\n',numel(results));
end

function [console,identifier,present,answer,scopeReport,captured] = run_case(source)
identifier=''; scopeReport=[]; captured=''; report=[]; text='';
console=evalc(sprintf('try\n%s\ncatch caught\nidentifier=caught.identifier;\nend',source));
present=exist('ans','var')~=0;
answer=struct('class','','dimensions',[],'real',[],'imaginary',[],'description','');
if present
    answer.class=class(ans); answer.dimensions=size(ans);
    if isnumeric(ans) || islogical(ans)
        answer.real=real(ans); answer.imaginary=imag(ans);
    elseif isa(ans,'function_handle')
        answer.description=func2str(ans);
    elseif ischar(ans)
        answer.description=ans;
    end
end
if exist('report','var'), scopeReport=report; end
if exist('text','var'), captured=text; end
end

function value=expression_probe_value()
value=40+nargout;
end
function expression_probe_no_output()
local=22
end
function varargout=expression_probe_varargout()
if nargout>0, varargout={70}; end
end

function varargout=expression_probe_optional()
varargout={70};
end

function value=expression_probe_missing()
if nargout>0, value=80; end
end

function expression_probe_global()
global ans;
3+4
end

function result=expression_probe_scope()
ans=100;
named=9;
named
before=ans;
2+3
after=ans;
result=[before,after,nargout];
end
