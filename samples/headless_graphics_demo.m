% Headless graphics: a backend renders the exported graph snapshot.
line = plot([1, 2, 3], [1, 4, 9]);
alias = line;
alias.Color = [1, 0, 0];
line.DisplayName = 'squares';
axesHandle = line.Parent;
figureHandle = axesHandle.Parent;
figureHandle.Name = 'Headless squares';
axesHandle.Title = 'y = x squared';
axesHandle.XLabel = 'x';
axesHandle.YLabel = 'y';
assert(isequal(line.Color, [1, 0, 0]));
assert(isequal(axesHandle.Children, line));
assert(isvalid(alias));

% Each plot creates a separate tree in this documented subset.
temporary = plot([3, 2, 1]);
temporaryAxes = temporary.Parent;
delete(temporaryAxes);
assert(~isvalid(temporary));
% Keep line/axesHandle/figureHandle alive for SDK or machine JSON consumers.
