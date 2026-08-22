clear;

folder = tempname();
[created, message] = mkdir(folder);
assert(created && isempty(message));
path = fullfile(folder, 'roundtrip');

matrix = reshape(1:8, [2 2 2]);
z = single([1 + 2i 3 - 4i]);
flags = logical([0 1; 1 0]);
characters = char([65 20013; 66 67]);
values = {1, 'two'; true, single(4)};
record = struct('label', 'answer', 'value', 42);

save(path, 'matrix', 'z', 'flags', 'characters', 'values', 'record');
clear matrix z flags characters values record;

snapshot = load(path);
assert(exist('matrix', 'var') == 0);
assert(snapshot.matrix(8) == 8);
assert(isequal(snapshot.z, single([1 + 2i 3 - 4i])));
assert(isequal(snapshot.flags, logical([0 1; 1 0])));
assert(strcmp(snapshot.characters, char([65 20013; 66 67])));
assert(strcmp(snapshot.values{1, 2}, 'two'));
assert(snapshot.record.value == 42);

load(path, 'matrix', 'z', 'flags', 'characters', 'values', 'record');
assert(matrix(8) == 8 && real(z(1)) == 1 && imag(z(1)) == 2);

plain = 17;
save(fullfile(folder, 'plain'), 'plain', '-nocompression');
clear plain;
load(fullfile(folder, 'plain'));
assert(plain == 17);

[removed, message] = rmdir(folder, 's');
assert(removed && isempty(message));
mat_file_summary = double(matrix(8)) + double(real(z(1))) + ...
    plain + record.value + removed
