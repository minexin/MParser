function mat_v5_interop_probe(output_directory)
% Generate MATLAB MAT v5 fixtures and verify equivalent MParser files.

if ~isfolder(output_directory)
    mkdir(output_directory);
end

expected.double_nd = reshape(1:8, [2 2 2]);
expected.logical_value = logical([0 1 1; 1 0 0]);
expected.single_complex = reshape(single( ...
    (0:3) + 0.5 - 1i * ((0:3) + 0.25)), [2 2]);
expected.int8_value = int8([-128 -1 0 127]);
expected.uint8_value = uint8([0 127 255]);
expected.int16_value = int16([-32768 0 32767]);
expected.uint16_value = uint16([0 32768 65535]);
expected.int32_value = int32([-2147483648 0 2147483647]);
expected.uint32_value = uint32([0 2147483648 4294967295]);
expected.int64_value = [intmin('int64') int64(0) intmax('int64')];
expected.uint64_value = [uint64(0) ...
    uint64(9007199254740992) + uint64(1) intmax('uint64')];
expected.characters = char([65 20013 66; 55357 56898 67]);

expected.cell_value = cell(2, 2);
expected.cell_value{1, 1} = 1;
expected.cell_value{1, 2} = 'right';
expected.cell_value{2, 1} = true;
expected.cell_value{2, 2} = zeros(0, 0);

expected.structure = repmat(struct('label', '', 'number', 0), 2, 2);
expected.structure(1, 1) = struct('label', 'item-0', 'number', 10);
expected.structure(1, 2) = struct('label', 'item-1', 'number', 11);
expected.structure(2, 1) = struct('label', 'item-2', 'number', 12);
expected.structure(2, 2) = struct('label', 'item-3', 'number', 13);

save(fullfile(output_directory, 'matlab-uncompressed.mat'), ...
    '-struct', 'expected', '-v7', '-nocompression');
save(fullfile(output_directory, 'matlab-compressed.mat'), ...
    '-struct', 'expected');

verify_file(fullfile(output_directory, 'mparser-uncompressed.mat'), expected);
verify_file(fullfile(output_directory, 'mparser-compressed.mat'), expected);
disp('MAT v5 MATLAB interoperability probe passed');
end

function verify_file(path, expected)
actual = load(path);
names = fieldnames(expected);
assert(isequal(fieldnames(actual), names));
for index = 1:numel(names)
    name = names{index};
    assert(isequaln(actual.(name), expected.(name)), ...
        ['MAT value mismatch: ' name]);
end
end
