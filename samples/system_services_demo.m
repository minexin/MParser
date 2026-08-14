root = pwd;
temporary = tempdir;
directory_ok = ischar(root) && ~isempty(root) && ...
               ischar(temporary) && ~isempty(temporary);

old = cd(temporary);
change_ok = ~strcmp(pwd, old);
cd(old);
change_ok = change_ok && strcmp(pwd, root);

initial_path = path;
addpath(temporary, '-end');
path_after_add = path;
rmpath(temporary);
path_ok = ischar(initial_path) && ischar(path_after_add) && ischar(path);

listing = dir(root);
listing_ok = isstruct(listing) && numel(listing) >= 1;

environment_value = getenv('PATH');
environment_ok = ischar(environment_value);

sin_location = which('sin');
which_ok = ischar(sin_location) && ~isempty(sin_location);

today = date;
now_parts = clock;
calendar_ok = ischar(today) && ~isempty(today) && numel(now_parts) == 6;

platform = computer;
runtime_version = version;
identity_ok = ischar(platform) && ~isempty(platform) && ...
              ischar(runtime_version) && ~isempty(runtime_version);

pause(0);
pause('off');
pause(1);
pause_ok = strcmp(pause('query'), 'off');
pause('on');

[command_status, command_output] = system('echo mparser-system-services');
process_ok = command_status == 0 && ischar(command_output) && ...
             ~isempty(command_output);

probe = 42;
workspace_names = who('probe');
workspace_details = whos('probe');
who_ok = numel(workspace_names) == 1 && ...
         strcmp(workspace_names{1}, 'probe');
whos_ok = isstruct(workspace_details) && ...
          strcmp(workspace_details.name, 'probe') && ...
          strcmp(workspace_details.class, 'double');

summary = sum([directory_ok change_ok path_ok listing_ok ...
               environment_ok which_ok calendar_ok identity_ok ...
               pause_ok process_ok who_ok whos_ok])
