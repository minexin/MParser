baseline_result = 0;

for j = 1:100
    for i = 1:1000
        k = abs(i) + 9;
        m = sin(i);
        baseline_result = k * m + j * 0.000001;
    end
end
