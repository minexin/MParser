x = 1:20;
y = zeros(1, 20);
checksum = 0;

for i = 1:20
    y(i) = sin(x(i)) + 2 * x(i);
    checksum = checksum + y(i);
end

first = y(1)
last = y(20)
checksum
