'use strict';
const assert=require('node:assert/strict'),fs=require('node:fs'),net=require('node:net'),path=require('node:path');
const {spawn}=require('node:child_process');
const [executable,output,mode='on']=process.argv.slice(2);assert.ok(executable&&output);assert.ok(['on','off'].includes(mode));
const root=path.resolve(output);fs.mkdirSync(root,{recursive:true});assert.equal(fs.readdirSync(root).length,0);
let server,port,count=0;
async function request(route,body,status=200){
    const response=await fetch(`http://127.0.0.1:${port}/api/v1${route}`,{method:body===undefined?'GET':'POST',
        headers:{'content-type':'application/json'},body:body===undefined?undefined:JSON.stringify(body),signal:AbortSignal.timeout(15000)});
    const result=await response.json();fs.writeFileSync(path.join(root,`${++count}.json`),JSON.stringify({route,body,status:response.status,result},null,2));
    if(Array.isArray(status))assert.ok(status.includes(response.status),JSON.stringify(result));
    else assert.equal(response.status,status,JSON.stringify(result));return result;
}
async function main(){
    const listener=net.createServer();await new Promise((resolve,reject)=>{listener.once('error',reject);listener.listen(0,'127.0.0.1',resolve);});
    port=listener.address().port;await new Promise(resolve=>listener.close(resolve));
    const log=fs.openSync(path.join(root,'server.log'),'w');server=spawn(path.resolve(executable),
        ['--host','127.0.0.1','--port',String(port),'--file-root',root],{windowsHide:true,stdio:['ignore',log,log]});fs.closeSync(log);
    let spawnError;server.on('error',error=>{spawnError=error;});let ready=false;
    for(let index=0;index<100;++index){if(spawnError)throw spawnError;assert.equal(server.exitCode,null);
        try{await request('/health');ready=true;break;}catch{}await new Promise(resolve=>setTimeout(resolve,50));}
    assert.ok(ready);const info=await request('/info');assert.ok(info.runtime.abi_revision>=10);
    assert.equal(info.features.includes('identity-graphics-writeback'),mode==='on');
    const {id}=await request('/sessions',{name:'graphics-writeback'},201);const base=`/sessions/${id}`;
    const execute=async command=>{const result=await request(`${base}/execute`,{command,include_workspace_values:true});
        assert.equal(result.status,'succeeded',JSON.stringify(result.diagnostics));return result;};
    if(mode==='off'){
        const before=await request(`${base}/workspace?include_values=true`);
        await request(`${base}/graphics-property`,{graph_identity:'1',object_identity:'3',property:'Color',
            value:{kind:'numeric',class:'double',dimensions:[1,3],data:[1,0,0]},expected_revision:before.revision},404);
        assert.deepEqual(await request(`${base}/workspace?include_values=true`),before);
        const result=await execute('marker=42;plot(1:3);');assert.equal(result.graphics.version,3);
        console.log(`PASS writeback OFF: absent feature/route, legacy execute/graphics on ABI 2.${info.runtime.abi_revision}`);return;
    }
    assert.ok(info.runtime.abi_revision>=11);
    const seed=await execute('marker=42; lines=plot([1,10;2,20]); first=lines(1); second=lines(2);');
    const graph=seed.graphics.graphIdentity,first=seed.graphics.objects[2].objectIdentity,second=seed.graphics.objects[3].objectIdentity;
    const workspace=()=>request(`${base}/workspace?include_values=true`);
    let revision=(await workspace()).revision;
    const writes=async(object,property,value,status=200,graphIdentity=graph,expected=revision)=>request(`${base}/graphics-property`,{
        graph_identity:graphIdentity,object_identity:object,property,value,expected_revision:expected},status);
    const rgb={kind:'numeric',class:'double',dimensions:[1,3],data:[1,0,0]};
    const runs=(await request(base)).run_count;
    const changed=await writes(second,'Color',rgb);assert.equal(changed.revision,String(BigInt(revision)+1n));
    const previousRevision=revision;revision=changed.revision;
    assert.deepEqual(changed.graphics.objects[3].properties.Color,[1,0,0]);
    assert.equal((await request(base)).run_count,runs,'write executes no source');
    assert.deepEqual((await request(`${base}/graphics`)).graphics,changed.graphics);
    const raced=await Promise.all([writes(second,'Color',rgb,[200,409]),writes(second,'Color',rgb,[200,409])]);
    assert.equal(raced.filter(result=>!result.error).length,1,'exactly one competing write commits');
    const winner=raced.find(result=>!result.error);assert.equal(winner.revision,String(BigInt(revision)+1n));
    revision=winner.revision;assert.equal((await request(base)).run_count,runs);
    assert.equal((await workspace()).variables.find(item=>item.name==='marker').value.data[0],42);
    const before=await workspace();
    await writes(second,'Color',rgb,409,graph,previousRevision);
    await writes(second,'Color',rgb,409,graph,'9007199254740993');
    await writes(second,'Color',rgb,400,graph,Number(revision));
    await writes(second,'Color',rgb,400,graph,'18446744073709551616');
    await writes(second,'Color',{kind:'numeric',class:'double',dimensions:[1,1],data:[2]},400);
    for(const invalid of ['0','01','+1','18446744073709551616'])await writes(invalid,'Color',rgb,400);
    await writes(Number(second),'Color',rgb,400);
    assert.deepEqual(await workspace(),before);
    assert.deepEqual((await request(`${base}/graphics`)).graphics,changed.graphics);
    const foreign=await request('/sessions',{name:'foreign'},201);
    const foreignPlot=await request(`/sessions/${foreign.id}/execute`,{command:'plot(1:3);'});
    await writes(second,'Color',rgb,409,foreignPlot.graphics.graphIdentity);
    await execute('delete(first);');revision=(await workspace()).revision;
    await writes(first,'Color',rgb,409);
    const kept=await writes(second,'Color',rgb);assert.equal(kept.graphics.objects.at(-1).id,3);
    assert.equal(kept.graphics.objects.at(-1).objectIdentity,second);revision=kept.revision;
    const followup=await execute('readback=second.Color;');
    assert.deepEqual(followup.workspace.find(item=>item.name==='readback').value.data,[1,0,0]);
    await request(`${base}/reset`,{});await execute('marker=42; plot(1:3);');revision=(await workspace()).revision;
    const reset=(await request(`${base}/graphics`)).graphics;
    assert.notEqual(reset.graphIdentity,graph);await writes(second,'Color',rgb,409);
    assert.deepEqual((await request(`${base}/graphics`)).graphics,reset);
    console.log('PASS graphics identity writeback: revision/race gate, readback, no source execution, invalid/foreign/deleted/reset rejection');
}
main().catch(error=>{console.error(error);process.exitCode=1;}).finally(async()=>{
    if(server?.pid&&server.exitCode===null){const stopped=new Promise(resolve=>server.once('close',resolve));server.kill();await stopped;}
});
