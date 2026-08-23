A = reshape(mod(1:144, 19), [12 12]) / 100 + eye(12) * 4;
b = (1:12)';
x = A \ b;
inverse_error = norm(A * inv(A) - eye(12), "fro");
baseline_result = sum(x, "all") + det(A) * 0.000001 + inverse_error;
clear A b x inverse_error
baseline_result
