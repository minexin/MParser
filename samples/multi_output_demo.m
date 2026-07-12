function y = multi_output_demo()
A = [1 2; 3 4];
[total, rows, cols] = summarize(A);
[~, doubled] = pair(total);
firstOnly = pair(5);
y = total + rows + cols + doubled + firstOnly;
end

function [total, rows, cols] = summarize(A)
total = sum(A);
[rows, cols] = size(A);
end

function [original, doubled] = pair(x)
original = x;
doubled = x * 2;
end
