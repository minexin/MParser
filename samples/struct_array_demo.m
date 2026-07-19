records = struct("value", {10, 20, 30, 40}, "label", "item");
picked = records([4, 2]);
packed = [picked.value];
field_name = "value";
dynamic_packed = [picked.(field_name)];
[first_pick, second_pick] = picked.value;
called = combine(picked.value);
spread_cell = {picked.value};

replacement = struct("value", 99, "label", "replacement");
records(2) = replacement;
records([1, 3]) = replacement;
assigned = [records.value];

grown = records;
grown(6) = replacement;
grown_last = grown(6).value;
gap_empty = isempty(grown(5).value);
gap_is_double = strcmp(class(grown(5).value), "double");
deleted = grown;
deleted([2, 5]) = [];
deleted_values = [deleted.value];

cell_values = reshape({1, 2, 3, 4}, 2, 2);
grid = struct("value", cell_values, "tag", "grid");
grid_21 = grid(2, 1).value;
grid_replacement = struct("value", 8, "tag", "changed");
grid(2, 2) = grid_replacement;
grid_22 = grid(2, 2).value;
grid_column = grid(:, 2);
grid_column_values = [grid_column.value];
subscripts = struct("value", {2, 2});
grid_via_csl = grid(subscripts.value).value;
column_values = reshape({7, 9}, 2, 1);
column_replacements = struct("value", column_values, "tag", "column");
grid_assigned = grid;
grid_assigned(:, 1) = column_replacements;
grid_assigned_values = [grid_assigned(:, 1).value];

empty_typed = struct("value", {});
empty_bare = struct([]);
empty_typed_flag = isempty(empty_typed);
empty_bare_flag = isempty(empty_bare);
empty_has_field = isfield(empty_typed, "value");
removed = rmfield(records, "label");
removed_values = [removed.value];

summary = sum(packed) + first_pick + second_pick + called + ...
          sum(dynamic_packed) + sum(assigned) + grown_last + ...
          gap_empty + gap_is_double + ...
          sum(deleted_values) + grid_21 + grid_22 + ...
          sum(grid_column_values) + grid_via_csl + ...
          sum(grid_assigned_values) + ...
          empty_typed_flag + empty_bare_flag + empty_has_field + ...
          sum(removed_values)

function out = combine(left, right)
out = left * 10 + right;
end
