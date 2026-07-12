function y = bytecode_profile_demo()
y = 0;
for i = 1:12
    y = y + kernel(i);
end

for j = 1:3
    y = y + j;
end
end

function z = kernel(x)
z = x * x + 1;
end
