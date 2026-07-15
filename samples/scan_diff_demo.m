A = reshape(1:12, 2, 3, 2);

columnSums = cumsum(A);
rowProducts = cumprod(A, 2);
reversePages = cumsum(A, 3, "reverse");
unchanged = cumsum(A, 5);

X = [3 1 5; 2 6 4];
runningMin = cummin(X, 2);
reverseMax = cummax([3 1 2; 7 6 5], 1, "reverse");

N = [nan 2 nan; 1 nan 3];
includeSums = cumsum(N, 1, "includenan");
omitSums = cumsum(N, 1, "omitnan");
omitProducts = cumprod(N, 2, "omitmissing");
omitMinima = cummin(N, 2);
includeMaxima = cummax(N, 2, "includemissing");
reverseOmit = cumsum([1 nan 3], 2, "reverse", "omitnan");

logicalSums = cumsum(logical([1 0 1]));
logicalMinima = cummin(logical([1 0 1]));

firstDifference = diff([1 4 9 16]);
secondDifference = diff([0 5 15 30 50], 2);
matrixDifference = diff([1 3 6; 10 15 21], 1, 2);
pageDifference = diff(A, 1, 3);
logicalDifference = diff(logical([1 0 1]));
highOrder = diff([1 2 3; 4 5 6], 3);
pastDimension = diff(A, 1, 5);
emptyDifference = diff([]);
scalarDifference = diff(4);

summary = sum(columnSums, "all") + sum(rowProducts, "all") + ...
          sum(reversePages, "all") + sum(unchanged, "all") + ...
          sum(runningMin, "all") + sum(reverseMax, "all") + ...
          sum(reverseOmit, "all") + sum(logicalSums, "all") + ...
          sum(double(logicalMinima), "all") + ...
          sum(firstDifference, "all") + ...
          sum(secondDifference, "all") + ...
          sum(matrixDifference, "all") + ...
          sum(pageDifference, "all") + ...
          sum(logicalDifference, "all")
