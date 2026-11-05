'use strict';
const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path'),vm=require('node:vm');
const root=path.resolve(process.argv[2]);
const load=(file,context)=>vm.runInNewContext(fs.readFileSync(path.join(root,'web/js',file),'utf8'),context);
async function check(enabled) {
  const nodes=new Map(),rendered=[],calls=[],timers=[];
  const node=selector=>{if(!nodes.has(selector))nodes.set(selector,{textContent:'',addEventListener(){}});return nodes.get(selector);};
  const graph={schema:'mparser.graphics',version:4,graphIdentity:'11',objects:[
    {id:1,type:'Figure',objectIdentity:'13',layout:null,properties:{}}]};
  const features=['session-graphics-snapshot',...(enabled?['graphics-layout-environment']:[])];
  const environment={width_pixels:800,height_pixels:600,pixels_per_inch:96,character_width_pixels:7,character_height_pixels:16};
  const MExec={editor:{init(){}},command:{init(){},print(){}},workspace:{render(){}},
    plot:{draw(){},renderGraphicsSnapshot:snapshot=>rendered.push(snapshot),layoutTarget:()=>({graph_identity:'11',figure_identity:'13'})}};
  const sandbox={MExec,location:{origin:'http://example.test'},AbortController,
    document:{querySelector:node,querySelectorAll:()=>[]},sessionStorage:{getItem:()=>null,setItem(){}},
    setTimeout:(fn,ms)=>{timers.push({fn,ms});return timers.length;},clearTimeout(){},
    fetch:async(url,options)=>{
      calls.push({url,options});let data;
      if(url.endsWith('/info'))data={version:'0.1',runtime:{version:'1.15.0'},features};
      else if(url.endsWith('/sessions'))data={id:'layout-session'};
      else if(url.includes('/workspace'))data={variables:[],revision:'9007199254740994'};
      else if(url.endsWith('/graphics-layout-environment')){
        const body=JSON.parse(options.body);assert.equal(body.expected_revision,'9007199254740993');
        assert.deepEqual(body.environment,environment);assert.equal(body.graph_identity,'11');assert.equal(body.figure_identity,'13');
        graph.objects[0].layout={revision:'1',contentSizePixels:[800,600],pixelsPerInch:96,characterSizePixels:[7,16]};
        data={graphics:structuredClone(graph),revision:'9007199254740994'};
      } else if(url.endsWith('/graphics-layout'))data={graphics:structuredClone(graph),revision:'9007199254740993'};
      else if(url.endsWith('/graphics'))data={graphics:{schema:'mparser.graphics',version:3,objects:[]}};
      else data={id:'layout-session'};
      return {ok:true,text:async()=>JSON.stringify(data)};
    }};
  sandbox.window={MExec,addEventListener(){}};
  load('api.js',sandbox);load('plot-layout.js',sandbox);MExec.plotLayout.measure=()=>environment;load('app.js',sandbox);
  await MExec.app.init();
  assert.equal(MExec.app.state.sessionId,'layout-session');
  assert.equal(calls.filter(call=>call.url.endsWith('/graphics-layout-environment')).length,enabled?1:0);
  assert.equal(calls.filter(call=>call.url.endsWith('/graphics')).length,enabled?0:1);
  assert.equal(rendered.at(-1).version,enabled?4:3);
  assert.equal(calls.filter(call=>call.url.includes('/workspace')).length,enabled?2:1,'layout mutation refreshes workspace aliases/revision');
  await MExec.app.refreshGraphics();
  assert.equal(calls.filter(call=>call.url.endsWith('/graphics-layout-environment')).length,enabled?1:0,'identical environment is not reposted');
  assert.ok(!timers.some(timer=>timer.ms===5000),'no connection retry was needed');
}
async function main(){await check(true);await check(false);
  console.log('PASS layout bootstrap: actual API/app wiring, exact revision/body, workspace refresh, deduplication and legacy capability fallback');}
main().catch(error=>{console.error(error);process.exitCode=1;});
