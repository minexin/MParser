if runCount == 0
    timings = zeros(1, 8);
end

runCount = runCount + 1;
tic;
for j = 1:1000
    for i = 1:1000
        k = abs(i) + 9;
        m = sin(i);
        n = k * m;
    end
end
timings(runCount) = toc;
finalValue = n
