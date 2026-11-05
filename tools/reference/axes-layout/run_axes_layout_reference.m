function run_axes_layout_reference(outputPath, expectedPath)
report.version = version;
report.computer = computer;
report.pixels_per_inch = get(groot,'ScreenPixelsPerInch');
report.screen_pixels = get(groot,'ScreenSize');
cases = {
    'initial', '';
    'normalized_position', "a.Position=[0.1 0.2 0.4 0.5];";
    'pixels_default', "a.Units='pixels';";
    'pixels_position', "a.Units='pixels'; a.Position=[81 121 320 300];";
    'pixels_to_normalized', "a.Units='pixels'; a.Position=[81 121 320 300]; a.Units='normalized';";
    'negative_origin', "a.Position=[-0.25 1.1 1.5 0.25];";
    'zero_width', "a.Position=[0.1 0.2 0 0.5];";
    'zero_height', "a.Position=[0.1 0.2 0.4 0];";
    'negative_width', "a.Position=[0.1 0.2 -0.4 0.5];";
    'negative_height', "a.Position=[0.1 0.2 0.4 -0.5];";
    'three_elements', "a.Position=[1 2 3];";
    'five_elements', "a.Position=[1 2 3 4 5];";
    'column_vector', "a.Position=[0.1;0.2;0.4;0.5];";
    'matrix_shape', "a.Position=[0.1 0.4;0.2 0.5];";
    'nan_origin', "a.Position=[NaN 0.2 0.4 0.5];";
    'inf_origin', "a.Position=[Inf 0.2 0.4 0.5];";
    'nan_width', "a.Position=[0.1 0.2 NaN 0.5];";
    'inf_width', "a.Position=[0.1 0.2 Inf 0.5];";
    'complex_position', "a.Position=[0.1+1i 0.2 0.4 0.5];";
    'single_position', "a.Position=single([0.1 0.2 0.4 0.5]);";
    'integer_position', "a.Position=int32([1 2 3 4]);";
    'logical_position', "a.Position=logical([0 0 1 1]);";
    'sparse_position', "a.Position=sparse([0.1 0.2 0.4 0.5]);";
    'uppercase_units', "a.Units='PIXELS';";
    'prefix_units', "a.Units='pix';";
    'string_units', 'a.Units="centimeters";';
    'ambiguous_units', "a.Units='p';";
    'invalid_units', "a.Units='invalid';";
    'numeric_units', "a.Units=1;";
    'plot_replace', "a.Units='pixels'; a.Position=[81 121 320 300]; a.XGrid='on'; plot(1:3);";
    'plot_replacechildren', "a.Units='pixels'; a.Position=[81 121 320 300]; a.XGrid='on'; a.NextPlot='replacechildren'; plot(1:3);";
    'resize_normalized', "a.Position=[0.1 0.2 0.4 0.5]; f.Position=[100 100 1000 500];";
    'resize_pixels', "a.Units='pixels'; a.Position=[81 121 320 300]; f.Position=[100 100 1000 500];";
    'reparent_normalized', "a.Position=[0.1 0.2 0.4 0.5]; g=figure('Visible','off','Units','pixels','Position',[100 100 400 300]); a.Parent=g; f=g;";
    'reparent_pixels', "a.Units='pixels'; a.Position=[81 121 320 300]; g=figure('Visible','off','Units','pixels','Position',[100 100 400 300]); a.Parent=g; f=g;";
    'figure_inches', "f.Units='inches'; f.Position=[2 2 8 6]; a.Units='pixels';";
    'figure_zero_width', "f.Position=[100 100 0 600];";
    'figure_negative_width', "f.Position=[100 100 -1 600];"
};
units = {'normalized','pixels','inches','centimeters','points','characters'};
for index=1:numel(units)
    cases(end+1,:) = {['to_' units{index}], ...
        "a.Position=[0.1 0.2 0.4 0.5]; a.Units='"+units{index}+"';"};
    cases(end+1,:) = {['roundtrip_' units{index}], ...
        "a.Position=[0.1 0.2 0.4 0.5]; a.Units='"+units{index}+"'; a.Units='normalized';"};
end
report.cases = cell(1,size(cases,1));
for index=1:size(cases,1)
    close all force;
    f=figure('Visible','off','Units','pixels','Position',[100 100 800 600]);
    a=axes(f);
    entry.name = cases{index,1};
    entry.source = char(cases{index,2});
    entry.succeeded = true;
    entry.identifier = '';
    try
        eval(entry.source);
    catch exception
        entry.succeeded = false;
        entry.identifier = exception.identifier;
    end
    drawnow;
    entry.position = a.Position;
    entry.units = a.Units;
    entry.position_class = class(a.Position);
    entry.position_size = size(a.Position);
    entry.position_constraint = a.PositionConstraint;
    entry.pixels = getpixelposition(a);
    entry.grid = a.XGrid;
    entry.figure_position = f.Position;
    entry.figure_units = f.Units;
    report.cases{index} = entry;
end
close all force;
if nargin>=2
    expected=jsondecode(fileread(expectedPath));
    actual=jsondecode(jsonencode(report));
    assert(isequaln(actual,expected),'Axes layout reference changed');
end
file=fopen(outputPath,'w'); assert(file>=0);
cleaner=onCleanup(@()fclose(file));
fprintf(file,'%s\n',jsonencode(report));
fprintf('PASS R2024b axes layout reference: %d cases\n',numel(report.cases));
end
