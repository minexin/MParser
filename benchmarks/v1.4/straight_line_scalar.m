x = 1.125;
a = sin(x) + cos(x);
b = sqrt(abs(a) + 1.0) * 1.25;
c = exp(b * 0.01) + log(b + 2.0);
d = tan(c * 0.05) + asin(sin(c * 0.02));
e = acos(cos(d * 0.03)) + atan(d);
f = (e + c) * (b + 0.5) / (a + 3.0);
g = sin(f) * cos(e) + sqrt(abs(f) + 1.0);
h = exp(g * 0.01) + log(g + 2.0);
i = tan(h * 0.05) + asin(sin(h * 0.02));
j = acos(cos(i * 0.03)) + atan(i);
k = (j + h) * (g + 0.5) / (f + 3.0);
l = sin(k) * cos(j) + sqrt(abs(k) + 1.0);
m = exp(l * 0.01) + log(l + 2.0);
n = tan(m * 0.05) + asin(sin(m * 0.02));
o = acos(cos(n * 0.03)) + atan(n);
p = (o + m) * (l + 0.5) / (k + 3.0);
baseline_result = p + o + n + m;
clear x a b c d e f g h i j k l m n o p
baseline_result
