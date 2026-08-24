single_values = single([1 2 3 4]);
single_scaled = single_values .* single(2) + single(1);
single_sum = sum(single_scaled, "all");

complex_values = [1 + 2i, 3 - 4i];
complex_out = complex_values .* (2 - i) + 1i;
complex_sum = sum(complex_out, "all");

real_domain = sqrt([1 4 9]);
complex_domain = sqrt([-1 4]);

summary = abs(single_sum - single(24)) + ...
          abs(complex_sum - (6 - 6i)) + ...
          abs(sum(real_domain, "all") - 6) + ...
          abs(sum(complex_domain, "all") - (2 + 1i));
summary = summary < 1e-5;
