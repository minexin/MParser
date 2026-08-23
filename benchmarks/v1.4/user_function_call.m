baseline_result = 0;
for index = 1:5000
    baseline_result = baseline_result + local_kernel(index);
end
clear index
baseline_result

function value = local_kernel(input)
value = sin(input) + sqrt(input + 1) / (input + 2);
end
