import palette.Status.Ready

ready = Ready;
busy = palette.Status('Busy');
same_ready = ready == palette.Status.Ready;
ready_name = string(ready);
busy_name = char(busy);

switch busy
    case palette.Status.Busy
        switch_code = 11;
    otherwise
        switch_code = -100;
end

[members, names] = enumeration(ready);
first_member = members(1);
first_name = names{1};
second_name = names{2};
visible_count = numel(members);
summary = ready.Code + busy.Code + first_member.Code + switch_code + ready.isReady() + same_ready + isenum(busy) + visible_count;
