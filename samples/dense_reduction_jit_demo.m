clear;

matrix = [1 2 3; 4 5 6];
product_columns = prod(matrix);
product_rows = prod(matrix, 2);
product_all = prod(matrix, "all");
mean_columns = mean(matrix);
mean_rows = mean(matrix, 2);
mean_all = mean(matrix, "all");

empty = zeros(0, 2);
empty_product = prod(empty, "all");
empty_mean = mean(empty, "all");

reduction_jit_checksum = product_all + mean_all + ...
    sum(product_columns, "all") + sum(product_rows, "all") + ...
    sum(mean_columns, "all") + sum(mean_rows, "all") + ...
    empty_product + isnan(empty_mean);
reduction_jit_checksum
