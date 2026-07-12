function y = indexed_assignment_demo()
A = zeros(2, 3);
for r = 1:2
    for c = 1:3
        A(r, c) = r * 10 + c;
    end
end

A(4) = 99;

v = zeros(1, 4);
for i = 1:4
    v(i) = i * i;
end
v([1 3]) = 7;

y = A(2, 2) + A(4) + sum(v);
end
