function y = end_indexing_demo()
v = [10 20 30 40 50];
A = [1 2 3; 4 5 6];

y = v(end) + v(end - 1) + sum(v(2:end - 1)) + sum(v([1 end]));
y = y + A(end, 2) + A(1, end) + A(end);

v(end) = 99;
A(end, end) = 42;
y = y + v(end) + A(end, end);
end
