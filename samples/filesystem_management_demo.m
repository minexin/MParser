root = tempname;

try
    [made, message, message_id] = mkdir(root);
    assert(made && isempty(message) && isempty(message_id));

    source_dir = fullfile(root, 'source');
    assert(mkdir(source_dir));
    source_file = fullfile(source_dir, 'value.txt');
    fid = fopen(source_file, 'w');
    assert(fid >= 3);
    assert(fprintf(fid, '%s', 'filesystem-demo') == 15);
    assert(fclose(fid) == 0);

    file_probe = [string(source_file), string(fullfile(root, 'missing.txt'))];
    assert(isequal(isfile(file_probe), [true false]));
    assert(isequal(isfolder({source_dir, source_file}), [true false]));

    [folder, name, extension] = fileparts(source_file);
    assert(strcmp(folder, source_dir));
    assert(strcmp(name, 'value'));
    assert(strcmp(extension, '.txt'));
    assert(strcmp(fileread(source_file), 'filesystem-demo'));

    copied_dir = fullfile(root, 'copied');
    assert(mkdir(copied_dir));
    assert(copyfile(source_dir, copied_dir));
    copied_file = fullfile(copied_dir, 'value.txt');
    assert(isfile(copied_file));

    moved_dir = fullfile(root, 'moved');
    assert(mkdir(moved_dir));
    assert(movefile(copied_file, moved_dir));
    moved_file = fullfile(moved_dir, 'value.txt');
    assert(isfile(moved_file) && ~isfile(copied_file));

    for index = 1:2
        log_file = fullfile(root, sprintf('event%d.log', index));
        fid = fopen(log_file, 'w');
        assert(fid >= 3);
        assert(fprintf(fid, '%d', index) == 1);
        assert(fclose(fid) == 0);
    end
    log_dir = fullfile(root, 'logs');
    assert(copyfile(fullfile(root, '*.log'), log_dir));
    log_probe = [string(fullfile(log_dir, 'event1.log')), string(fullfile(log_dir, 'event2.log'))];
    assert(all(isfile(log_probe)));

    [removed_nonempty, ignored, ignored_id] = rmdir(source_dir);
    assert(~removed_nonempty && ~isempty(ignored) && ~isempty(ignored_id));
    assert(rmdir(source_dir, 's'));

    candidate = tempname(root);
    assert(strcmp(fileparts(candidate), root));
    assert(~isfile(candidate) && ~isfolder(candidate));

    filesystem_management_summary = 91;
catch error_info
    if isfolder(root)
        rmdir(root, 's');
    end
    rethrow(error_info);
end

assert(rmdir(root, 's'));
filesystem_management_summary
