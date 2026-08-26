clear;

left = table([1; 2; 2], [10; 20; 21], ...
    VariableNames={'K', 'X'});
right = table([2; 2; 3], [30; 31; 40], ...
    VariableNames={'K', 'X'});

[inner, leftIndex, rightIndex] = innerjoin(left, right, ...
    'Keys', 'K');
full = outerjoin(left, right, 'Keys', 'K');
leftOnly = outerjoin(left, right, 'Keys', 'K', ...
    'MergeKeys', true, 'Type', 'left');
counts = groupcounts(left, 'K');
summaries = groupsummary(left, 'K', {'mean', 'sum'}, 'X');

assert(height(inner) == 4);
assert(leftIndex == [2; 2; 3; 3]);
assert(rightIndex == [1; 2; 1; 2]);
assert(inner.K == [2; 2; 2; 2]);
assert(inner.X_L == [20; 20; 21; 21]);
assert(inner.X_R == [30; 31; 30; 31]);
assert(width(full) == 4);
assert(height(full) == 6);
assert(width(leftOnly) == 3);
assert(height(leftOnly) == 5);
assert(counts.K == [1; 2]);
assert(counts.GroupCount == [1; 2]);
assert(summaries.mean_X == [10; 20.5]);
assert(summaries.sum_X == [10; 41]);

summary = height(inner) + height(full) + height(leftOnly) + ...
    sum(leftIndex) + sum(rightIndex) + sum(counts.GroupCount) + ...
    sum(summaries.mean_X) + sum(summaries.sum_X);
fprintf('table relational summary=%.1f\n', summary);
