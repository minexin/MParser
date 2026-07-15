clear;

total = 0;
for j = 1:12
    for i = 1:2:5
        total = total + j * i;
    end
end

descendingTotal = 0;
for k = 5:-2:1
    descendingTotal = descendingTotal + k;
end

emptyCount = 0;
for unused = 5:1
    emptyCount = emptyCount + 1;
end

summary = total + descendingTotal + emptyCount
