'use strict';
const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path'),net=require('node:net');
const {spawn}=require('node:child_process');
const [executable,output]=process.argv.slice(2);assert.ok(executable&&output);
const root=path.resolve(output);fs.mkdirSync(root,{recursive:true});assert.equal(fs.readdirSync(root).length,0);
const reference=JSON.parse(fs.readFileSync(path.join(__dirname,'../../tests/legend_creation_reference.json'),'utf8'));
let server,port,sequence=0;
async function request(route,body,status=200){
  const response=await fetch(`http://127.0.0.1:${port}/api/v1${route}`,{method:body===undefined?'GET':'POST',
    headers:{'content-type':'application/json'},body:body===undefined?undefined:JSON.stringify(body),signal:AbortSignal.timeout(15000)});
  const result=await response.json();fs.writeFileSync(path.join(root,`${++sequence}.json`),JSON.stringify({route,body,status:response.status,result},null,2));
  assert.equal(response.status,status,JSON.stringify(result));return result;
}
function check(graph,expected){
  const legends=graph.objects.filter(item=>item.type==='Legend');assert.equal(legends.length,expected.legend_count,expected.name);
  const index=line=>{const y=line.properties.YData;return y.length===2&&[1,2,3].includes(y[0])&&y[1]===y[0]+1?y[0]:4;};
  const names=['<deleted>','<deleted>','<deleted>'];
  for(const line of graph.objects.filter(item=>item.type==='Line')){const which=index(line);if(which<=3)names[which-1]=line.properties.DisplayName;}
  assert.deepEqual(names,expected.names,expected.name+' DisplayName');
  if(!legends.length)return;
  const legend=legends[0];assert.deepEqual(legend.entries.map(entry=>entry.label),expected.labels,expected.name+' labels');
  assert.deepEqual(legend.entries.map(entry=>index(graph.objects.find(item=>item.id===entry.line))),
    Array.isArray(expected.members)?expected.members:[expected.members],expected.name+' members');
  for(const [property,field] of [['Location','location'],['Orientation','orientation'],['AutoUpdate','auto'],['Visible','visible']])
    assert.equal(legend.properties[property],expected[field],expected.name+' '+property);
}
async function main(){
  const listener=net.createServer();await new Promise((resolve,reject)=>{listener.once('error',reject);listener.listen(0,'127.0.0.1',resolve);});
  port=listener.address().port;await new Promise(resolve=>listener.close(resolve));
  const log=fs.openSync(path.join(root,'server.log'),'w');server=spawn(path.resolve(executable),
    ['--host','127.0.0.1','--port',String(port),'--file-root',root],{windowsHide:true,stdio:['ignore',log,log]});fs.closeSync(log);
  let spawnError;server.on('error',error=>{spawnError=error;});let ready=false;
  for(let attempt=0;attempt<100;++attempt){if(spawnError)throw spawnError;assert.equal(server.exitCode,null);
    try{await request('/health');ready=true;break;}catch{}await new Promise(resolve=>setTimeout(resolve,50));}
  assert.ok(ready);const {id}=await request('/sessions',{name:'legend-creation'},201),base=`/sessions/${id}`;
  for(const expected of reference.cases){
    await request(`${base}/reset`,{});
    const seed="f=figure; f.Visible='off'; ax=gca; a=plot(1:2); hold on; b=plot(2:3); c=plot(3:4);";
    const result=await request(`${base}/execute`,{command:seed+expected.source,include_workspace_values:true});
    assert.equal(result.status,expected.succeeded?'succeeded':'runtime-failed',expected.name+' '+JSON.stringify(result.diagnostics));
    assert.equal(result.workspace_export_failed,false);check(result.graphics,expected);
    if(expected.succeeded&&expected.legend_count===1){
      const read=await request(`${base}/execute`,{command:'legend_labels=g.String;',include_workspace_values:true});
      assert.equal(read.status,'succeeded');check(read.graphics,expected);
      const labels=read.workspace.find(item=>item.name==='legend_labels').value;
      assert.deepEqual(labels.dimensions,[1,expected.labels.length]);
    }
    const layout=await request(`${base}/graphics-layout`);check(layout.graphics,expected);
  }
  console.log(`PASS Legend creation REST: ${reference.cases.length} actual R2024b cases, cross-command String, duplicate/cross-Axes references, atomic failures and array lifecycle on installed SDK`);
}
main().catch(error=>{console.error(error);process.exitCode=1;}).finally(async()=>{
  if(server?.pid&&server.exitCode===null){const stopped=new Promise(resolve=>server.once('close',resolve));server.kill();await stopped;}
});
