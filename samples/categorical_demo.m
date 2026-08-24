clear;

c = categorical({'b', 'a', 'b', ''});
assert(iscategorical(c));
assert(all(double(c(1:3)) == [2 1 2]));
assert(isundefined(c(4)));

c = addcats(c, {'c', 'd'}, 'Before', 'b');
c(4) = 'c';
c(6) = 'd';
assert(isundefined(c(5)));
assert(sum(c == 'b') == 2);

d = c;
d(2) = [];
assert(numel(d) == 5);
h = [c c];
assert(numel(h) == 12);

o = categorical([1 2 1], [1 2], {'low', 'high'}, ...
    'Ordinal', true);
assert(isordinal(o));
assert(isprotected(o));
assert(o(1) < o(2));

counts = countcats(c);
assert(all(counts == [1 1 1 2]));
summary = sum(counts) + sum(c == 'b') + ...
    double(isordinal(o)) + double(isprotected(o));
fprintf('categorical summary=%.0f\n', summary);

tc = categorical({'b'; 'a'; 'b'});
t = table([3; 1; 2], tc, 'VariableNames', {'X', 'C'});
t.C(3) = 'a';
assert(sum(t.C == 'a') == 2);
[sortedTable, order] = sortrows(t, {'C', 'X'});
assert(all(order == [2; 3; 1]));
assert(all(sortedTable.X == [1; 2; 3]));

n = table([1; 2], [3; 4], 'VariableNames', {'A', 'B'});
n{:, 1:2} = [10 30; 20 40];
n(1, :) = [];
assert(n.A == 20 && n.B == 40);

leftTable = table([1; 2], 'VariableNames', {'L'});
rightTable = table([3; 4], 'VariableNames', {'R'});
wideTable = [leftTable rightTable];
tallTable = [wideTable; wideTable];
assert(width(wideTable) == 2 && height(tallTable) == 4);

tabularSummary = height(sortedTable) + width(wideTable) + ...
    height(tallTable) + n.A;
fprintf('tabular summary=%.0f\n', tabularSummary);
