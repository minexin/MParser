% Reference replay of the same corpus used by the kernel's CTest.
repo = fileparts(fileparts(mfilename('fullpath')));
corpus = jsondecode(fileread(fullfile(repo, 'tests', 'contextual_identifiers.json')));
parent = fullfile(repo, 'build', 'contextual-identifier-matlab');
if ~isfolder(parent), mkdir(parent); end
root = tempname(parent);
mkdir(root);
records = struct('name', {}, 'case_id', {}, 'source', {}, ...
    'ok', {}, 'result', {}, 'error_id', {}, 'message', {});
for caseIndex = 1:numel(corpus.cases)
    if iscell(corpus.cases)
        item = corpus.cases{caseIndex};
    else
        item = corpus.cases(caseIndex);
    end
    for nameIndex = 1:numel(corpus.names)
        name = corpus.names{nameIndex};
        caseId = [item.id '_' name];
        directory = fullfile(root, caseId);
        mkdir(directory);
        className = ['Contextual_' caseId];
        source = strrep(strrep(item.source, '@NAME@', name), '@CLASS@', className);
        sourcePath = fullfile(directory, [caseId '.m']);
        writeSource(sourcePath, source);
        if isfield(item, 'class_source') && ~isempty(item.class_source)
            classSource = strrep(strrep(item.class_source, '@NAME@', name), '@CLASS@', className);
            writeSource(fullfile(directory, [className '.m']), classSource);
        end
        record = struct('name', name, 'case_id', item.id, 'source', source, ...
            'ok', false, 'result', [], 'error_id', '', 'message', '');
        expectedError = '';
        if isfield(item, 'expected_rejections') && isfield(item.expected_rejections, name)
            expectedError = item.expected_rejections.(name);
        end
        try
            record.result = runCase(sourcePath);
            record.ok = isempty(expectedError) && isequal(record.result, item.expected);
        catch exception
            record.error_id = exception.identifier;
            record.message = exception.message;
            record.ok = strcmp(record.error_id, expectedError);
        end
        records(end + 1) = record; %#ok<SAGROW>
        fprintf('%s/%s: %d %s\n', item.id, name, record.ok, record.error_id);
    end
end
report = struct('release', version('-release'), 'version', version, ...
    'corpus', corpus, 'records', records, 'passed', sum([records.ok]));
writeSource(fullfile(root, 'reference.json'), jsonencode(report));
fprintf('Reference %s: %d/%d passed; evidence %s\n', report.release, report.passed, numel(records), root);
assert(strcmp(report.release, '2024b'), 'Expected MATLAB R2024b');
assert(all([records.ok]), 'Reference corpus has failures; inspect reference.json before changing the kernel');

function result = runCase(sourcePath)
    result = NaN;
    run(sourcePath);
end

function writeSource(path, source)
    file = fopen(path, 'w', 'n', 'UTF-8');
    assert(file ~= -1, 'Cannot create reference artifact');
    cleanup = onCleanup(@() fclose(file));
    fwrite(file, source, 'char');
end
