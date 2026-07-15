A = [1 2 3 4; 5 6 7 8; 9 10 11 12];
mask = A > 5;
picked = A(mask);
A(mask) = 100;

rows = logical([1 0 1]);
columns = logical([0 1 1 0]);
block = A(rows, columns);

flags = logical([0 2 0 3]);
flags(logical([1 0 1 0])) = [5 2];
reshaped = reshape(mask, 4, 3);

summary = sum(picked) + sum(A) + sum(block) + sum(flags) + ...
    sum(double(reshaped)) + islogical(mask) + isa(mask, "logical");
