clear;

x = linspace(-3, 3, 12000);
wave = sin(x) .* cos(x) + sqrt(abs(x) + 1);
energy = sum(wave .* wave, "all");

grid = reshape(1:24, [4 6]);
weights = linspace(0.5, 1.5, 6);
weighted = grid .* weights + 2;
column_totals = sum(weighted, 1);

dense_jit_checksum = energy + ...
                     sum(column_totals, "all") * 0.001;
clear x wave energy grid weights weighted column_totals
dense_jit_checksum
