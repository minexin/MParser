x = 1:2000;
y = zeros(1, 2000);
baseline_result = 0;

for i = 1:2000
    y(i) = sin(x(i)) + 2 * x(i);
    baseline_result = baseline_result + y(i);
end

baseline_result = baseline_result + y(1) + y(2000);
