repo = fileparts(fileparts(mfilename('fullpath')));
corpus = jsondecode(fileread(fullfile(repo, 'tests', 'graphics_color_cases.json')));
parent = fullfile(repo, 'build', 'graphics-color-matlab');
if ~isfolder(parent), mkdir(parent); end
root = tempname(parent);
mkdir(root);
fig = figure('Visible', 'off');
cleanup = onCleanup(@() closeReferenceFigure(fig));
ax = axes('Parent', fig);
lineHandle = plot(ax, [1,2]);
textHandle = text(ax, 0.5, 0.5, 'Color reference');
handles = {lineHandle, textHandle};
targets = {'Line', 'Text'};
records = struct('case_id', {}, 'target', {}, 'expression', {}, 'accepted', {}, ...
    'value', {}, 'value_class', {}, 'dimensions', {}, 'preserved', {}, 'error_id', {}, 'message', {});
for caseIndex = 1:numel(corpus.cases)
    if iscell(corpus.cases), item = corpus.cases{caseIndex}; else, item = corpus.cases(caseIndex); end
    for targetIndex = 1:numel(handles)
        handle = handles{targetIndex};
        handle.Color = [0.2,0.3,0.4];
        original = handle.Color;
        record = struct('case_id', item.id, 'target', targets{targetIndex}, ...
            'expression', item.expression, 'accepted', false, 'value', [], ...
            'value_class', '', 'dimensions', [], 'preserved', false, 'error_id', '', 'message', '');
        try
            input = eval(item.expression);
            handle.Color = input;
            record.accepted = true;
        catch exception
            record.error_id = exception.identifier;
            record.message = exception.message;
        end
        record.value = handle.Color;
        record.value_class = class(record.value);
        record.dimensions = size(record.value);
        record.preserved = isequal(record.value, original);
        if isfield(item, 'accepted')
            assert(record.accepted == item.accepted, ['Acceptance mismatch: ' item.id]);
            if record.accepted
                expected = item.value;
                if isnumeric(expected), expected = reshape(expected, 1, []); end
                assert(isequal(record.value, expected), ['Readback mismatch: ' item.id]);
            else
                assert(strcmp(record.error_id, item.reference_error), ['Rejection mismatch: ' item.id]);
            end
        end
        records(end + 1) = record; %#ok<SAGROW>
        fprintf('%s/%s: accepted=%d value=%s %s\n', item.id, targets{targetIndex}, ...
            record.accepted, jsonencode(record.value), record.error_id);
    end
end
alphaEvidence = struct('target', {}, 'alpha', {}, 'minimum_green', {}, 'center_rgb', {});
for targetIndex = 1:numel(targets)
    for alpha = [0,0.5,1]
        alphaFigure = figure('Visible', 'off', 'Color', [1,1,1], 'Position', [100,100,400,300]);
        alphaCleanup = onCleanup(@() closeReferenceFigure(alphaFigure));
        alphaAxes = axes('Parent', alphaFigure, 'Position', [0,0,1,1], ...
            'XLim', [0,1], 'YLim', [0,1], 'Visible', 'off');
        if targetIndex == 1
            alphaObject = line(alphaAxes, [0,1], [0.5,0.5], 'LineWidth', 80);
        else
            alphaObject = text(alphaAxes, 0.5, 0.5, 'M', 'FontSize', 140, ...
                'HorizontalAlignment', 'center', 'VerticalAlignment', 'middle');
        end
        alphaObject.Color = [1,0,0,alpha];
        imagePath = fullfile(root, sprintf('alpha_%s_%g.png', targets{targetIndex}, alpha));
        print(alphaFigure, imagePath, '-dpng', '-r100');
        pixels = imread(imagePath);
        center = reshape(pixels(round(end/2),round(end/2),:), 1, 3);
        alphaEvidence(end + 1) = struct('target', targets{targetIndex}, 'alpha', alpha, ...
            'minimum_green', double(min(pixels(:,:,2), [], 'all')), 'center_rgb', double(center)); %#ok<SAGROW>
        clear alphaCleanup;
    end
end
report = struct('release', version('-release'), 'version', version, ...
    'corpus', corpus, 'records', records, 'accepted', sum([records.accepted]), 'alpha_evidence', alphaEvidence);
file = fopen(fullfile(root, 'reference.json'), 'w', 'n', 'UTF-8');
assert(file ~= -1, 'Cannot create reference artifact');
fileCleanup = onCleanup(@() fclose(file));
fwrite(file, jsonencode(report), 'char');
clear fileCleanup;
clear cleanup;
fprintf('Reference %s: %d accepted / %d total; evidence %s\n', ...
    report.release, report.accepted, numel(records), root);
assert(strcmp(report.release, '2024b'), 'Expected MATLAB R2024b');
assert(all([records([records.accepted] == false).preserved]), 'Invalid color changed the property');

function closeReferenceFigure(fig)
    if isgraphics(fig), close(fig); end
end
