function y = bytecode_indexing_demo()
A = [1 2 3; 4 5 6; 7 8 9];
col = A(:, 2);
linear = A(:);

A(:, 3) = 10;
A(end, end) = 42;

v = [1 2 3 4];
v(:) = 7;
v(end) = 11;

y = sum(col, "all") + sum(linear, "all") + ...
    sum(A(:, 3), "all") + sum(v, "all") + A(end, end);
end
