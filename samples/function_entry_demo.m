function y = function_entry_demo()
y = 999;
end

function [s, p] = kernel(x, n)
s = 0;
for i = 1:n
    s = s + x * i;
end
p = x * n;
end
