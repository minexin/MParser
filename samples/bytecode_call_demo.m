function y = bytecode_call_demo()
A = [1 2; 3 4];
[total, rows, cols] = summarize(A);
[~, doubled] = pair(total);
scope = local_scope(2);
y = total + rows + cols + doubled + scope;
end

function [total, rows, cols] = summarize(A)
total = sum(A);
[rows, cols] = size(A);
end

function [original, doubled] = pair(x)
original = x;
doubled = x * 2;
end

function y = local_scope(x)
x = x + 1;
y = x;
end
