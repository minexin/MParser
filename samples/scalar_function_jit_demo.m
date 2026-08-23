summary = 0;
for index = 1:1000
    summary = summary + local_kernel(index);
end
clear index
summary

function value = local_kernel(input)
shifted = input + 1;
value = shifted * shifted;
end
