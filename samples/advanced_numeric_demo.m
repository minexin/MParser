data = [1 2 3; 4 5 6];
assert(isequal(median(data), [2.5 3.5 4.5]));
assert(isequal(median(data, 2), [2; 5]));
assert(abs(std([1 2 3 4]) - 1.2909944487358056) < 1e-10);
assert(abs(var([1+2i 3+4i]) - 4) < 1e-12);

cube = reshape(1:24, [2 3 4]);
cube_median = median(cube, [1 2]);
assert(isequal(size(cube_median), [1 1 4]));
assert(isequal(squeeze(cube_median), [3.5; 9.5; 15.5; 21.5]));

A = [1 2; 3 4];
assert(abs(det(A) + 2) < 1e-12);
assert(norm(inv(A) - [-2 1; 1.5 -0.5], 'fro') < 1e-12);
assert(isequal([2 0; 0 4] \ [2; 8], [1; 2]));
assert(norm([1 2] / [1 0; 0 2] - [1 1], 'fro') < 1e-12);
assert(trace(A) == 5 && rank(A) == 2);

[vectors, values] = eig([0 -1; 1 0]);
assert(norm([0 -1; 1 0] * vectors - vectors * values, 'fro') < 1e-10);
assert(dot([1+2i 3+4i], [1+2i 3+4i]) == 30);
assert(isequal(cross([1 0 0], [0 1 0]), [0 0 1]));

signal = single([1 2 3 4]);
spectrum = fft(signal);
assert(isa(spectrum, 'single') && abs(spectrum(1) - 10) < 1e-5);
assert(norm(double(ifft(spectrum)) - double(signal)) < 1e-5);
assert(isequal(conv([1 2], [3 4]), [3 10 8]));
assert(isequal(conv([1 2], [3 4], 'same'), [10 8]));
assert(abs(trapz([0 1 2], [0 1 4]) - 3) < 1e-12);

coefficients = polyfit([1 2 3 4], [1 4 9 16], 2);
assert(norm(coefficients - [1 0 0]) < 1e-10);
assert(norm(polyval(coefficients, [5 6]) - [25 36]) < 1e-9);
[scaled_coefficients, fit_info, mu] = ...
    polyfit([1 2 3 4], [1 4 9 16], 2);
assert(isfield(fit_info, 'R') && fit_info.df == 1);
normalized = ([1 2 3 4] - mu(1)) / mu(2);
assert(norm(polyval(scaled_coefficients, normalized) - [1 4 9 16]) < 1e-9);

advanced_numeric_summary = 174
