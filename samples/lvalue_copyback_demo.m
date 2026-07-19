root.inner.value = 7;
root.inner.data = [10, 20, 30];
root.inner.data(2) = 99;
root.inner.data(end) = 77;

root.items = struct("value", {1, 2, 3});
root.items(2).value = 20;
field_name = "value";
root.items(3).(field_name) = 31;
root.items(5).value = 55;
root.items(2).tag = 9;
items_sum = sum([root.items.value]);
item_tag = root.items(2).tag;
tag_gap_empty = isempty(root.items(1).tag);

root.created(3).value = 6;
created_value = root.created(3).value;
created_gap_empty = isempty(root.created(2).value);

root.cells = {struct("value", 4), struct("value", 5)};
root.cells{2}.value = 50;
root.more_cells{3}.value = 12;
more_cell_value = root.more_cells{3}.value;

root.grid = reshape({1, 2, 3, 4}, 2, 2);
root.grid{2, 1} = 22;
root.grid(1, 2) = {33};
grid_column = root.grid(:, 2);
grid_column_sum = grid_column{1} + grid_column{2};

root.inner.data(1) = [];
root.cells(1) = [];

caught = 0;
try
    root.inner.data(1).bad = 4;
catch err
    caught = 1;
end
unchanged = root.inner.data(1);

growth_caught = 0;
try
    root.items(8).value.bad = 4;
catch growth_err
    growth_caught = 1;
end
item_count = size(root.items, 2);

summary = root.inner.value + sum(root.inner.data) + ...
          items_sum + item_tag + tag_gap_empty + ...
          created_value + created_gap_empty + ...
          root.cells{1}.value + more_cell_value + ...
          root.grid{1, 1} + root.grid{2, 1} + ...
          root.grid{1, 2} + root.grid{2, 2} + ...
          grid_column_sum + caught + unchanged + ...
          growth_caught + item_count;
