clear;
clc;
tic;
for j = 1:1000
    for i = 1:1000
        k = abs(i) + 9;
        m = sin(i);
        n = k * m;
    end
end
a = toc
