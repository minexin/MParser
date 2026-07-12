function y = local_function_demo()
A = [1 2; 3 4];
y = pick(A, 2) + scale(3);
end

function y = pick(A, row)
y = A(row, 1) + A(row, 2);
end

function y = scale(x)
y = x * 2;
end
