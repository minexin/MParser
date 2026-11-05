'use strict';
const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path'),vm=require('node:vm');
const root=path.resolve(process.argv[2]),messages=[],requests=[],jumps=[];
const label={hidden:true},checkbox={checked:false,disabled:false,closest:()=>label};
let formatted=0;
const MExec={value:{stringify:JSON.stringify,toDisplay(){formatted++;return 'RETURN';}},
  editor:{state:{path:'/history.m',content:'a=1\na=2\na=3'},documentId:()=>9,revealLine:n=>jumps.push(n)},
  command:{print:(text,cls,action)=>messages.push({text,cls,action}),writeOutput:text=>messages.push({text,cls:'text'})}};
const sandbox={MExec,window:{addEventListener(){}},location:{origin:'http://localhost'},AbortController,setTimeout,clearTimeout,
  document:{querySelector:selector=>selector==='#ordered-events-enabled'?checkbox:null,querySelectorAll:()=>[]},
  fetch:async(url,options)=>{requests.push({url,method:options.method,body:options.body&&JSON.parse(options.body)});
    return {ok:true,text:async()=>'{"id":"one"}'};}};
for(const name of ['ordered-events.js','api.js','app.js'])
  vm.runInNewContext(fs.readFileSync(path.join(root,'web/js',name),'utf8'),sandbox);
