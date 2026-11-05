'use strict';
const assert=require('node:assert/strict'),fs=require('node:fs'),vm=require('node:vm'),path=require('node:path');
class E{constructor(){this.events={};this.children=[];this.hidden=false;this.textContent='';}addEventListener(k,f){this.events[k]=f;}appendChild(c){this.children.push(c);}set innerHTML(v){this.children=[];}}
const nodes=Object.fromEntries(['analysis-panel','analysis-status','analysis-list','analysis-functions','analysis-close','editor'].map(id=>[id,new E()]));
const checkButton=new E(),pending=[],jumps=[];
const MExec={app:{state:{sessionId:'one'},showTab(){}},editor:{state:{path:'/main.m',content:'if\n'},documentId:()=>1,fileName:()=> 'main.m',revealLine:n=>jumps.push(n)},
 api:{compile:body=>new Promise((resolve,reject)=>pending.push({body,resolve,reject}))}};
const context={window:{},MExec,document:{getElementById:id=>nodes[id],createElement:()=>new E(),querySelectorAll:()=>[checkButton]}};
vm.runInNewContext(fs.readFileSync(path.join(__dirname,'../web/js/analysis.js'),'utf8'),context);
const analysis=MExec.analysis;analysis.init();
(async()=>{
 const work=analysis.check();assert.equal(checkButton.disabled,true);assert.equal(pending[0].body.source,'if\n');
 pending[0].resolve({valid:false,functions:[],diagnostics:[{severity:'error',message:'<img src=x> unexpected end',source:{name:'main.m',begin:{line:1,column:1}}}]});
 await work;assert.equal(checkButton.disabled,false);assert.match(nodes['analysis-status'].textContent,/未通过/);
 const row=nodes['analysis-list'].children[0];assert.equal(row.children[0].textContent.includes('<img src=x>'),true);row.children[0].events.click();assert.deepEqual(jumps,[1]);
 MExec.editor.state.content='x=1;';analysis.documentChanged();assert.match(nodes['analysis-status'].textContent,/过期/);row.children[0].events.click();assert.deepEqual(jumps,[1]);
 const old=analysis.check(),fresh=analysis.check();pending[2].resolve({valid:true,functions:['foo'],diagnostics:[]});await fresh;pending[1].resolve({valid:false,diagnostics:[{message:'old'}]});await old;
 assert.match(nodes['analysis-status'].textContent,/通过/);assert.match(nodes['analysis-functions'].textContent,/foo/);
 const switched=analysis.check();MExec.app.state.sessionId='two';pending[3].resolve({valid:false,diagnostics:[{message:'wrong owner'}]});await switched;assert.match(nodes['analysis-status'].textContent,/过期/);
 const closed=analysis.check();nodes['analysis-close'].events.click();pending[4].resolve({valid:true,diagnostics:[]});await closed;assert.equal(nodes['analysis-panel'].hidden,true);
 const failed=analysis.check();pending[5].reject(Error('offline'));await failed;assert.match(nodes['analysis-status'].textContent,/offline/);assert.equal(checkButton.disabled,false);
 MExec.editor.state.content='x=1;';const foreign=analysis.check();pending[6].resolve({valid:false,diagnostics:[{severity:'error',message:'elsewhere',source:{name:'other.m',begin:{line:1}}},{severity:'warning',message:'bad range',source:{name:'main.m',begin:{line:999}}}]});await foreign;
 assert.equal(nodes['analysis-list'].children.every(r=>r.children.length===0),true,'foreign/out-of-range diagnostics are text only');
 console.log('PASS compile analysis: compiler-only request, safe diagnostic text, exact source/range navigation, stale guards, latest response, close and error cleanup');
})().catch(e=>{console.error(e);process.exitCode=1;});
