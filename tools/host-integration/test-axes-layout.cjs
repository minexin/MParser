'use strict';
const assert=require('node:assert/strict'),fs=require('node:fs'),net=require('node:net'),path=require('node:path');
const {spawn}=require('node:child_process');
const [executable,output,mode='on']=process.argv.slice(2);assert.ok(executable&&output);assert.ok(['on','off'].includes(mode));
const root=path.resolve(output);fs.mkdirSync(root,{recursive:true});assert.equal(fs.readdirSync(root).length,0);
let server,port,sequence=0;
async function request(route,body,status=200){
    const response=await fetch(`http://127.0.0.1:${port}/api/v1${route}`,{method:body===undefined?'GET':'POST',
        headers:{'content-type':'application/json'},body:body===undefined?undefined:JSON.stringify(body),signal:AbortSignal.timeout(15000)});
    const result=await response.json();fs.writeFileSync(path.join(root,`${++sequence}.json`),JSON.stringify({route,body,status:response.status,result},null,2));
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
    assert.ok(ready);const info=await request('/info');assert.equal(info.runtime.version,'1.15.0');
    assert.equal(info.features.includes('graphics-layout-environment'),mode==='on');
    const {id}=await request('/sessions',{name:'axes-layout'},201);const base=`/sessions/${id}`;
    const execute=async command=>{const result=await request(`${base}/execute`,{command,include_workspace_values:true});
        assert.equal(result.status,'succeeded',JSON.stringify(result.diagnostics));return result;};
    if(mode==='off'){
        await request(`${base}/graphics-layout`,undefined,404);
        await request(`${base}/graphics-layout-environment`,{},404);
        const legacy=await execute('marker=42; plot(1:3);');assert.equal(legacy.graphics.version,3);
        console.log(`PASS layout OFF: absent capability/routes, legacy execution/graphics on ABI 2.${info.runtime.abi_revision}`);return;
    }
    assert.ok(info.runtime.abi_revision>=12 && info.features.includes('identity-graphics-writeback'));
    const seed=await execute('marker=42; a=axes; a.Position=[0.1 0.2 0.4 0.5]; plot(1:3); initial=a.Position;');
    const axes=seed.graphics.objects.find(node=>node.type==='Axes');
    assert.deepEqual(axes.properties.Position,[0.1,0.2,0.4,0.5]);assert.equal(axes.properties.Units,'normalized');
    const graph=seed.graphics.graphIdentity,target=axes.objectIdentity;
    const workspace=()=>request(`${base}/workspace?include_values=true`);
    let revision=(await workspace()).revision;
    const write=async(property,value,status=200)=>request(`${base}/graphics-property`,{
        graph_identity:graph,object_identity:target,property,value,expected_revision:revision},status);
    const position=[0.55,0.2,0.35,0.5],runs=(await request(base)).run_count;
    const changed=await write('InnerPosition',{kind:'numeric',class:'double',dimensions:[4,1],data:position});
    assert.equal(changed.revision,String(BigInt(revision)+1n));revision=changed.revision;
    assert.deepEqual(changed.graphics.objects.find(node=>node.objectIdentity===target).properties.Position,position);
    assert.equal((await request(base)).run_count,runs);
    const before=await workspace();
    for(const [dimensions,data] of [[[1,3],[0,0,1]],[[2,2],[0,0,1,1]],[[1,4],[0,0,-1,1]]]){
        await write('Position',{kind:'numeric',class:'double',dimensions,data},400);
    }
    await write('Units',{kind:'character',dimensions:[1,6],text:'pixels'},400);
    assert.deepEqual(await workspace(),before);
    assert.deepEqual((await request(`${base}/graphics`)).graphics,changed.graphics);
    const followup=await execute("readback=a.InnerPosition; a.XGrid='on'; plot([3,2,1]); after=a.Position; b=axes; b.Position=[0.05 0.1 0.3 0.7];");
    for(const name of ['readback','after'])assert.deepEqual(followup.workspace.find(item=>item.name===name).value.data,position);
    const actualAxes=followup.graphics.objects.filter(node=>node.type==='Axes');assert.equal(actualAxes.length,2);
    assert.deepEqual(actualAxes[0].properties.Position,position);assert.equal(actualAxes[0].properties.XGrid,'off');
    assert.deepEqual(actualAxes[1].properties.Position,[0.05,0.1,0.3,0.7]);
    assert.equal((await workspace()).variables.find(item=>item.name==='marker').value.data[0],42);
    const layoutRead=()=>request(`${base}/graphics-layout`);
    const absent=await layoutRead();assert.equal(absent.graphics.version,4);
    const figure=absent.graphics.objects.find(node=>node.type==='Figure');assert.equal(figure.layout,null);
    revision=(await workspace()).revision;
    const environment={width_pixels:800,height_pixels:600,pixels_per_inch:96,character_width_pixels:6,character_height_pixels:14};
    const provide=async(metrics=environment,status=200,expected=revision,graphId=graph,figureId=figure.objectIdentity)=>
        request(`${base}/graphics-layout-environment`,{graph_identity:graphId,figure_identity:figureId,
            environment:metrics,expected_revision:expected},status);
    const runsBeforeEnvironment=(await request(base)).run_count;
    const configured=await provide();assert.equal(configured.revision,String(BigInt(revision)+1n));
    assert.equal(configured.graphics.version,4);revision=configured.revision;
    const frameFigure=configured.graphics.objects.find(item=>item.type==='Figure');
    assert.deepEqual(frameFigure.layout,{revision:'1',contentSizePixels:[800,600],pixelsPerInch:96,characterSizePixels:[6,14]});
    assert.equal((await request(base)).run_count,runsBeforeEnvironment);
    assert.ok(!(await request(`${base}/graphics`)).graphics.objects.find(item=>item.type==='Figure').layout);
    const control=await layoutRead();
    for(const field of Object.keys(environment)){
        await provide({...environment,[field]:0},400);await provide({...environment,[field]:'96'},400);
    }
    await provide(environment,400,Number(revision));await provide(environment,400,'18446744073709551616');
    await provide(environment,409,'9007199254740993');await provide(environment,400,revision,graph,target);
    assert.deepEqual(await layoutRead(),control);
    const race=await Promise.all([provide(environment,[200,409]),provide(environment,[200,409])]);
    assert.equal(race.filter(item=>!item.error).length,1);const winner=race.find(item=>!item.error);
    revision=winner.revision;assert.equal(winner.graphics.objects.find(item=>item.type==='Figure').layout.revision,'2');
    const converted=await execute("a.Units='pixels'; converted=a.Position;");
    const observed=converted.workspace.find(item=>item.name==='converted').value.data;
    for(let index=0;index<4;++index)assert.ok(Math.abs(observed[index]-[441,121,280,300][index])<1e-9);
    const physical=await layoutRead();assert.equal(physical.graphics.objects.find(item=>item.objectIdentity===target).properties.Units,'pixels');
    assert.equal(physical.graphics.objects.find(item=>item.type==='Figure').layout.revision,'2');
    const foreign=await request('/sessions',{name:'layout-foreign'},201);
    const foreignPlot=await request(`/sessions/${foreign.id}/execute`,{command:'plot(1:3);'});
    revision=(await workspace()).revision;await provide(environment,409,revision,foreignPlot.graphics.graphIdentity);
    await execute('close all; plot(1:3);');revision=(await workspace()).revision;
    await provide(environment,409);const replacement=await layoutRead();
    assert.equal(replacement.graphics.objects.find(item=>item.type==='Figure').layout,null);
    await request(`${base}/reset`,{});await execute('plot(1:3);');revision=(await workspace()).revision;
    await provide(environment,409);assert.equal((await layoutRead()).graphics.objects.find(item=>item.type==='Figure').layout,null);
    console.log('PASS Axes layout: normalized properties, explicit v4 environment, source conversion, revision/race gate, legacy isolation and invalid/foreign/deleted/reset rejection');
}
main().catch(error=>{console.error(error);process.exitCode=1;}).finally(async()=>{
    if(server?.pid&&server.exitCode===null){const stopped=new Promise(resolve=>server.once('close',resolve));server.kill();await stopped;}
});
