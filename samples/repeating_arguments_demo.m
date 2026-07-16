[count, firstX, lastY, observedNargin] = summarize(10, 1, 2, 3, 4);
summary = count * 1000 + firstX * 100 + lastY * 10 + observedNargin;

function [count, firstX, lastY, observedNargin] = summarize(seed, x, y)
arguments
    seed (1,1) double {mustBePositive}
end
arguments (Repeating)
    x (1,1) double {mustBePositive}
    y (1,1) double {mustBeInteger}
end
count = numel(x);
firstX = seed;
lastY = seed;
if count > 0
    firstX = x{1};
    lastY = y{count};
end
observedNargin = nargin;
end
