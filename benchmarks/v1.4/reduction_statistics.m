data = reshape(1:12000, [120 100]);
column_mean = mean(data, 1);
column_std = std(data, 0, 1);
column_median = median(data, 1);
column_energy = sum(data .* data, 1);
baseline_result = sum(column_mean, "all") + ...
                  sum(column_std, "all") + ...
                  sum(column_median, "all") + ...
                  sum(column_energy, "all") * 0.000001;
clear data column_mean column_std column_median column_energy
baseline_result
