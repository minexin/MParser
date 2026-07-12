function y = runtime_benchmark_demo()
y = 0;
for i = 1:1000
    y = y + kernel(i);
end
end

function z = kernel(x)
z = x * x + 1;
end
