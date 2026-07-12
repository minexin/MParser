function y = bytecode_control_demo()
y = 0;
for i = 1:6
    if i == 2
        continue
    end
    if i > 4
        break
    end
    y = y + i;
end

j = 1;
while j <= 3
    y = y + 10 * j;
    j = j + 1;
end
end
