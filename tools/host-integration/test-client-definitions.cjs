'use strict';
const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path'),vm=require('node:vm');
const root=path.resolve(process.argv[2]),pending=[];
let active=null;
class E {
  constructor(tag='div'){this.tag=tag;this.children=[];this.events={};this.dataset={};this.style={};this.value='';this._text='';
    this.hidden=false;this.disabled=false;this.scrollTop=0;this.scrollLeft=0;this.selectionStart=0;this.selectionEnd=0;
    this.classList={toggle(){},add(){}};}
  appendChild(child){child.parentNode=this;this.children.push(child);}
  replaceChildren(){this.children=[];this._text='';}
  addEventListener(name,handler){(this.events[name]??=[]).push(handler);}
  fire(name,event={}){let result;for(const handler of this.events[name]||[])result=handler(event);return result;}
  setAttribute(name,value){this[name]=value;}removeAttribute(name){delete this[name];}
  remove(){this.parentNode.children=this.parentNode.children.filter(child=>child!==this);}
  cloneNode(){return new E(this.tag);}focus(){active=this;}
  setSelectionRange(begin,end){this.selectionStart=begin;this.selectionEnd=end;}
  querySelector(selector){return descendants(this).find(child=>selector==='[data-document-id="'+child.dataset.documentId+'"]')||null;}
  set textContent(text){this._text=text;this.children=[];}get textContent(){return this._text+this.children.map(child=>child.textContent).join('');}
  set innerHTML(text){if(this.id!=='editor-gutter')assert.equal(text,'','compiler/user text never uses innerHTML');this.replaceChildren();}
}
function descendants(element){return element.children.flatMap(child=>[child,...descendants(child)]);}
const surface=new E(),initial=new E('textarea');initial.id='editor';surface.appendChild(initial);
const ids=['editor-doc-tabs','editor-gutter','editor-file','editor-dirty','analysis-panel','analysis-status','analysis-functions',
  'analysis-list','analysis-close','analysis-refresh','analysis-scope','analysis-sources','analysis-source-list'];
const nodes=Object.fromEntries(ids.map(id=>{const node=new E();node.id=id;return [id,node];}));nodes['analysis-scope'].value='current';
const checkButton=new E();
const document={get activeElement(){return active;},
  getElementById:id=>id==='editor'?surface.children.find(child=>child.id===id):nodes[id]||null,
  querySelector:selector=>document.getElementById(selector.slice(1)),querySelectorAll:()=>[checkButton],createElement:tag=>new E(tag)};
const MExec={app:{state:{sessionId:'one'},showTab(){}},command:{print(){}},
  api:{compile:body=>new Promise((resolve,reject)=>pending.push({body,resolve,reject}))}};
const context={MExec,window:{confirm:()=>true},document,TextEncoder,Event:class{constructor(type){this.type=type;}}};
for(const name of ['editor.js','definition-directory.js','analysis.js'])
  vm.runInNewContext(fs.readFileSync(path.join(root,'web/js',name),'utf8'),context);
