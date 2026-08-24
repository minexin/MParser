clear;
clc;

S = sparse([1; 3; 3], [1; 2; 2], [4; 5; -2], 3, 3);
T = speye(3);
P = spones(S);
S(2, 3) = 7;
selected = S(:, 2);
dense = full(S);
stored = nonzeros(S);
details = whos('S');

summary = nnz(S) + nnz(T) + nnz(P) + issparse(S) + ...
          issparse(selected) + isnumeric(S) + isfloat(S) + ...
          isa(S, 'double') + details.sparse + ~issparse(dense) + ...
          (dense(2, 3) == 7) + (numel(stored) == 3) + S(2, 3);
disp(summary);
