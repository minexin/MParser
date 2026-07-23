global shared_value loop_last
shared_value = 10;

for i = 1:12
    loop_last = i;
end

first = workspace_counter(1);
second = workspace_counter(2);
workspace_set_shared(7);
third = shared_value;
fourth = workspace_get_shared();

summary = first * 1000 + second * 100 + third * 10 + fourth + loop_last

function out = workspace_counter(step)
persistent count values
if isempty(count)
    count = 0;
    values = zeros(1, 2);
end
count = count + step;
values(1) = values(1) + step;
out = count * 10 + values(1);
end

function workspace_set_shared(value)
global shared_value
shared_value = value;
end

function out = workspace_get_shared()
global shared_value
out = shared_value;
end
