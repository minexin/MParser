function y = linspace_demo()
x = linspace(0, 1, 5);
curve = x .* x + 1;
defaultGrid = linspace(1, 3);
y = sum(curve) + length(defaultGrid) + defaultGrid(100);
end
