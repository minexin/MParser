function y = bytecode_indexing_demo()
A = [1 2 3; 4 5 6; 7 8 9];
col = A(:, 2);
linear = A(:);

A(:, 3) = 10;
A(end, end) = 42;

v = [1 2 3 4];
v(:) = 7;
v(end) = 11;

y = sum(col) + sum(linear) + sum(A(:, 3)) + sum(v) + A(end, end);
end
