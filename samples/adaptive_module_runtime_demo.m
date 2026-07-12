function y = adaptive_module_runtime_demo()
y = -1;
end

function y = hot_a(seed)
y = 0;
for i = 1:6
    y = y + seed * i;
end
end

function y = hot_b(seed)
y = 1;
for j = 1:6
    y = y + seed * j;
end
end
