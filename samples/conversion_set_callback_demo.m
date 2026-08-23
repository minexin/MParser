integer_rows = int2str([1.2 -12.7; 1000 NaN]);
assert(strcmp(integer_rows(1, :), '   1   -13'));
assert(strcmp(integer_rows(2, :), '1000   NaN'));

matrix_source = mat2str(single([1 2; 3 4]), 15, 'class');
parsed_matrix = str2num('[1 2; 3 4]');
parsed_range = str2num('1:2:7');
parsed_function = str2num('sin(pi / 2)');
blocked_source = str2num('system(''echo unsafe'')');
assert(strcmp(matrix_source, 'single([1 2;3 4])'));
assert(isequal(parsed_matrix, [1 2; 3 4]));
assert(isequal(parsed_range, [1 3 5 7]));
assert(abs(parsed_function - 1) < 1e-12);
assert(isempty(blocked_source));

cube = reshape(1:12, 2, 3, 2);
scalar_cells = num2cell(cube);
grouped_cells = num2cell(cube, [1 3]);
joined_blocks = cell2mat({1, [2 3]; [4; 5], [6 7; 8 9]});
empty_joined = cell2mat({[], [10 11]});
nd_cells = cell(1, 1, 2);
nd_cells{1, 1, 1} = ones(2, 3, 1);
nd_cells{1, 1, 2} = 2 * ones(2, 3, 2);
nd_joined = cell2mat(nd_cells);
missing_joined = cell2mat({repmat(missing, 1, 2), ...
    repmat(missing, 1, 3)});
assert(iscellstr({'alpha', ['b'; 'c']}));
assert(~iscellstr({'alpha', "beta"}));
assert(isequal(size(scalar_cells), [2 3 2]));
assert(isequal(size(grouped_cells), [1 3]));
assert(isequal(grouped_cells{2}, reshape([3 4 9 10], 2, 1, 2)));
assert(isequal(joined_blocks, [1 2 3; 4 6 7; 5 8 9]));
assert(isequal(empty_joined, [10 11]));
assert(isequal(size(nd_joined), [2 3 3]) && ...
    all(nd_joined(:, :, 1) == 1, 'all') && ...
    all(nd_joined(:, :, 2:3) == 2, 'all'));
assert(isequal(size(missing_joined), [1 5]) && ...
    all(ismissing(missing_joined), 'all'));

squares = arrayfun(@(value)value ^ 2, [1 2; 3 4]);
logical_map = arrayfun(@(value)value > 1, [1 2; 3 4]);
empty_map = arrayfun(@(value)value + 1, zeros(2, 0));
nd_map = arrayfun(@(value)value + 1, reshape(1:8, 2, 2, 2));
nonuniform = arrayfun(@pair_value, [1 2 3], 'UniformOutput', false);
[first, second] = arrayfun(@pair_outputs, [1 2 3]);
recovered = arrayfun(@fail_on_two, [1 2 3], ...
    'ErrorHandler', @recover_value);
assert(isequal(squares, [1 4; 9 16]));
assert(isa(logical_map, 'logical') && ...
    isequal(logical_map, logical([0 1; 1 1])));
assert(isempty(empty_map) && isequal(size(empty_map), [2 0]));
assert(isequal(size(nd_map), [2 2 2]) && nd_map(2, 2, 2) == 9);
assert(isequal(nonuniform{2}, [2 3]));
assert(isequal(first, [1 2 3]) && isequal(second, [11 12 13]));
assert(isequal(recovered, [1 22 3]));

text_values = ["Alpha", string(missing); "", "beta"];
has_fragment = contains(text_values, ["ph", "ET"]);
starts_a = startsWith(text_values, "a", 'IgnoreCase', true);
ends_a = endsWith(text_values, ["a", "TA"]);
cell_has_fragment = contains({'Alpha', 'beta'; '', 'Gamma'}, ...
    {'mm', 'PH'}, 'IgnoreCase', true);
assert(isequal(has_fragment, logical([1 0; 0 0])));
assert(isequal(starts_a, logical([1 0; 0 0])));
assert(isequal(ends_a, logical([1 0; 0 1])));
assert(isequal(cell_has_fragment, logical([1 0; 0 1])));
assert(all(contains(["abc", ""], ""), 'all'));

[combined, combined_a, combined_b] = ...
    union([3 NaN 1 3 NaN], [2 3 NaN], 'stable');
[common, common_a, common_b] = ...
    intersect([3 NaN 1 3], [2 3 NaN]);
[different, different_a] = setdiff([3 NaN 1 3], [2 3 NaN]);
[exclusive, exclusive_a, exclusive_b] = ...
    setxor([3 NaN 1 3], [2 3 NaN]);
[members, locations] = ismember([3 NaN 1 3], [2 3 NaN 3]);
[missing_numeric, missing_numeric_a, missing_numeric_b] = ...
    union(missing, single(1));
[cell_union, cell_union_a, cell_union_b] = ...
    union({'beta', 'alpha', 'beta'}, {'gamma', 'alpha'}, 'stable');
[string_union, string_union_a, string_union_b] = ...
    union(["b", string(missing)], [string(missing), "a"]);
character_union = union('cab', 'bd');
integer_union = union(int8([2 1]), [3 2]);
[complex_members, complex_locations] = ...
    ismember([1 + 2i, 3], [3, 1 + 2i]);
[row_members, row_locations] = ismember([1 2; 3 4; 1 2], ...
    [3 4; 1 2], 'rows');
assert(combined(1) == 3 && isnan(combined(2)) && combined(3) == 1);
assert(isequal(combined_a, [1; 2; 3; 5]) && ...
    isequal(combined_b, [1; 3]));
assert(isequal(common, 3) && common_a == 1 && common_b == 2);
assert(different(1) == 1 && isnan(different(2)) && ...
    isequal(different_a, [3; 2]));
assert(isequal(exclusive(1:2), [1 2]) && ...
    sum(isnan(exclusive)) == 2);
assert(isequal(exclusive_a, [3; 2]) && isequal(exclusive_b, [1; 3]));
assert(isequal(members, logical([1 0 0 1])) && ...
    isequal(locations, [2 0 0 2]));
assert(isa(missing_numeric, 'single') && missing_numeric(1) == single(1) && ...
    isnan(missing_numeric(2)) && isequal(missing_numeric_a, 1) && ...
    isequal(missing_numeric_b, 1));
assert(strcmp(cell_union{1}, 'beta') && strcmp(cell_union{2}, 'alpha') && ...
    strcmp(cell_union{3}, 'gamma') && isequal(cell_union_a, [1; 2]) && ...
    isequal(cell_union_b, 1));
assert(strcmp(string_union(1), "a") && strcmp(string_union(2), "b") && ...
    ismissing(string_union(3)) && ismissing(string_union(4)) && ...
    isequal(string_union_a, [1; 2]) && isequal(string_union_b, [2; 1]));
assert(strcmp(character_union, 'abcd'));
assert(isa(integer_union, 'int8') && ...
    isequal(integer_union, int8([1 2 3])));
assert(isequal(complex_members, logical([1 1])) && ...
    isequal(complex_locations, [2 1]));
assert(isequal(row_members, logical([1; 1; 1])) && ...
    isequal(row_locations, [2; 1; 2]));

summary = 31

function value = pair_value(input)
value = [input input + 1];
end

function [first, second] = pair_outputs(input)
first = input;
second = input + 10;
end

function value = fail_on_two(input)
if input == 2
    error('Demo:ArrayfunFailure', 'expected callback failure');
end
value = input;
end

function value = recover_value(info, input)
value = info.index * 10 + input;
end
