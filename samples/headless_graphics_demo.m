% Headless graphics: a backend renders the exported graph snapshot.
line = plot([1, 2, 3], [1, 4, 9]);
alias = line;
alias.Color = [1, 0, 0];
line.DisplayName = 'squares';
axesHandle = line.Parent;
figureHandle = axesHandle.Parent;
figureHandle.Name = 'Headless squares';
title(axesHandle, 'y = x squared');
xlabel(axesHandle, 'x');
ylabel(axesHandle, 'y');
assert(isequal(line.Color, [1, 0, 0]));
assert(isequal(axesHandle.Children, line));
assert(isvalid(alias));

% Use a separate Figure so the example does not replace the original plot.
temporaryFigure = figure();
temporary = plot([3, 2, 1]);
delete(temporaryFigure);
assert(~isvalid(temporary));
assert(isvalid(line));
% Keep line/axesHandle/figureHandle alive for SDK or machine JSON consumers.
