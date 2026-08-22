clear;

seed = 40;
value = eval('seed + 2');
[rows, columns] = eval('size(ones(2, 3))');

captured = evalc('disp(7)');
assignin('base', 'missing_row', [missing missing]);
missing_row = evalin('base', 'missing_row');

sin = [10 20 30];
shadowed_last = eval('sin(end)');

partial = 1;
try
    eval('partial = 9; error(''Demo:Expected'', ''stop'')');
catch
end
recovered = eval('error(''Demo:Expected'', ''stop'')', 'partial + 1');

scope_marker = 5;
[caller_value, eval_caller_value] = caller_roundtrip();
summary = value + rows + columns + numel(missing_row) + ...
    recovered + caller_value + shadowed_last + ...
    eval_caller_value + scope_marker

function [result, caller_seen] = caller_roundtrip()
    caller_seen = eval( ...
        'evalin(''caller'', ''scope_marker'')');
    eval('assignin(''caller'', ''scope_marker'', 6)');
    assign_from_helper();
    result = read_from_helper();
end

function assign_from_helper()
    assignin('caller', 'caller_number', 41);
end

function value = read_from_helper()
    value = evalin('caller', 'caller_number + 1');
end
