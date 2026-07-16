function y = native_cache_demo()
y = -1;
end

function y = cache_add(seed)
y = 0;
for i = 1:8
    y = y + seed + i;
end
end

function y = cache_multiply(seed)
y = 0;
for i = 1:8
    y = y + seed * i;
end
end

function y = cache_absolute(seed)
y = 0;
for i = 1:8
    y = y + abs(seed - i);
end
end
