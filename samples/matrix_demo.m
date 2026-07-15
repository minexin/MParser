function y = matrix_demo()
A = [1 2; 3 4];
B = A * A';
s = size(B);
y = B(2, 2) + sum(B, "all") + s(1) + s(2);
end
