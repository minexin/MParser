A = reshape(1:12, 3, 4);
A([1 3], :) = [];

B = reshape(1:12, 3, 4);
B(:, logical([0 1 0 1])) = [];

C = reshape(1:24, 2, 3, 4);
C(:, :, [1 3]) = [];

row = [1 2 3 4 5];
row([2 4]) = [];

L = logical([1 0 1 0]);
L([2 3]) = [];

whole = [1 2; 3 4];
whole(:) = [];

summary = sum(A(:)) + sum(B(:)) + sum(C(:)) + ...
          sum(row) + sum(L) + numel(whole)
