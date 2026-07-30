offset = 4;
values = [1 2 3];
values(2) = 5;

total = 0;
for i = 1:3
    if i == 2
        total = total + values(i);
    else
        total = total + i;
    end
end

switch total
    case 9
        total = total + 10;
    otherwise
        total = -1;
end

try
    selected = values(3);
catch err
    selected = 0;
end

state.metrics.total = total;
state.metrics.selected = selected;
adjust = @(value) value + offset;
summary = adjust(state.metrics.total) + state.metrics.selected
