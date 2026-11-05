root=tempname(pwd); mkdir(root); guard=onCleanup(@()rmdir(root,'s'));
fprintf('VERSION %s\n',version);
f=figure('Visible','off'); a=axes(f); l=plot(a,[1 2 3],[3 1 2]);
g=figure('Visible','off'); plot([1 2],[5 6]);
dead=figure('Visible','off'); delete(dead);
cases={
 'print_no_ext', @() print(f,fullfile(root,'print_no_ext'),'-dpng');
 'print_mismatch', @() print(f,fullfile(root,'print_mismatch.pdf'),'-dpng');
 'print_unknown_ext', @() print(f,fullfile(root,'print_unknown_ext.xyz'),'-dpng');
 'print_upper', @() print(f,fullfile(root,'print_upper.PNG'),'-dPNG');
 'print_current', @() print(fullfile(root,'print_current'),'-dpng');
 'print_axes', @() print(a,fullfile(root,'print_axes.png'),'-dpng');
 'print_line', @() print(l,fullfile(root,'print_line.png'),'-dpng');
 'save_no_ext', @() saveas(f,fullfile(root,'save_no_ext'),'png');
 'save_mismatch', @() saveas(f,fullfile(root,'save_mismatch.pdf'),'png');
 'save_unknown_ext', @() saveas(f,fullfile(root,'save_unknown_ext.xyz'),'png');
 'save_upper', @() saveas(f,fullfile(root,'save_upper.PNG'),'PNG');
 'save_infer_upper', @() saveas(f,fullfile(root,'save_infer_upper.PNG'));
 'save_default_fig', @() saveas(f,fullfile(root,'save_default_fig'));
 'save_axes', @() saveas(a,fullfile(root,'save_axes.png'));
 'save_line', @() saveas(l,fullfile(root,'save_line.png'));
 'save_array', @() saveas([f g],fullfile(root,'save_array.png'));
 'save_number', @() saveas(f.Number,fullfile(root,'save_number.png'));
 'save_dead', @() saveas(dead,fullfile(root,'save_dead.png'));
 'print_dead', @() print(dead,fullfile(root,'print_dead.pdf'),'-dpdf');
 'print_explicit_pdf', @() print(f,fullfile(root,'print_explicit_pdf'),'-dpdf');
 'print_no_current', @() print(fullfile(root,'print_no_current'),'-dpng');
};
% Each row pins the observed R2024b result and file signature, not dimensions.
png='89504E470D0A1A0A';
expected={
 '', '.png', png;
 '', '.pdf', png;
 '', '.xyz', png;
 'MATLAB:print:InvalidDeviceOption', '', '';
 '', '.png', png;
 'MATLAB:print:HGFigure', '', '';
 'MATLAB:print:HGFigure', '', '';
 '', '.png', png;
 '', '.pdf', png;
 '', '.xyz', png;
 'MATLAB:saveas:badFormat', '', '';
 'MATLAB:saveas:badFormat', '', '';
 '', '.fig', '4D41544C41422035';
 '', '.png', png;
 '', '.png', png;
 'MATLAB:print:ValidateOnlyPrintOnePage', '', '';
 '', '.png', png;
 'MATLAB:saveas:invalidHandle', '', '';
 'MATLAB:print:HandleArgs', '', '';
 '', '.pdf', '255044462D312E34';
 'MATLAB:print:ValidateNoFigure', '', '';
};
for k=1:size(cases,1)
 name=cases{k,1};
 if strcmp(name,'print_no_current'),close all;end
 identifier='';
 try
  cases{k,2}();
  fprintf('CASE %s OK\n',name);
 catch e
  identifier=e.identifier;
  fprintf('CASE %s ERROR %s %s\n',name,e.identifier,replace(e.message,newline,' '));
 end
 matches=dir(fullfile(root,[name '*']));
 matches=matches(~[matches.isdir]);
 assert(strcmp(identifier,expected{k,1}),'Unexpected diagnostic for %s',name);
 assert(numel(matches)==isempty(identifier),'Unexpected file count for %s',name);
 for j=1:numel(matches)
  fid=fopen(fullfile(root,matches(j).name),'rb');
  bytes=fread(fid,8,'*uint8'); fclose(fid);
  signature=reshape(dec2hex(bytes,2).',1,[]);
  fprintf('FILE %s %d %s\n',matches(j).name,matches(j).bytes,signature);
  assert(strcmp(matches(j).name,[name expected{k,2}]) && strcmp(signature,expected{k,3}), ...
      'Unexpected file name or signature for %s',name);
 end
end
close all;
clear guard;
fprintf('PASS %d R2024b export reference cases\n',size(cases,1));
