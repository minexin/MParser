'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const net = require('node:net');
const {spawn} = require('node:child_process');
const [executable, output] = process.argv.slice(2);
assert.ok(executable && output);
const root = path.resolve(output);
fs.mkdirSync(root, {recursive:true});
assert.equal(fs.readdirSync(root).length, 0);
const reference = JSON.parse(fs.readFileSync(path.join(__dirname,'../../tests/legend_reference.json'),'utf8'));
let server, port, sequence=0;
async function request(route, body, status=200) {
    const response=await fetch(`http://127.0.0.1:${port}/api/v1${route}`, {
        method:body===undefined?'GET':'POST',headers:{'content-type':'application/json'},
        body:body===undefined?undefined:JSON.stringify(body),signal:AbortSignal.timeout(15000)});
    const result=await response.json();
    fs.writeFileSync(path.join(root,`${++sequence}.json`),JSON.stringify({route,body,status:response.status,result},null,2));
    assert.equal(response.status,status,JSON.stringify(result)); return result;
}
function check(result, phase) {
    assert.equal(result.status,'succeeded',JSON.stringify(result.diagnostics));
    assert.equal(result.workspace_export_failed,false);
    const graph=result.graphics;
    const legend=graph.objects.find(node=>node.type==='Legend');
    assert.ok(legend);
    assert.deepEqual(legend.entries.map(entry=>entry.label),reference[phase].labels);
    assert.equal(legend.properties.AutoUpdate,reference[phase].auto);
    assert.deepEqual(graph.objects.filter(node=>node.type==='Line').map(node=>node.properties.DisplayName),reference[phase].names);
}
async function main() {
    const listener=net.createServer();
    await new Promise((resolve,reject)=>{listener.once('error',reject);listener.listen(0,'127.0.0.1',resolve);});
    port=listener.address().port; await new Promise(resolve=>listener.close(resolve));
    const log=fs.openSync(path.join(root,'server.log'),'w');
    server=spawn(path.resolve(executable),['--host','127.0.0.1','--port',String(port),'--file-root',root],
        {windowsHide:true,stdio:['ignore',log,log]}); fs.closeSync(log);
    let spawnError;server.on('error',error=>{spawnError=error;});
    let ready=false;
    for(let index=0;index<100;++index) {
        if(spawnError)throw spawnError;
        assert.equal(server.exitCode,null);
        try{await request('/health');ready=true;break;}catch{}
        await new Promise(resolve=>setTimeout(resolve,50));
    }
    assert.ok(ready);
    const {id}=await request('/sessions',{name:'legend-state'},201);
    const base=`/sessions/${id}`;
    const phases=[
        ['initial','a=plot(1:2); hold on; b=plot(2:3); g=legend;'],
        ['off_added',"g.AutoUpdate='off'; c=plot(3:4); c.DisplayName='third';"],
        ['off_rename',"a.DisplayName='renamed';"],['off_deleted','delete(a);'],
        ['on_again',"g.AutoUpdate='on';"],['on_added','d=plot(4:5);'],
        ['labels',"g.String={'custom B','custom C'};"],['after_labels','e=plot(5:6);'],
        ['short_labels',"g.String={'only B'};"],['extra_labels',"g.String={'B','D','E','extra'};"],
        ['empty_name',"b.DisplayName='';"],['empty_labels','g.String={};'],
        ['mixed_names',"close all; a=plot(1:2); a.DisplayName='measured'; hold on; b=plot(2:3); g=legend;"],
        ['mixed_added','c=plot(3:4);']];
    for(const [phase,command] of phases) {
        check(await request(`${base}/execute`,{command,include_workspace_values:true}),phase);
        check(await request(`${base}/execute`,{command:'labels=g.String; mode=g.AutoUpdate;',include_workspace_values:true}),phase);
    }
    const before=await request(`${base}/execute`,{command:'g.AutoUpdate=false;',include_workspace_values:true});
    for(const command of ["g.AutoUpdate=2;","g.String={'changed',42};"]) {
        const denied=await request(`${base}/execute`,{command,include_workspace_values:true});
        assert.equal(denied.status,'runtime-failed');assert.deepEqual(denied.graphics,before.graphics);
    }
    console.log('PASS Legend REST: 14 R2024b states, cross-command String reads and invalid-write atomicity');
}
main().catch(error=>{console.error(error);process.exitCode=1;}).finally(async()=>{
    if(server?.pid&&server.exitCode===null){const stopped=new Promise(resolve=>server.once('close',resolve));server.kill();await stopped;}
});
