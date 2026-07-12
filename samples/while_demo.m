function y = while_demo()
i = 1;
y = 0;
values = zeros(1, 5);

while i <= 5
    values(i) = i * i;
    y = y + values(i);
    i = i + 1;
end
end
