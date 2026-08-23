x = linspace(-4, 4, 12000);
a = sin(x) .* cos(x);
b = sqrt(abs(x) + 1);
c = (a + b) ./ (1 + x .* x);
d = exp(c .* 0.02) + log(abs(c) + 2);
baseline_result = sum(d .* d + c, "all");
clear x a b c d
baseline_result
