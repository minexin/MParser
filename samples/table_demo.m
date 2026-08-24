clear;

t = table([1; 2], [3; 4], ...
    VariableNames={'Left', 'Right'}, ...
    RowNames={'r1'; 'r2'});

selected = t(2, :);
content = t{:, 'Right'};
t.Left(2) = 9;
t.extra = [5; 6];
t{:, 'Right'} = [11; 12];
t(:, 'extra') = [];
t.Properties.Description = 'runtime table demo';

a = array2table([1 2; 3 4], ...
    VariableNames={'A', 'B'});
roundtrip = table2array(a);
s = table2struct(t);
u = struct2table(s);
columnStruct = struct('A', [1; 2], 'B', [3; 4]);
fromScalarStruct = struct2table(columnStruct);
duplicated = t({'r1', 'r1'}, {'Left', 'Left'});
same = t == t;
t.EmptyWide = zeros(height(t), 0);
widthWithEmpty = width(t);
t.EmptyWide = [];
widthAfterEmpty = width(t);
braceNullRejected = 0;
try
    t{:, 'Left'} = [];
catch
    braceNullRejected = 1;
end

summary = [height(t), width(t), selected.Left, content(2), ...
    t.Left(2), t.Right(2), roundtrip(2, 2), istable(u), ...
    height(fromScalarStruct), fromScalarStruct.B(2), ...
    duplicated.Left_1(2), same.Left(2), ...
    widthWithEmpty, widthAfterEmpty, braceNullRejected];
summary
