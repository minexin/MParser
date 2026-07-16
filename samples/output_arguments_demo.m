[flag,row] = checkedOutputs(2,[1;2;3]);
[first,second] = repeatedOutputs(4);
[head,tail1,tail2] = mixedOutputs(5);
summary = flag + sum(row) + first + second + head + tail1 + tail2

function [flag,row] = checkedOutputs(value,column)
arguments (Output)
    flag (1,1) logical
    row (1,:) double {mustBePositive}
end
flag = value;
row = column;
end

function values = repeatedOutputs(seed)
arguments (Output,Repeating)
    values (1,1) double {mustBePositive}
end
values{1} = seed;
values{2} = seed + 1;
end

function [head,tail] = mixedOutputs(seed)
arguments (Output)
    head (1,1) double
end
arguments (Output,Repeating)
    tail (1,1) double {mustBePositive}
end
head = seed;
tail{1} = seed + 1;
tail{2} = seed + 2;
end
