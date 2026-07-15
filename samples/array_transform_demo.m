A = zeros(2, 3, 2);
for k = 1:numel(A)
    A(k) = k;
end

R = reshape(A, 3, [], 2);
RV = reshape(A, [4 3]);
P = permute(R, [3 1 2]);
I = ipermute(P, [3 1 2]);
S = squeeze(reshape(1:6, 1, 1, 6));
T = repmat([1 2; 3 4], [2 1 2]);
C = cat(3, A, A + 100);
H = horzcat([1; 2], [3; 4]);
V = vertcat([1 2], [3 4]);
E = repmat([1 2], 0, 3);
Z = reshape([], 0, 2);

K = cell(1, 2, 2);
K{1, 1, 1} = 5;
K{1, 2, 2} = 7;
KR = reshape(K, 2, 2);
KP = permute(K, [3 2 1]);
KT = repmat(K, [2 1 1]);
KC = cat(1, K, K);

kr_first = KR{1};
kr_last = KR{4};
kp_value = KP{2, 2};
kt_value = KT{2, 2, 2};
kc_value = KC{2, 2, 2};

numeric_score = sum(A, "all") + sum(R, "all") + sum(RV, "all") + ...
    sum(P, "all") + sum(I, "all") + sum(S, "all") + ...
    sum(T, "all") + sum(C, "all") + sum(H, "all") + sum(V, "all");
shape_score = sum(size(A), "all") + sum(size(R), "all") + ...
    sum(size(RV), "all") + sum(size(P), "all") + ...
    sum(size(I), "all") + sum(size(S), "all") + ...
    sum(size(T), "all") + sum(size(C), "all") + ...
    sum(size(H), "all") + sum(size(V), "all") + ...
    sum(size(KR), "all") + sum(size(KP), "all") + ...
    sum(size(KT), "all") + sum(size(KC), "all");
cell_score = kr_first + kr_last + kp_value + kt_value + kc_value;
checks = R(1) + R(end) + RV(4, 3) + P(2, 3, 2) + I(3, 2, 2) + ...
    S(6) + T(4, 2, 2) + C(2, 3, 4) + H(2, 2) + V(2, 2);

summary = numeric_score + shape_score + cell_score + checks;
