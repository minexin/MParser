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
parent_direct = eval('parent_increment(4)');
parent_handle = @parent_increment;
parent_handle_value = eval('parent_handle(5)');
dynamic_parent_handle = eval('@parent_increment');
dynamic_parent_handle_value = dynamic_parent_handle(6);
[nested_parent_value, nested_parent_count] = nested_parent_roundtrip();
dynamic_global_first = dynamic_global_step();
dynamic_global_second = dynamic_global_step();
dynamic_persistent_first = dynamic_persistent_step();
dynamic_persistent_second = dynamic_persistent_step();
summary = value + rows + columns + numel(missing_row) + ...
    recovered + caller_value + shadowed_last + ...
    eval_caller_value + scope_marker + parent_direct + ...
    parent_handle_value + dynamic_parent_handle_value + ...
    nested_parent_value + nested_parent_count + ...
    dynamic_global_first + dynamic_global_second + ...
    dynamic_persistent_first + dynamic_persistent_second

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

function value = parent_increment(input)
    value = input + 1;
end

function [value, counter] = nested_parent_roundtrip()
    counter = 0;
    value = eval('bump(11)');

    function result = bump(input)
        counter = counter + 1;
        result = input + counter;
    end
end

function value = dynamic_global_step()
    eval(['global DEMO_DYNAMIC_GLOBAL; ' ...
        'if isempty(DEMO_DYNAMIC_GLOBAL), DEMO_DYNAMIC_GLOBAL = 0; end; ' ...
        'DEMO_DYNAMIC_GLOBAL = DEMO_DYNAMIC_GLOBAL + 1;']);
    DEMO_DYNAMIC_GLOBAL = DEMO_DYNAMIC_GLOBAL + 10;
    value = DEMO_DYNAMIC_GLOBAL;
end

function value = dynamic_persistent_step()
    eval(['persistent demo_dynamic_persistent; ' ...
        'if isempty(demo_dynamic_persistent), ' ...
        'demo_dynamic_persistent = 0; end; ' ...
        'demo_dynamic_persistent = demo_dynamic_persistent + 1;']);
    demo_dynamic_persistent = demo_dynamic_persistent + 10;
    value = demo_dynamic_persistent;
end
