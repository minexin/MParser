function y = vector_demo()
A = 1:4;
B = A .* 2 + 1;
y = sum(B) + B(2);
end
