function y = expression_demo(A, obj)
    [a, b] = obj.Value + A(1:10, end)' .* 2;
    h = @(x) x.^2 + sin(x);
    meta = ?pkg.MyClass;
    y = h(a) + b;
end
