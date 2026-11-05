'use strict';
const assert=require('node:assert/strict'),fs=require('node:fs'),net=require('node:net'),path=require('node:path'),vm=require('node:vm');
const {spawn}=require('node:child_process');
const [executable,clientRoot,output]=process.argv.slice(2);assert.ok(executable&&clientRoot&&output);
const root=path.resolve(output);fs.mkdirSync(root,{recursive:true});assert.equal(fs.readdirSync(root).length,0);
let server,port,sequence=0;
async function request(route,body){
  const response=await fetch(`http://127.0.0.1:${port}/api/v1${route}`,{method:body===undefined?'GET':'POST',
    headers:{'content-type':'application/json'},body:body===undefined?undefined:JSON.stringify(body),signal:AbortSignal.timeout(15000)});
  const result=await response.json();fs.writeFileSync(path.join(root,`${++sequence}.json`),JSON.stringify({route,body,status:response.status,result},null,2));
  if(!response.ok){const error=new Error(result.error?.message||String(response.status));error.status=response.status;throw error;}return result;
}
async function main(){
  const listener=net.createServer();await new Promise((resolve,reject)=>{listener.once('error',reject);listener.listen(0,'127.0.0.1',resolve);});
  port=listener.address().port;await new Promise(resolve=>listener.close(resolve));
  const log=fs.openSync(path.join(root,'server.log'),'w');server=spawn(path.resolve(executable),
    ['--host','127.0.0.1','--port',String(port),'--file-root',root,'--web-root',path.resolve(clientRoot,'web')],
    {windowsHide:true,stdio:['ignore',log,log]});fs.closeSync(log);
  let spawnError;server.on('error',error=>{spawnError=error;});let ready=false;
  for(let index=0;index<100;++index){if(spawnError)throw spawnError;assert.equal(server.exitCode,null);
    try{await request('/health');ready=true;break;}catch{}await new Promise(resolve=>setTimeout(resolve,50));}
  assert.ok(ready);const info=await request('/info');assert.equal(info.runtime.version,'1.15.0');
  assert.ok(info.features.includes('graphics-layout-environment'));
  const sandbox={window:{},MExec:{}};
  vm.runInNewContext(fs.readFileSync(path.resolve(clientRoot,'web/js/plot-layout.js'),'utf8'),sandbox);
  const layout=sandbox.MExec.plotLayout;
  const response=await fetch(`http://127.0.0.1:${port}/js/plot-layout.js`);assert.ok(response.ok);
  assert.equal(await response.text(),fs.readFileSync(path.resolve(clientRoot,'web/js/plot-layout.js'),'utf8'));
  const {id}=await request('/sessions',{name:'client-layout'}),base=`/sessions/${id}`;
  const execute=async command=>{const result=await request(`${base}/execute`,{command,include_workspace_values:true});
    assert.equal(result.status,'succeeded',JSON.stringify(result.diagnostics));return result;};
  await execute("a=axes; plot([0,1],[0,1]); a.Position=[0.1,0.2,0.3,0.5]; b=axes; plot([0,1],[1,0]); b.Position=[0.6,0.1,0.3,0.3];");
  const runs=(await request(base)).run_count;
  let latest,commits=0,owner={sessionId:id,epoch:0,enabled:true};
  // Controlled host metrics, not evidence of a browser measurement.
  let environment={width_pixels:1000,height_pixels:600,pixels_per_inch:96,character_width_pixels:6.5,character_height_pixels:18};
  const sync=layout.controller({current:()=>owner,api:{graphicsLayout:session=>request(`/sessions/${session}/graphics-layout`),
    graphicsLayoutEnvironment:(session,body)=>request(`/sessions/${session}/graphics-layout-environment`,body)},
    render:scene=>{latest=scene;},target:()=>({graph_identity:latest.graphIdentity,
      figure_identity:latest.objects.find(item=>item.type==='Figure').objectIdentity}),
    measure:()=>environment,onCommit:async()=>{commits++;await request(`${base}/workspace?include_values=true`);}});
  await sync.refresh();await sync.refresh();assert.equal(commits,1);assert.equal((await request(base)).run_count,runs);
  assert.equal(latest.version,4);const figure=latest.objects.find(item=>item.type==='Figure');
  const axes=latest.objects.filter(item=>item.type==='Axes');assert.equal(axes.length,2);
  assert.equal(figure.layout.revision,'1');
  assert.deepEqual(JSON.parse(JSON.stringify(layout.frame(axes[0].properties,figure.layout,1000,600))),
    {left:100,top:180,width:300,height:300});
  const converted=await execute("a.Units='pixels'; pixel_position=a.Position;");
  assert.deepEqual(converted.workspace.find(item=>item.name==='pixel_position').value.data,[101,121,300,300]);
  owner={...owner,epoch:1};await sync.refresh();assert.equal(commits,1);
  environment={...environment,width_pixels:1200,height_pixels:800};await sync.refresh();assert.equal(commits,2);
  const grown=latest.objects.find(item=>item.type==='Figure');assert.equal(grown.layout.revision,'2');
  const pixelAxes=latest.objects.find(item=>item.type==='Axes');
  assert.deepEqual(JSON.parse(JSON.stringify(layout.frame(pixelAxes.properties,grown.layout,1200,800))),
    {left:100,top:380,width:300,height:300},'pixel size remains fixed after resizing');
  const legacy=await request(`${base}/graphics`);assert.equal(legacy.graphics.version,3);
  assert.ok(!Object.hasOwn(legacy.graphics.objects.find(item=>item.type==='Figure'),'layout'));
  await request(`${base}/reset`,{});await execute('plot(1:3);');owner={...owner,epoch:2};
  await sync.refresh();assert.equal(commits,3);assert.notEqual(latest.graphIdentity,legacy.graphics.graphIdentity);
  assert.equal(latest.objects.find(item=>item.type==='Figure').layout.revision,'1','reset requires fresh injection');
  console.log('PASS real SDK/REST + client controller: static module, exact identities, deduplicated injection, source readback, resize, reset and legacy v3 isolation; metrics are controlled fixtures, not browser evidence');
}
main().catch(error=>{console.error(error);process.exitCode=1;}).finally(async()=>{
  if(server?.pid&&server.exitCode===null){const stopped=new Promise(resolve=>server.once('close',resolve));server.kill();await stopped;}
});
