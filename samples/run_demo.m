function y = run_demo()
y = 0;
for i = 1:3
    if sin(pi / 2) > 0
        y = y + i;
    else
        y = y - i;
    end
end
end
