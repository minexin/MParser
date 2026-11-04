function run_colon_reference(outputPath, expectedPath)
cases = jsondecode(fileread(fullfile(fileparts(mfilename('fullpath')), 'cases.json')));
records = cell(1, numel(cases));
for index = 1:numel(cases)
    item = cases(index);
    record.id = item.id;
    record.expression = item.expression;
    try
        value = eval(item.expression);
        record.succeeded = true;
        record.class = class(value);
        record.dimensions = size(value);
        if isempty(value)
            record.hex = {};
        else
            record.hex = cellstr(num2hex(value(:)));
        end
        record.error = '';
    catch exception
        record.succeeded = false;
        record.class = '';
        record.dimensions = [];
        record.hex = {};
        record.error = exception.identifier;
    end
    records{index} = record;
end
report.matlab_version = version;
report.release = version('-release');
report.cases = [records{:}];
if nargin >= 2
    expected = jsondecode(fileread(expectedPath));
    actual = jsondecode(jsonencode(report));
    assert(isequaln(actual, expected), 'R2024b colon reference changed');
end
file = fopen(outputPath, 'w');
assert(file >= 0);
closer = onCleanup(@()fclose(file));
fprintf(file, '%s\n', jsonencode(report));
fprintf('Recorded %d colon cases on MATLAB %s\n', numel(cases), version);
end
