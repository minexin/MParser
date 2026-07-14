B = zeros(2, 2);
B(3:4, 3:4) = [1 2; 3 4];
B(1, 4) = 8;

v = [1 2];
v([4 6]) = [40 60];

L = [1 2; 3 4];
L(5) = 9;

c = [1; 2];
c(4) = 4;

R = zeros(1, 2);
ids = [1 2; 2 1];
R(ids) = [10 20; 30 40];

N = zeros(2, 2, 2);
N(:, 1, 2) = [5; 6];
N(3, 2, 3) = 7;

E = [1; 2] + [10 20 30];
Z = ones(2, 1, 2) + [10 20 30];

s = 1;
s(3) = 5;

bshape = size(B);
lshape = size(L);
cshape = size(c);
nshape = size(N);
eshape = size(E);
zshape = size(Z);

shape_score = sum(bshape) + sum(lshape) + sum(cshape) + sum(nshape) + ...
    sum(eshape) + sum(zshape) + length(v);
value_score = sum(B) + sum(v) + sum(L) + sum(c) + sum(R) + sum(N) + ...
    sum(E) + sum(Z) + sum(s);
checks = B(2, 2) + B(3, 3) + B(4, 4) + v(4) + L(1, 3) + ...
    c(4) + R(1) + R(2) + N(1, 1, 2) + N(2, 1, 2) + N(3, 2, 3) + ...
    E(2, 3) + Z(2, 3, 2) + s(3);

summary = shape_score + value_score + checks;
