function y = compiled_module_demo()
y = -1;
end

function [total, product] = accumulate_scale(x, n)
total = 0;
for i = 1:n
    total = total + x * i;
end
product = x * n;
end
