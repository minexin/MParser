clear;

rt = datetime(2024, 1, [2; 1; 2]);
tt = timetable(rt, [3; 1; 2], 'VariableNames', {'X'});
assert(istimetable(tt));
assert(~istable(tt));
assert(height(tt) == 3);
assert(width(tt) == 1);
assert(day(tt.Time) == [2; 1; 2]);
assert(day(tt.Properties.RowTimes) == [2; 1; 2]);

selected = tt(datetime(2024, 1, 2), :);
assert(selected.X == [3; 2]);

[sorted, order] = sortrows(tt, 'X');
assert(order == [2; 3; 1]);
assert(day(sorted.Time) == [1; 2; 2]);
[byTime, timeOrder] = sortrows(tt);
assert(day(byTime.Time) == [1; 2; 2]);
assert(timeOrder == [2; 1; 3]);
[reverseTime, reverseOrder] = sortrows(tt, 'Time', 'descend');
assert(day(reverseTime.Time) == [2; 2; 1]);
assert(reverseOrder == [1; 3; 2]);

asTable = timetable2table(tt);
assert(istable(asTable));
assert(width(asTable) == 2);
roundTrip = table2timetable(asTable);
assert(istimetable(roundTrip));
assert(roundTrip.X == tt.X);
withoutTimes = timetable2table(tt, 'ConvertRowTimes', false);
assert(width(withoutTimes) == 1);

fromArray = array2timetable([1 4; 2 5; 3 6], ...
    'RowTimes', rt, 'VariableNames', {'A', 'B'});
assert(fromArray{:, :} == [1 4; 2 5; 3 6]);

right = timetable(rt, [30; 10; 20], 'VariableNames', {'Y'});
wide = [tt right];
assert(width(wide) == 2);
tall = [wide; wide];
assert(height(tall) == 6);

trimmed = tt;
trimmed(2, :) = [];
assert(height(trimmed) == 2);
assert(day(trimmed.Time) == [2; 2]);

summary = height(tall) + width(wide) + sum(selected.X) + ...
    sum(order) + width(asTable) + width(withoutTimes) + ...
    sum(sum(fromArray{:, :})) + height(trimmed);
assert(summary == 45);
fprintf('timetable summary=%.0f\n', summary);
