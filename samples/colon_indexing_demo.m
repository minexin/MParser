function y = colon_indexing_demo()
A = [1 2 3; 4 5 6; 7 8 9];
col = A(:, 2);
row = A(1, :);
block = A(2:3, [1 3]);
linear = A(:);

v = [1 2 3 4];
v(:) = 7;
A(:, 3) = 10;

y = sum(col, "all") + sum(row, "all") + sum(block, "all") + ...
    sum(linear, "all") + sum(v, "all") + sum(A(:, 3), "all");
end
