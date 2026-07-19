factor = 3;
transform = @(value)value * factor;
factor = 10;

closure_value = transform(4);
[named_first, named_second] = feval(@pair, 7);
[text_first, text_second] = feval('pair', 8);

pair_handle = str2func('pair');
[converted_first, converted_second] = pair_handle(9);

values = [10 20 30];
dynamic_target = values;
indexed_value = dynamic_target(2);
dynamic_target = transform;
called_value = dynamic_target(5);

sin = [4 5];
shadow_index = sin(2);
sin_handle = str2func('sin');
sin_value = feval(sin_handle, 0);

anonymous_name = func2str(transform);
pair_name = func2str(pair_handle);
details = functions(transform);
detail_function = details.function;
detail_type = details.type;
captured_factor = details.workspace{1}.factor;

summary = closure_value + named_first + named_second + text_first + ...
    text_second + converted_first + converted_second + indexed_value + ...
    called_value + shadow_index + captured_factor + sin_value;

function [first, second] = pair(value)
first = value;
second = value + 1;
end
