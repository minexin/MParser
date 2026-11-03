% Run with MATLAB R2024b to reproduce the host variable-name baseline.
names = {'valid_name', 'end', '_x', 'arguments', 'properties', ...
    'methods', 'events', 'enumeration', 'import', ...
    repmat('a', 1, 63), repmat('a', 1, 64), char(233)};
report = struct('release', version('-release'), ...
    'maxLength', namelengthmax, 'keywords', {iskeyword}, ...
    'names', {names}, 'valid', cellfun(@isvarname, names));
disp(jsonencode(report));
inputs = {1, [], {'abc'}, ['ab';'cd'], string({'a','b'}), string(missing)};
for index = 1:numel(inputs)
    for functionName = {'isvarname', 'iskeyword'}
        try
            result = feval(functionName{1}, inputs{index});
            fprintf('%s %d = %s\n', functionName{1}, index, jsonencode(result));
        catch exception
            fprintf('%s %d error %s\n', functionName{1}, index, exception.identifier);
        end
    end
end
