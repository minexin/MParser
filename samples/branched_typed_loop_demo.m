clear;

score = 0;
high = 0;
middle = 0;
for j = 1:20
    for i = -10:10
        value = i * j;
        if value > 50
            score = score + value;
            high = high + 1;
        elseif value < -50
            score = score - value;
            high = high + 1;
        else
            score = score + abs(value);
            middle = middle + 1;
        end
    end
end

summary = score + high + middle
