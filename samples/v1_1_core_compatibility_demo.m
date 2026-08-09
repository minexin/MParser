power = 2^3^2;
dotPower = 2.^3.^2;

if power == 64, branch = 10; else, branch = -1; end
switch branch, case 10, switched = 20; otherwise, switched = -1; end

A = [1 2 3; 4 5 6];
sumFirst = 0;
for col = A, sumFirst = sumFirst + col(1); end

count = 0;
while count < 3, count = count + 1; end

linear = A(:);
v = [7 8 9];
vlinear = v(:);
summary = power + dotPower + branch + switched + sumFirst + count + ...
    sum(linear, "all")
