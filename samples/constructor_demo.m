function y = constructor_demo()
Z = zeros(2, 3);
O = ones(1, 4);
I = eye(3);
R = eye(2, 3);
[rows, cols] = size(R);
y = sum(Z, "all") + sum(O, "all") + sum(I, "all") + ...
    sum(R, "all") + rows + cols;
end