function event(kind,label,text,extra={}){
  return {kind,label,text,sequence:'0',source_context:'1',capture_depth:0,disposition:'visible',output_suppressed:false,
    line_spacing:'compact',value_unavailable:null,source:null,diagnostic:null,...extra};
}
function collection(events,summary={}){
  return {schema:'mexec.execution-events',version:1,offset_unit:'utf8-byte',column_unit:'utf8-byte',
    line_base:1,column_base:1,end_exclusive:true,
    summary:{enabled:true,started:true,truncated:false,truncation_reason:'none',generated_count:String(events.length),
      retained_count:String(events.length),retained_bytes:'1024',...summary},
    events:events.map((value,index)=>({...value,sequence:String(index)}))};
}
function render(data,extra={}){
  messages.length=0;formatted=0;
  const result={status:'succeeded',output_events:[{text:'LEGACY_DUPLICATE'}],
    top_level_expressions:[{value:{data:[999]}}],outputs:[{name:'wrong',value:{data:[999]}}],
    diagnostics:[],execution_events:data,...extra};
  const before=JSON.stringify(result);MExec.app.handleExecuteResult(result,'command');
  assert.equal(JSON.stringify(result),before,'rendering never changes the response');
  return messages.map(item=>item.text).join('|');
}
async function main(){
  MExec.orderedEvents.configure({features:[]});
  assert.equal(label.hidden,true);assert.equal(checkbox.disabled,true);
  checkbox.checked=true;MExec.orderedEvents.configure({features:[]});assert.equal(checkbox.checked,false);
  MExec.orderedEvents.configure({features:['ordered-display-events']});
  assert.equal(label.hidden,false);assert.equal(checkbox.disabled,false);assert.equal(checkbox.checked,false);
  const body={command:'a=1'};await MExec.api.execute('one',body);
  assert.equal(requests.at(-1).body.include_execution_events,undefined,'capability alone does not opt in');
  checkbox.checked=true;await MExec.api.execute('one',body);
  assert.equal(requests.at(-1).body.include_execution_events,true);assert.equal(requests.at(-1).body.max_execution_events,4096);
  assert.equal(body.include_execution_events,undefined,'caller input is preserved');
  await MExec.api.execute('one',{command:'a=1',include_execution_events:false});
  assert.equal(requests.at(-1).body.include_execution_events,false,'explicit legacy request is retained');
  await MExec.api.execute('one',{source:'a=1',compile_only:true});
  assert.equal(requests.at(-1).body.include_execution_events,undefined,'compile-only requests stay effect free');
  await MExec.api.execute('one',{source_path:'history.m',expected_source_content:'a=1'});
  assert.ok(requests.at(-1).url.endsWith('/conditional-execute'));assert.equal(requests.at(-1).body.include_execution_events,true);
  await MExec.api.debugStart('one',{});assert.equal(requests.at(-1).body.include_execution_events,undefined,'debug protocol is not widened');
  MExec.orderedEvents.configure({features:[]});await MExec.api.execute('one',body);
  assert.equal(requests.at(-1).body.include_execution_events,undefined);

  const warning={severity:'warning',message:'between-warning',source:null};
  const data=collection([event('assignment','a','1'),event('text','','left'),event('text','',' right\n\n'),
    event('warning','','between-warning',{diagnostic:warning}),event('assignment','a','2'),
    event('assignment','hidden','999',{output_suppressed:true}),
    event('error','','caught-secret',{disposition:'caught',diagnostic:{severity:'error',message:'caught-secret'}}),
    event('text','','evalc-hidden',{capture_depth:1}),event('expression','ans','3')]);
  data.events[0].snapshot={secret:'SNAPSHOT_MUST_NOT_BE_FORMATTED'};
  const text=render(data,{diagnostics:[warning]});
  assert.deepEqual(messages.map(item=>item.text),['a =\n1','left',' right\n\n','between-warning','a =\n2','ans =\n3']);
  assert.equal(formatted,0,'historical text comes only from protected kernel records');
  assert.ok(!text.includes('999')&&!text.includes('LEGACY')&&!text.includes('SNAPSHOT'));

  const error={severity:'error',identifier:'Test:Stop',message:'stop',source:null};
  const failed=render(collection([event('assignment','a','8'),event('error','','stop',{disposition:'unhandled',diagnostic:error})]),
    {status:'runtime-failed',diagnostics:[error]});
  assert.equal(messages.filter(item=>item.text==='stop').length,1,'unhandled event is not printed twice');
  assert.ok(failed.indexOf('a =')<failed.indexOf('|stop'));assert.equal(formatted,0);

  render(collection([event('assignment','a','1')],{truncated:true,truncation_reason:'event-count',generated_count:'9007199254740993'}),
    {status:'runtime-failed',diagnostics:[error]});
  assert.ok(messages.some(item=>item.text.includes('1/9007199254740993')),'counts do not lose uint64 precision');
  assert.ok(messages.findIndex(item=>item.text.includes('回显前缀'))<messages.findIndex(item=>item.text==='stop'));
  assert.equal(messages.some(item=>item.text.includes('LEGACY')),false,'truncation never fills history from old groups');
  render(collection([],{truncated:true,truncation_reason:'event-count',generated_count:'1'}));
  assert.ok(messages.some(item=>item.text.includes('0/1')));
  render(collection([event('assignment','large','',{value_unavailable:'value-byte-limit'})]));
  assert.ok(messages[0].text.includes('值不可用: value-byte-limit'));
  render(collection([],{started:false}),{status:'compilation-failed',diagnostics:[error]});
  assert.equal(messages.filter(item=>item.text==='stop').length,1,'pre-execution compiler diagnostics remain visible');
  render(collection([]),{entry_function:'calculate',outputs:[{name:'answer',value:{data:[4]}}]});
  assert.equal(formatted,1,'explicit entry-function returns are not lost');assert.ok(messages.at(-1).text.startsWith('ans ='));

  for(const modify of [x=>x.version=2,x=>x.events[0].sequence='2',x=>x.events[1].kind='future',
    x=>x.summary.retained_count='02',x=>x.summary.generated_count='18446744073709551616',
    x=>x.summary.generated_count='3',x=>x.events[1].diagnostic=null]){
    const invalid=collection([event('assignment','a','1'),event('error','','stop',{diagnostic:error})]);modify(invalid);
    render(invalid,{diagnostics:[error]});
    assert.ok(messages.some(item=>item.text.includes('事件协议')));
    assert.equal(messages.some(item=>item.text.includes('a =')||item.text.includes('LEGACY')),false,'invalid collections render no partial history');
  }
  render(undefined);assert.ok(messages.some(item=>item.text==='LEGACY_DUPLICATE'));assert.equal(formatted,2,'legacy result groups are unchanged');
  MExec.app.state.sessionId='one';messages.length=0;
  const plan={ownerSession:'one',documentId:9,path:'/history.m',content:MExec.editor.state.content};
  const diagnostic={...error,source:{name:'exact.m',begin:{line:2}}};
  MExec.app.handleExecuteResult({status:'runtime-failed',execution_source_name:'exact.m',
    execution_events:collection([event('error','','stop',{disposition:'unhandled',diagnostic})]),diagnostics:[diagnostic]},'editor',plan);
  const action=messages.find(item=>item.action)?.action;assert.ok(action);action();assert.deepEqual(jumps,[2]);
  MExec.editor.state.content+='changed';action();assert.deepEqual(jumps,[2]);assert.ok(messages.at(-1).text.includes('文档已变化'));
  console.log('PASS ordered display client: explicit capability/checkbox opt-in, exact kernel history, suppression/evalc/caught errors, no duplicate groups, truncation/unavailable/precision, protocol rejection, entry returns and stale-source navigation (simulated DOM)');
}
main().catch(error=>{console.error(error);process.exitCode=1;});