const editor=MExec.editor,analysis=MExec.analysis,read=MExec.definitionDirectory.read;
const area=()=>document.getElementById('editor');
const edit=text=>{area().value=text;area().setSelectionRange(text.length,text.length);area().fire('input',{});};
const tick=()=>new Promise(resolve=>setImmediate(resolve));
const position=(source,index)=>{
  const prefix=source.slice(0,index),lines=prefix.split(/\r\n|\r|\n/);
  return {offset:Buffer.byteLength(prefix),line:lines.length,column:Buffer.byteLength(lines.at(-1))+1};
};
function definition(documents,index,sourceIndex,name,kind,start,end,parent=null,className='',declarationOnly=false){
  const unit=documents[sourceIndex];
  return {index,name,kind,class_name:className,source_index:sourceIndex,parent_function:parent,declaration_only:declarationOnly,
    source:{name:unit.sourceName,begin:position(unit.source,start),end:position(unit.source,end)}};
}
function directory(documents,definitions){
  if(!definitions){
    definitions=[];
    documents.forEach((unit,sourceIndex)=>{
      const start=unit.source.indexOf('function y='),nested=unit.source.indexOf('function z=');
      if(start<0)return;
      const index=definitions.length,name=unit.source.slice(start+11,unit.source.indexOf('(',start));
      definitions.push(definition(documents,index,sourceIndex,name,'primary-function',start,unit.source.length));
      if(nested>=0)definitions.push(definition(documents,index+1,sourceIndex,'helper','nested-function',nested,unit.source.lastIndexOf('end'),index));
    });
  }
  return {schema:'mexec.definition-directory',version:1,complete:true,offset_unit:'utf8-byte',column_unit:'utf8-byte',
    line_base:1,column_base:1,end_exclusive:true,source_name:documents[0].sourceName,source_utf8:documents[0].source,
    sources:documents.map(unit=>({name:unit.sourceName,source_utf8:unit.source,
      ...(unit.classMethodOwner?{class_method_owner:unit.classMethodOwner}:{})})),definitions};
}
const buttons=()=>descendants(nodes['analysis-functions']).filter(child=>child.className==='analysis-definition');
async function checked(){
  const work=analysis.check(),request=pending.at(-1);
  const docs=editor.compilationDocuments().filter(doc=>request.body.sources ? request.body.sources.some(unit=>unit.name===doc.sourceName) : doc.documentId===editor.documentId())
    .map(doc=>({...doc,classMethodOwner:request.body.sources?.find(unit=>unit.name===doc.sourceName)?.class_method_owner||''}));
  request.resolve({valid:true,functions:['exported'],diagnostics:[],definition_directory:directory(docs)});await work;return docs;
}
async function main(){
  editor.init();analysis.init();analysis.configure({features:['compile-definition-directory','compile-source-graph']});
  const first='% \u4e2d\u6587\ud83d\ude00\r\nfunction y=alpha(x)\r\ny=helper(x);\r\nfunction z=helper(x)\r\nz=x;\r\nend\r\nend\r\n';
  const second='% \u00e9\rfunction y=beta(x)\ry=helper(x);\rfunction z=helper(x)\rz=x+1;\rend\rend\r';
  editor.open('/a/alpha.m',first,'one');const a=editor.documentId(),elementA=area();
  editor.open('/b/beta.m',second,'one');const b=editor.documentId();
  editor.newFile();edit('independent draft');const draft=editor.documentId(),draftArea=area();
  const beforeSave=editor.saveSnapshot().sequence;editor.compilationDocuments();editor.compilationDocuments();
  assert.equal(editor.saveSnapshot().sequence,beforeSave+1,'compile snapshots do not advance save sequencing');
  nodes['analysis-scope'].value='open';
  const initialCheck=analysis.check();
  const all=editor.compilationDocuments();pending.at(-1).resolve({valid:true,functions:[],diagnostics:[],definition_directory:directory(all)});await initialCheck;
  const sourceRows=nodes['analysis-source-list'].children;
  sourceRows[2].children[0].children[0].checked=false;sourceRows[2].children[0].children[0].fire('change');
  const docs=await checked();assert.equal(pending.at(-1).body.sources.length,2);
  assert.equal(pending.at(-1).body.sources[0].source,first,'raw CRLF is retained');assert.equal(pending.at(-1).body.sources[1].source,second,'raw CR is retained');
  assert.equal(buttons().length,4);assert.ok(buttons()[1].textContent.includes('/a/alpha.m'));assert.ok(buttons()[3].textContent.includes('/b/beta.m'));
  const rows=read(directory(docs),docs);
  assert.equal(rows[0].range.start,first.replace(/\r\n?/g,'\n').indexOf('function y='),'astral UTF-8 prefix maps to UTF-16');
  assert.equal(rows[1].depth,1);assert.equal(rows[3].depth,1);
  const snapshot=editor.compilationDocuments().find(doc=>doc.documentId===a);
  snapshot.content='mutated copy';assert.equal(editor.state.content,'independent draft','snapshots do not mutate live editor state');
  buttons()[1].fire('click');assert.equal(editor.documentId(),a);assert.equal(area(),elementA);
  assert.ok(area().value.slice(area().selectionStart,area().selectionEnd).startsWith('function z=helper'));
  assert.ok(!nodes['analysis-status'].textContent.includes('过期'),'cross-document activation does not expire an unchanged graph');
  buttons()[3].fire('click');assert.equal(editor.documentId(),b);assert.equal(editor.state.dirty,false);
  editor.activateDocument(draft);assert.equal(area(),draftArea);assert.equal(editor.state.content,'independent draft');
  assert.equal(editor.undo(),true);assert.equal(editor.state.content,'');assert.equal(editor.redo(),true);
  assert.equal(editor.state.content,'independent draft','definition navigation preserves another draft and its undo history');
  assert.ok(!nodes['analysis-status'].textContent.includes('过期'),'unselected draft edits do not change the compiled graph');
  editor.activateDocument(a);const staleButton=buttons()[3];edit(editor.state.content+'\n% changed');
  assert.equal(staleButton.disabled,true);editor.undo();assert.equal(editor.state.content,first.replace(/\r\n?/g,'\n'));
  staleButton.fire('click');assert.equal(editor.documentId(),a,'edit/undo cannot revive an expired navigation result');
  const stalePlan=docs[0];assert.equal(editor.isCompilationDocumentCurrent(stalePlan),false,'revision protects ABA text changes');
  await checked();const sessionButton=buttons()[3];MExec.app.state.sessionId='two';sessionButton.fire('click');
  assert.equal(editor.documentId(),a);assert.equal(sessionButton.disabled,true,'late actions cannot cross sessions');
  MExec.app.state.sessionId='one';await checked();const closedButton=buttons()[3];editor.closeDocument(b);
  assert.equal(closedButton.disabled,true);closedButton.fire('click');assert.equal(editor.hasDocument(b),false,'closed source is never recreated');
  const earlier=analysis.check(),earlyRequest=pending.at(-1),later=analysis.check(),lateRequest=pending.at(-1);
  const current=editor.compilationDocuments().filter(doc=>doc.documentId===a);
  lateRequest.resolve({valid:true,functions:['fresh'],diagnostics:[],definition_directory:directory(current)});await later;
  earlyRequest.resolve({valid:false,functions:['OLD'],diagnostics:[{message:'old result'}]});await earlier;
  assert.ok(nodes['analysis-functions'].textContent.includes('fresh')&&!nodes['analysis-functions'].textContent.includes('OLD'));
  const malformed=analysis.check();const wrong=directory(current);wrong.definitions[0].source.begin.column++;
  pending.at(-1).resolve({valid:true,functions:[],diagnostics:[],definition_directory:wrong});await malformed;
  assert.equal(buttons().length,0);assert.ok(nodes['analysis-functions'].textContent.includes('不可导航'));
  const sourceOwner=nodes['analysis-source-list'].children[0].children[2].children[0];sourceOwner.value='ExplicitOwner';sourceOwner.fire('input');
  const explicit=analysis.check();assert.equal(pending.at(-1).body.sources[0].class_method_owner,'ExplicitOwner');
  pending.at(-1).resolve({valid:false,functions:[],diagnostics:[]});await explicit;
  sourceOwner.value='';sourceOwner.fire('input');

  const valid=directory(docs);
  for(const mutate of [d=>d.version=2,d=>d.complete=false,d=>d.offset_unit='utf16',d=>d.end_exclusive=false,
    d=>d.sources[0].source_utf8=first.replace(/\r\n/g,'\n'),d=>d.definitions[1].source.name='/wrong/alpha.m',
    d=>d.definitions[0].source.begin.offset=Buffer.byteLength('% \u4e2d\u6587')+1,
    d=>d.definitions[1].source.end.offset=Number.MAX_SAFE_INTEGER+1,d=>d.definitions[2].source.begin.line=999,
    d=>d.definitions[1].parent_function=1,d=>{d.definitions[0].parent_function=1;d.definitions[1].parent_function=0;},
    d=>d.definitions[1].source.end.offset=0,d=>d.sources[0].class_method_owner='invented']){
    const bad=JSON.parse(JSON.stringify(valid));mutate(bad);assert.throws(()=>read(bad,docs));
  }
  const external=[{...docs[0],sourceName:'@Range/Range.m',source:'classdef Range\nmethods\ny=read(obj)\nend\nend\n',content:'classdef Range\nmethods\ny=read(obj)\nend\nend\n'},
    {...docs[1],sourceName:'@Range/read.m',source:'function y=read(obj)\ny=1;\nend\n',content:'function y=read(obj)\ny=1;\nend\n',classMethodOwner:'Range'}];
  const declarations=[definition(external,0,0,'read','method',external[0].source.indexOf('y=read'),external[0].source.indexOf('\nend'),null,'Range',true),
    definition(external,1,1,'read','method',0,external[1].source.length,null,'Range')];
  const methods=read(directory(external,declarations),external);
  assert.equal(methods[0].declarationOnly,true);assert.equal(methods[1].className,'Range');
  assert.equal(methods[0].range.start,external[0].source.indexOf('y=read'),'declarations do not need a function-token parser');
  const many=Array.from({length:101},(_,index)=>`function y=f${index}()\ny=${index};\nend\n`);
  editor.open('/many.m',many.join(''),'one');nodes['analysis-scope'].value='current';
  const pageWork=analysis.check(),pageDocs=editor.compilationDocuments().filter(doc=>doc.documentId===editor.documentId());
  let start=0;const entries=many.map((source,index)=>{const result=definition(pageDocs,index,0,'f'+index,index?'local-function':'primary-function',start,start+source.length);start+=source.length;return result;});
  pending.at(-1).resolve({valid:true,functions:[],diagnostics:[],definition_directory:directory(pageDocs,entries)});await pageWork;
  assert.equal(buttons().length,100);descendants(nodes['analysis-functions']).find(child=>child.tag==='button'&&child.textContent==='下一页').fire('click');
  assert.equal(buttons().length,1);assert.equal(buttons()[0].dataset.definitionIndex,'100');
  analysis.configure({features:[]});assert.equal(nodes['analysis-scope'].hidden,true);assert.equal(buttons()[0].disabled,true);
  nodes['analysis-scope'].value='open';const count=pending.length;await analysis.check();assert.equal(pending.length,count,'unsupported graph never probes the endpoint');
  assert.ok(nodes['analysis-status'].textContent.includes('多源码'));
  nodes['analysis-scope'].value='current';const closed=analysis.check();analysis.close();pending.at(-1).resolve({valid:true,diagnostics:[]});await closed;
  assert.equal(nodes['analysis-panel'].hidden,true);assert.equal(checkButton.disabled,false);await tick();
  if(process.argv[3]){
    const corpus=JSON.parse(fs.readFileSync(process.argv[3],'utf8'));
    for(const fixture of corpus){
      analysis.close();nodes['analysis-scope'].value='current';
      for(const record of Array.from(editor.documentList()))editor.closeDocument(record.id);
      for(const unit of fixture.sources)editor.open(unit.name,unit.source,'one');
      analysis.configure({features:['compile-definition-directory','compile-source-graph']});nodes['analysis-scope'].value='open';
      let replay=analysis.check(),request=pending.at(-1),ownerChanged=false;
      fixture.sources.forEach((unit,index)=>{
        if(!unit.class_method_owner)return;
        const input=nodes['analysis-source-list'].children[index].children[2].children[0];
        input.value=unit.class_method_owner;input.fire('input');ownerChanged=true;
      });
      if(ownerChanged){request.resolve({valid:false,diagnostics:[]});await replay;replay=analysis.check();request=pending.at(-1);}
      assert.deepEqual(JSON.parse(JSON.stringify(request.body.sources)),fixture.sources,'actual source graph request matches the captured SDK corpus');
      request.resolve(fixture.result);await replay;
      assert.equal(buttons().length,fixture.result.definition_directory.definitions.length,'real compiler directory is navigable');
      const actions=buttons().slice();
      actions.forEach((button,index)=>{
        const record=fixture.result.definition_directory.definitions[index],source=fixture.sources[record.source_index];
        button.fire('click');assert.equal(editor.state.path,source.name);
        const bytes=Buffer.from(source.source),begin=bytes.subarray(0,record.source.begin.offset).toString('utf8').replace(/\r\n?/g,'\n').length;
        const end=bytes.subarray(0,record.source.end.offset).toString('utf8').replace(/\r\n?/g,'\n').length;
        assert.equal(area().selectionStart,begin);assert.equal(area().selectionEnd,end,'actual compiler byte offsets select exact editor UTF-16 ranges');
      });
      assert.ok(!nodes['analysis-status'].textContent.includes('过期'));
    }
    console.log(`PASS ${corpus.length} real installed-SDK definition directories through actual analysis/editor navigation (simulated DOM)`);
  }
  console.log('PASS definition client: exact source graph/owner and raw CRLF/CR, UTF-8/UTF-16 ranges, duplicate nested names, methods/declarations, cross-document selection, drafts/undo, revision/session/close/stale guards, protocol rejection, pagination and legacy capability fallback (simulated DOM)');
}
main().catch(error=>{console.error(error);process.exitCode=1;});
