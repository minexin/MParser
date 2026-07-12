function y = loop_control_demo()
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

j = 0;
while j < 5
    j = j + 1;
    if j == 3
        continue
    end
    if j == 5
        break
    end
    y = y + 10 * j;
end
end
