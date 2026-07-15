A = zeros(2, 3, 2);
for k = 1:numel(A)
    A(k) = k;
end

shape = size(A);
[rows, tail] = size(A);
dimensions = ndims(A);
count = numel(A);

same = A(2, 2, 2) + A(2, 5) + A(10);
third_end = A(1, end, 2);
folded_end = A(2, end);
slice = A(:, 2:3, :);
slice_check = slice(2, 1, 2);

C = cell(2, 3, 2);
C{2, 2, 2} = 42;
C{2, end, end} = 23;
cell_value = C{2, 5};
cell_last = C{end};

M = zeros(2, 3, 2);
M(:, 2:3, 2) = 5;
filled_total = sum(M, "all");

summary = same + folded_end + third_end + rows + tail + dimensions + ...
    count + cell_value + cell_last + slice_check + filled_total;
