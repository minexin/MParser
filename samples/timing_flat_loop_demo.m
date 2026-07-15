clear;
clc;
tic;
for i = 1:1000000
    k = abs(i) + 9;
    m = sin(i);
    n = k * m;
end
a = toc
