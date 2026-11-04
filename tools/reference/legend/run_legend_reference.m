function run_legend_reference(outputPath, expectedPath)
f = figure('Visible','off');
cleaner = onCleanup(@()close(f));
ax = axes(f); hold(ax,'on');
a = plot(ax,1:2); b = plot(ax,2:3);
g = legend(ax,'show');
report.version = version;
report.initial = state(g, [a b]);
g.AutoUpdate = 'off';
c = plot(ax,3:4); c.DisplayName = 'third';
report.off_added = state(g, [a b c]);
a.DisplayName = 'renamed';
report.off_rename = state(g, [a b c]);
delete(a);
report.off_deleted = state(g, [b c]);
g.AutoUpdate = 'on';
report.on_again = state(g, [b c]);
d = plot(ax,4:5);
report.on_added = state(g, [b c d]);
g.String = {'custom B','custom C'};
report.labels = state(g, [b c d]);
e = plot(ax,5:6);
report.after_labels = state(g, [b c d e]);
g.String = {'only B'};
report.short_labels = state(g, [b c d e]);
g.String = {'B','D','E','extra'};
report.extra_labels = state(g, [b c d e]);
b.DisplayName = '';
report.empty_name = state(g, [b c d e]);
g.String = {};
report.empty_labels = state(g, [b c d e]);
clf(f); ax=axes(f); hold(ax,'on');
a=plot(ax,1:2); a.DisplayName='measured'; b=plot(ax,2:3); g=legend(ax,'show');
report.mixed_names = state(g,[a b]);
c=plot(ax,3:4);
report.mixed_added = state(g,[a b c]);
if nargin >= 2
    actual = jsondecode(jsonencode(report));
    expected = jsondecode(fileread(expectedPath));
    assert(isequaln(actual,expected), 'Legend reference changed');
end
file = fopen(outputPath,'w'); assert(file>=0);
closer = onCleanup(@()fclose(file));
fprintf(file,'%s\n',jsonencode(report));
disp(jsonencode(report));
end

function result = state(g, lines)
drawnow;
result.valid = isvalid(g);
if result.valid
    result.labels = g.String;
    result.auto = g.AutoUpdate;
end
result.names = get(lines, 'DisplayName');
end
