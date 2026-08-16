joined = ['ab' 'cd'];
assert(strcmp(joined, 'abcd'));

lowered = lower('AbC');
uppered = upper(["Alpha", "bETA"]);
assert(strcmp(lowered, 'abc'));
assert(all(strcmp(uppered, ["ALPHA", "BETA"]), "all"));

trimmed_char = strtrim('  padded text  ');
trimmed_strings = strtrim([" left ", " right  "]);
assert(strcmp(trimmed_char, 'padded text'));
assert(all(strcmp(trimmed_strings, ["left", "right"]), "all"));

scalar_text = num2str(3.14);
matrix_text = num2str([1 20; 300 4]);
precision_text = num2str(pi, 3);
integer_text = num2str(uint64(42));
complex_text = num2str(1 + 2i);
assert(strcmp(scalar_text, '3.14'));
assert(size(matrix_text, 1) == 2);
assert(strcmp(precision_text, '3.14'));
assert(strcmp(integer_text, '42'));
assert(strcmp(complex_text, '1+2i'));

[parts, delimiters] = strsplit('a,,b', ',', ...
    'CollapseDelimiters', false);
assert(numel(parts) == 3 && strcmp(parts{2}, ''));
assert(numel(delimiters) == 2);

digit_runs = regexp('a1b22', '\d+', 'match');
[starts, finishes] = regexp('a1b22', '\d+', 'start', 'end');
first_run = regexp('a1b22', '\d+', 'once', 'match');
assert(numel(digit_runs) == 2 && strcmp(digit_runs{2}, '22'));
assert(isequal(starts, [2 4]) && isequal(finishes, [2 5]));
assert(strcmp(first_run, '1'));

[ordered, order_index] = sort([3 NaN 1 2], 'ascend', ...
    'MissingPlacement', 'last');
assert(isequaln(ordered, [1 2 3 NaN]));
assert(isequal(order_index, [3 4 1 2]));

[row_ordered, row_index] = sort([3 1; 2 4], 2, 'descend');
assert(isequal(row_ordered, [3 1; 4 2]));
assert(isequal(row_index, [1 2; 2 1]));

missing_row = [missing missing missing];
[missing_ordered, missing_index] = sort(missing_row);
assert(isequal(size(missing_ordered), [1 3]));
assert(isequal(missing_index, [1 2 3]));

[distinct, first_index, inverse_index] = unique([3 1 3 2 1]);
assert(isequal(distinct, [1 2 3]));
assert(isequal(first_index, [2; 4; 1]));
assert(isequal(inverse_index, [3; 1; 3; 2; 1]));

[stable_values, last_index, stable_inverse] = ...
    unique([3 1 3 2 1], 'stable', 'last');
assert(isequal(stable_values, [3 1 2]));
assert(isequal(last_index, [3; 5; 4]));
assert(isequal(stable_inverse, [1; 2; 1; 3; 2]));

[unique_rows, row_first, row_inverse] = ...
    unique([2 1; 1 2; 2 1], 'rows');
assert(isequal(unique_rows, [1 2; 2 1]));
assert(isequal(row_first, [2; 1]));
assert(isequal(row_inverse, [2; 1; 2]));

unique_characters = unique('banana');
unique_strings = unique(["beta", "alpha", "beta"]);
unique_missing = unique(missing_row);
[distinct_missing, missing_first, missing_inverse] = unique( ...
    missing_row, 'TreatMissingAsDistinct', true);
assert(strcmp(unique_characters, 'abn'));
assert(all(strcmp(unique_strings, ["alpha", "beta"]), "all"));
assert(isscalar(unique_missing) && ismissing(unique_missing));
assert(isequal(size(distinct_missing), [1 3]));
assert(isequal(missing_first, [1; 2; 3]));
assert(isequal(missing_inverse, [1; 2; 3]));

cells = {1, 2, 3};
assert(iscell(cells) && ~iscell([1 2 3]));
doubled = cellfun(@(value)value * 2, cells);
named = cellfun('sin', {0, pi / 2});
tripled = cellfun(@triple_value, cells);
combined = cellfun(@(left, right)left + right, {1, 2}, {3, 4});
expanded = cellfun(@(value)[value value], {1, 2}, ...
    'UniformOutput', false);
[minimums, positions] = cellfun(@min, {[2 1], [4 3]});
recovered = cellfun(@(value)value(2), {10, 20}, ...
    'ErrorHandler', @(errorInfo, value)-errorInfo.index);
nested = cellfun(@(values)cellfun(@(value)value + 1, values), ...
    {{1, 2}, {3, 4}}, 'UniformOutput', false);
assert(isequal(doubled, [2 4 6]));
assert(isequal(named, [0 1]));
assert(isequal(tripled, [3 6 9]));
assert(isequal(combined, [4 6]));
assert(iscell(expanded) && isequal(expanded{2}, [2 2]));
assert(isequal(minimums, [1 3]) && isequal(positions, [2 2]));
assert(isequal(recovered, [-1 -2]));
assert(isequal(nested{1}, [2 3]) && isequal(nested{2}, [4 5]));

records = struct('a', {1, 3}, 'b', {2, 4});
record_cells = struct2cell(records);
rebuilt = cell2struct({5, 6}, {'x', 'y'}, 2);
assert(isequal(size(record_cells), [2 1 2]));
assert(record_cells{1, 1, 2} == 3);
assert(rebuilt.x == 5 && rebuilt.y == 6);

summary = 12

function output = triple_value(value)
output = value * 3;
end
