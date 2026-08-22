selector = 3;
switch selector
    case {1, 2}
        selected = 12;
    case {3, 4}
        selected = 34;
    otherwise
        selected = -1;
end

values = {1, 2, 3};
expanded = [values{:}];
[first, second] = values{1:2};

[nested_value, nested_after] = nested_demo(5);

records(1).value = 10;
records(3).value = 30;
gap_empty = isempty(records(2).value);

tree(2).node.value = 7;

summary = selected + sum(expanded) + first + second + ...
          nested_value + nested_after + records(1).value + ...
          records(3).value + gap_empty + tree(2).node.value;

function [value, after] = nested_demo(seed)
shared = seed;
value = bump(2);
after = shared;

    function result = bump(delta)
    shared = shared + delta;
    result = shared * 2;
    end
end
