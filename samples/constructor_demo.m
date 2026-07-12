function y = constructor_demo()
Z = zeros(2, 3);
O = ones(1, 4);
I = eye(3);
R = eye(2, 3);
[rows, cols] = size(R);
y = sum(Z) + sum(O) + sum(I) + sum(R) + rows + cols;
end
