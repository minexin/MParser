function y = bytecode_vm_demo()
A = 1:4;
B = A .* 2 + 1;
C = [1 2; 3 4];
D = C * C';
s = size(D);
y = sum(B) + B(2) + D(2, 2) + sum(D) + s(1) + s(2);
end
