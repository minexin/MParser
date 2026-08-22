root = tempname();
[made, message, message_id] = mkdir(root);
assert(made && isempty(message_id));

first = fullfile(root, 'alpha.txt');
second = fullfile(root, 'beta.txt');
doomed = fullfile(root, 'delete-me.tmp');

fid = fopen(first, 'w');
assert(fid >= 3);
assert(fprintf(fid, '%s', 'alpha') == 5);
assert(fclose(fid) == 0);

fid = fopen(second, 'w');
assert(fid >= 3);
assert(fprintf(fid, '%s', 'beta') == 4);
assert(fclose(fid) == 0);

fid = fopen(doomed, 'w');
assert(fid >= 3);
assert(fprintf(fid, '%s', 'doomed') == 6);
assert(fclose(fid) == 0);

[query_all, all_attributes] = fileattrib(fullfile(root, '*.txt'));
assert(query_all && numel(all_attributes) == 2);
assert(isfield(all_attributes, 'UserWrite'));

[set_readonly, message, message_id] = fileattrib(first, '-w');
assert(set_readonly && isempty(message) && isempty(message_id));
[query_one, first_attributes] = fileattrib(first);
writable_disabled = first_attributes.UserWrite == 0;
assert(query_one && writable_disabled);
[set_writable, message, message_id] = fileattrib(first, '+w');
assert(set_writable && isempty(message) && isempty(message_id));

entries = dir(fullfile(root, '*.txt'));
assert(numel(entries) == 2 && entries(1).datenum > 0);
delete(fullfile(root, 'delete-*.tmp'));
deleted = ~isfile(doomed);
assert(deleted);

[removed, message, message_id] = rmdir(root, 's');
assert(removed && isempty(message) && isempty(message_id));

file_metadata_summary = query_all + numel(all_attributes) + ...
    set_readonly + writable_disabled + set_writable + ...
    numel(entries) + deleted + removed
