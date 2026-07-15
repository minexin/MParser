A = reshape(1:24, 2, 3, 4);

columnTotals = sum(A);
rowTotals = sum(A, 2);
pageTotals = sum(A, [1 2]);
grandTotal = sum(A, "all");
pageMeans = mean(A, 3);
vectorProduct = prod([1 2 3 4]);

X = [0 5 0; -2 0 7];
[maxByRow, maxColumn] = max(X, [], 2);
[maxByRowLinear, maxLinearIndex] = max(X, [], 2, "linear");
linearIndices = find(X);
[lastRows, lastColumns, lastValues] = find(X, 2, "last");
nonzeroRows = any(X, 2);
fullyNonzeroColumns = all(X ~= 0, 1);

Y = [3 1 5; 2 1 4];
[minByColumn, minRow] = min(Y);
elementwiseMax = max(Y, 4);
[maximum, maximumIndex] = max(A, [], "all");
rowIndices = find([0 4 0 8]);

N = [1 nan; 3 4];
omitTotals = sum(N, 1, "omitnan");
nanSafeMax = max(N, [], 1);

emptySum = sum([]);
emptyProduct = prod([]);
emptyMean = mean([]);
emptyMaximum = max([]);

summary = grandTotal + sum(columnTotals, "all") + ...
          sum(rowTotals, "all") + sum(pageTotals, "all") + ...
          sum(pageMeans, "all") + vectorProduct + ...
          sum(maxByRow, "all") + sum(maxColumn, "all") + ...
          sum(linearIndices, "all") + sum(lastRows, "all") + ...
          sum(lastColumns, "all") + sum(lastValues, "all") + ...
          sum(double(nonzeroRows), "all") + ...
          sum(double(fullyNonzeroColumns), "all") + ...
          sum(omitTotals, "all") + sum(nanSafeMax, "all") + ...
          sum(minByColumn, "all") + sum(minRow, "all") + ...
          sum(elementwiseMax, "all") + maximum + maximumIndex + ...
          emptySum + emptyProduct
