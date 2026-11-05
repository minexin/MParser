function run_legend_creation_reference(outputPath, expectedPath)
cases = {
    'positional', 'g=legend(''A'',''B'',''C'');';
    'cell_row', 'g=legend({''A'',''B'',''C''});';
    'cell_column', 'g=legend({''A'';''B'';''C''});';
    'string_array', 'g=legend(["A","B","C"]);';
    'character_matrix', 'g=legend([''One'';''Two'';''Tri'']);';
    'short', 'g=legend(''A'');';
    'extra', 'g=legend(''A'',''B'',''C'',''extra'');';
    'empty_middle', 'g=legend(''A'','''',''C'');';
    'empty_all', 'g=legend({'''','''',''''});';
    'empty_cell', 'g=legend({});';
    'empty_character', 'g=legend('''');';
    'reserved_label', 'g=legend({''off'',''Location'',''show''});';
    'subset', 'g=legend([c,a],{''C'',''A''});';
    'subset_default', 'g=legend([c,a]);';
    'subset_scalar', 'g=legend(b,''B'');';
    'subset_duplicate', 'g=legend([a,a,b],{''first'',''second'',''third''});';
    'subset_column', 'g=legend([c;a],{''C'';''A''});';
    'subset_empty', 'h=[a,b,c]; g=legend(h([]),{''A''});';
    'target_labels', 'g=legend(ax,{''A'',''B'',''C''});';
    'target_subset', 'g=legend(ax,[c,a],{''C'',''A''});';
    'location', 'g=legend({''A'',''B''},''Location'',''southwest'');';
    'orientation', 'g=legend(''A'',''B'',''Orientation'',''horizontal'');';
    'properties_only', 'g=legend(''Location'',''northwest'',''AutoUpdate'',''off'');';
    'properties_duplicate', 'g=legend(''Location'',''north'',''Location'',''south'');';
    'property_case', 'g=legend({''A''},''location'',''NORTHWEST'',''orientation'',''HORIZONTAL'');';
    'property_prefix', 'g=legend({''A''},''Loc'',''nw'');';
    'property_unknown', 'g=legend({''A''},''Bogus'',1);';
    'property_invalid', 'g=legend({''A''},''Location'',''invalid'');';
    'property_unpaired', 'g=legend({''A''},''Location'');';
    'labels_mixed_invalid', 'g=legend({''A'',42});';
    'labels_numeric', 'g=legend(42);';
    'repeated_labels', 'g=legend({''oldA'',''oldB'',''oldC''}); g=legend({''newA'',''newB''});';
    'repeated_subset', 'g=legend([c,a]); g=legend({''A'',''B''});';
    'repeated_default', 'g=legend([c,a]); g=legend;';
    'repeated_show', 'g=legend([c,a]); g=legend(''show'');';
    'subset_added', 'g=legend([c,a],{''C'',''A''}); d=plot(4:5);';
    'subset_added_off', 'g=legend([c,a],{''C'',''A''},''AutoUpdate'',''off''); d=plot(4:5);';
    'empty_then_added', 'g=legend(''A'','''',''C''); d=plot(4:5);';
    'labels_unicode', 'g=legend({char([27979,37327]),char([21442,32771])});';
    'target_not_current', 'ax2=axes; other=plot(10:11); g=legend(ax,[c,a],{''C'',''A''});';
    'subset_other_axes', 'ax2=axes; other=plot(10:11); g=legend([a,other],{''A'',''O''});';
    'subset_deleted', 'delete(b); g=legend([a,b],{''A'',''B''});';
    'subset_matrix', 'g=legend([a,b;c,a]);';
    'mode_toggle', 'g=legend; legend(''toggle'');';
    'mode_case', 'g=legend(''SHOW'');';
    'cell_matrix', 'g=legend({''A'',''C'';''B'',''extra''});';
    'subset_empty_numeric', 'g=legend([],{''A''});';
    'subset_cross_figure', 'f2=figure; f2.Visible=''off''; other=plot(10:11); g=legend([a,other],{''A'',''O''});';
    'target_cross_figure', 'f2=figure; f2.Visible=''off''; other=plot(10:11); g=legend(ax,[a,other],{''A'',''O''});';
    'location_prefix', 'g=legend(''Location'',''northw'');';
    'location_ambiguous', 'g=legend(''Location'',''n'');';
    'location_short', 'g=legend(''Location'',''nw'');';
    'orientation_prefix', 'g=legend(''Orientation'',''h'');';
    'auto_numeric', 'g=legend(''AutoUpdate'',false);';
    'visible_case', 'g=legend(''Visible'',''OFF'');';
    'visible_prefix', 'g=legend(''Visible'',''of'');';
    'repeat_properties', 'g=legend([c,a]); g=legend(''Location'',''south'');';
    'repeat_empty', 'g=legend([c,a]); g=legend({});';
    'named_subset', 'a.DisplayName=''A''; c.DisplayName=''C''; g=legend([c,a]);';
    'empty_label_named', 'b.DisplayName=''before''; g=legend(''A'','''',''C'');';
    'duplicate_default', 'g=legend([a,a,b]);';
    'duplicate_deleted', 'g=legend([a,a,b],{''A'',''again'',''B''}); delete(a);';
    'empty_axes', 'delete([a,b,c]); g=legend({''A''});';
    'subset_reparent', 'g=legend([c,a]); ax2=axes; c.Parent=ax2;';
    'mixed_reparent', 'ax2=axes; other=plot(10:11); g=legend(ax,[a,other]); other.Parent=ax;';
    'invalid_after_legend', 'g=legend({''oldA'',''oldB'',''oldC''}); g=legend({''newA''},''Location'',''invalid'');';
    'automatic_reparent', 'g=legend; ax2=axes; c.Parent=ax2;';
    'two_legends_reparent', 'g=legend; ax2=axes; other=plot(10:11); g2=legend(ax2); c.Parent=ax2; delete(g2);';
    'character_trim', 'g=legend([''A '';''BC'']);';
    'character_column', 'g=legend([''A'';''B'';''C'']);';
    'visible_explicit_labels', 'g=legend({''A''},''Visible'',''off'');';
    'delete_automatic_legend', 'g=legend; delete(g);';
    'delete_manual_legend', 'g=legend([c,a]); delete(g);';
    'automatic_reparent_named', 'g=legend; c.DisplayName=''chosen''; ax2=axes; c.Parent=ax2;';
    'string_reparent', 'g=legend; g.String={''A'',''B'',''C''}; ax2=axes; c.Parent=ax2;';
    'named_arguments', 'g=legend({''A'',''B''},Location=''southwest'',Orientation=''horizontal'',AutoUpdate=false);';
    'named_only', 'g=legend(Location=''northwest'',AutoUpdate=false);';
    'named_invalid', 'g=legend({''A''},Location=''bad'');';
    'lifecycle_matrix', 'g=legend; m=[a,b;c,a]; delete(b); assert(isequal(isvalid(m),[true,false;true,true])); delete(m); assert(isequal(isvalid(m),false(2)));';
    'lifecycle_empty', 'g=legend; h=[a,b,c]; e=h([]); assert(isempty(isvalid(e))); delete(e);'
};
report.version = version;
report.cases = cell(1,size(cases,1));
for index = 1:size(cases,1)
    f=figure('Visible','off'); closer=onCleanup(@()close(f));
    ax=axes(f); a=plot(ax,1:2); hold(ax,'on'); b=plot(ax,2:3); c=plot(ax,3:4);
    lastwarn(''); entry=struct('name',cases{index,1},'source',cases{index,2});
    try
        eval(cases{index,2});
        entry.succeeded=true; entry.error='';
    catch exception
        entry.succeeded=false; entry.error=exception.identifier;
    end
    [~,entry.warning]=lastwarn;
    legends=findall(f,'Type','legend');
    entry.legend_count=numel(legends);
    entry.labels={}; entry.members=[]; entry.location=''; entry.orientation=''; entry.auto=''; entry.visible=''; entry.associated_is_original=false;
    if numel(legends)==1
        item=legends(1);
        entry.labels=reshape(item.String,1,[]);
        entry.location=item.Location; entry.orientation=item.Orientation;
        entry.auto=char(item.AutoUpdate); entry.visible=char(item.Visible);
        entry.associated_is_original=(item.Axes==ax);
        children=item.PlotChildren;
        for child=reshape(children,1,[])
            if isvalid(a)&&child==a, member=1;
            elseif isvalid(b)&&child==b, member=2;
            elseif isvalid(c)&&child==c, member=3;
            else, member=4;
            end
            entry.members(end+1)=member;
        end
    end
    entry.names=cell(1,3);
    for which=1:3
        if which==1,line=a;elseif which==2,line=b;else,line=c;end
        if isvalid(line),entry.names{which}=line.DisplayName;else,entry.names{which}='<deleted>';end
    end
    entry.current_is_original=(f.CurrentAxes==ax);
    entry.global_current_is_original=(gca==ax);
    report.cases{index}=entry;
    clear closer;
    if exist('f2','var'),close(f2);clear f2;end
end
if nargin>=2
    expected=jsondecode(fileread(expectedPath));
    assert(isequaln(jsondecode(jsonencode(report)),expected),'Legend creation reference changed');
end
file=fopen(outputPath,'w');assert(file>=0);closer=onCleanup(@()fclose(file));
fprintf(file,'%s\n',jsonencode(report));
fprintf('PASS %d actual R2024b Legend creation cases\n',numel(report.cases));
end
