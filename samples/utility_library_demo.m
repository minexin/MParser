factorials = factorial([0 1 5]);
single_factorial = factorial(single(5));
saturated_factorial = factorial(uint8(6));
assert(isequal(factorials, [1 1 120]));
assert(isa(single_factorial, 'single') && single_factorial == single(120));
assert(isa(saturated_factorial, 'uint8') && ...
       saturated_factorial == uint8(255));

common = gcd([12 15], [18 25]);
multiples = lcm([4 6], [6 15]);
mixed_common = gcd(int16([12 15]), 5);
exact_common = gcd(uint64(18), uint64(2));
assert(isequal(common, [6 5]));
assert(isequal(multiples, [12 30]));
assert(isa(mixed_common, 'int16') && ...
       isequal(mixed_common, int16([1 5])));
assert(isa(exact_common, 'uint64') && exact_common == uint64(2));

prime_values = primes(uint16(30));
prime_mask = isprime(uint64([0 1 2 17 18 97]));
assert(isa(prime_values, 'uint16'));
assert(isequal(prime_values, uint16([2 3 5 7 11 13 17 19 23 29])));
assert(isequal(prime_mask, logical([0 0 1 1 0 1])));

decades = logspace(0, 3, 4);
pi_space = logspace(single(0), pi, 3);
empty_space = logspace(0, 1, 0);
assert(isequal(decades, [1 10 100 1000]));
assert(isa(pi_space, 'single') && abs(double(pi_space(3)) - pi) < 1e-6);
assert(isequal(size(empty_space), [1 0]));

[grid_x, grid_y, grid_z] = meshgrid(single([1 2]), int16([3 4]), [5 6]);
assert(isequal(size(grid_x), [2 2 2]));
assert(isa(grid_x, 'single') && isa(grid_y, 'int16'));
assert(grid_x(2, 2, 2) == single(2));
assert(grid_y(2, 2, 2) == int16(4));
assert(grid_z(2, 2, 2) == 6);

cube = reshape(1:8, 2, 2, 2);
flipped_cube = flip(cube, 3);
flipped_rows = flipud([1 2; 3 4]);
flipped_words = fliplr(["left", "middle", "right"]);
flipped_missing = flip(repmat(missing, 2, 3), 2);
assert(isequal(flipped_cube, reshape([5 6 7 8 1 2 3 4], 2, 2, 2)));
assert(isequal(flipped_rows, [3 4; 1 2]));
assert(all(strcmp(flipped_words, ["right", "middle", "left"]), "all"));
assert(isequal(size(flipped_missing), [2 3]) && ...
       all(ismissing(flipped_missing), "all"));

overlap_positions = strfind('banana', 'ana');
overlap_replacement = strrep('banana', 'ana', 'X');
array_positions = strfind(["alpha", "banana"], 'a');
cell_replacement = strrep({'alpha', "banana"}, 'a', 'A');
assert(isequal(overlap_positions, [2 4]));
assert(strcmp(overlap_replacement, 'bXX'));
assert(iscell(array_positions) && isequal(array_positions{2}, [2 4 6]));
assert(strcmp(cell_replacement{1}, 'AlphA'));
assert(strcmp(cell_replacement{2}, "bAnAnA"));

rng(42);
selection = randperm(1000000000, 8);
rng(42);
selection_replay = randperm(1000000000, 8);
empty_selection = randperm(0);
assert(isequal(selection, selection_replay));
assert(numel(selection) == 8 && numel(unique(selection)) == 8);
assert(all(selection >= 1 & selection <= 1000000000, "all"));
assert(isequal(size(empty_selection), [1 0]));

summary = 13
