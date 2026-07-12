phase = phase + 1;
if phase <= 2
    y = 0;
elseif phase <= 4
    y = [0 0];
else
    y = 0;
end
for i = 1:6
    y = y + i * i;
end
