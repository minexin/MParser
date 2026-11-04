'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const net = require('node:net');
const crypto = require('node:crypto');
const {spawn} = require('node:child_process');
const [executable,evidenceDirectory,mode='on'] = process.argv.slice(2);
assert.ok(executable && evidenceDirectory && ['on','off'].includes(mode));
const root = path.resolve(evidenceDirectory);
fs.mkdirSync(root,{recursive:true});
assert.equal(fs.readdirSync(root).length,0,'use a fresh evidence directory');
const workspace = path.join(root,'workspace'); fs.mkdirSync(workspace);
let server,port,sequence=0;
async function request(route,method='GET',body,status=200) {
    const response = await fetch(`http://127.0.0.1:${port}/api/v1${route}`,{
        method,headers:{'content-type':'application/json'},
        body:body===undefined ? undefined : JSON.stringify(body),signal:AbortSignal.timeout(15000),
    });
    const data = await response.json();
    fs.writeFileSync(path.join(root,`${String(++sequence).padStart(3,'0')}.json`),
        JSON.stringify({route,method,body,status:response.status,response:data},null,2));
    assert.equal(response.status,status,JSON.stringify(data)); return data;
}
function value(result,name) { return result.workspace.find(item=>item.name===name)?.value; }
function collection(result) {
    const data = result.execution_events;
    assert.equal(data.schema,'mexec.execution-events'); assert.equal(data.version,1);
    assert.equal(data.offset_unit,'utf8-byte'); assert.equal(data.end_exclusive,true);
    assert.equal(data.summary.enabled,true); assert.equal(data.summary.started,true);
    assert.equal(data.summary.retained_count,String(data.events.length));
    data.events.forEach((event,index)=>{
        assert.equal(event.sequence,String(index)); assert.ok(BigInt(event.source_context)>0n);
        assert.ok(event.source?.name); assert.ok(event.source.begin.line>=1);
    });
    return data;
}
async function main() {
    const listener = net.createServer();
    await new Promise((resolve,reject)=>{listener.once('error',reject);listener.listen(0,'127.0.0.1',resolve);});
    port=listener.address().port; await new Promise(resolve=>listener.close(resolve));
    const log=fs.openSync(path.join(root,'server.log'),'w');
    server=spawn(path.resolve(executable),['--host','127.0.0.1','--port',String(port),'--file-root',workspace],
        {windowsHide:true,stdio:['ignore',log,log]}); fs.closeSync(log);
    let spawnError; server.on('error',error=>{spawnError=error;});
    let ready=false;
    for(let index=0;index<100;++index) {
        if(spawnError)throw spawnError; assert.equal(server.exitCode,null);
        try {await request('/health');ready=true;break;}catch{}
        await new Promise(resolve=>setTimeout(resolve,50));
    }
    assert.ok(ready);
    const info=await request('/info');
    assert.equal(info.runtime.version,'1.15.0'); assert.equal(info.runtime.abi_revision,10);
    assert.equal(info.features.includes('ordered-display-events'),mode==='on');
    const {id}=await request('/sessions','POST',{name:'ordered-events'},201);
    const base=`/sessions/${id}`;
    const execute=(command,extra={})=>request(`${base}/execute`,'POST',
        {command,include_workspace_values:true,...extra});
    const legacy=await execute('a=1');
    assert.equal(legacy.status,'succeeded'); assert.equal(legacy.execution_events,undefined);
    assert.equal(value(legacy,'a').data[0],1);
    const before=(await request(`${base}/workspace`)).revision;
    if(mode==='off') {
        await request(`${base}/execute`,'POST',{command:'a=99',include_execution_events:true},400);
        assert.equal((await request(`${base}/workspace`)).revision,before);
    } else {
        for(const [name,command,expected] of [
            ['assignment','a=1',[['assignment','a','1',false]]],
            ['suppressed','a=2;',[['assignment','a','2',true]]],
            ['comment-semicolon','a=3 %;',[['assignment','a','3',false]]],
            ['reassigned','a=4\na=5',[['assignment','a','4',false],['assignment','a','5',false]]],
            ['interleaved',"a=6\ndisp('between');\na=7",[['assignment','a','6',false],['text','','between\n\n',false],['assignment','a','7',false]]],
            ['partial-failure',"a=8\nerror('Events:Stop','stop');\na=9",[['assignment','a','8',false],['error','','stop',false]]],
        ]) {
            const result=await execute(command,{include_execution_events:true});
            assert.equal(result.status,name==='partial-failure'?'runtime-failed':'succeeded',JSON.stringify(result));
            const data=collection(result); assert.equal(data.summary.truncated,false);
            assert.deepEqual(data.events.map(event=>[event.kind,event.label,event.text,event.output_suppressed]),expected,name);
            for(const event of data.events.filter(event=>event.kind==='assignment')) {
                assert.equal(event.snapshot.schema,'mparser.value-snapshot'); assert.equal(event.snapshot.version,1);
                assert.equal(event.value_unavailable,null);
            }
            if(name==='partial-failure') {
                assert.equal(value(result,'a').data[0],8); assert.equal(data.events[1].disposition,'unhandled');
                assert.equal(data.events[1].diagnostic.identifier,'Events:Stop');
            }
        }
        const partial=collection(await execute('a=12\na=13',{include_execution_events:true,max_execution_events:1}));
        assert.equal(partial.summary.generated_count,'2'); assert.equal(partial.summary.truncated,true);
        assert.equal(partial.summary.truncation_reason,'event-count'); assert.equal(partial.events.length,1);
        const zero=collection(await execute('a=14',{include_execution_events:true,max_execution_events:0}));
        assert.equal(zero.events.length,0); assert.equal(zero.summary.truncated,true);
        const nested=collection(await execute("visible=1; captured=evalc('inside=2; 3'); visible=4",{include_execution_events:true}));
        assert.ok(nested.events.some(event=>event.capture_depth===1&&event.kind==='expression'));
        const caught=collection(await execute("try; a=10; error('Events:Caught','caught'); catch; a=11; end",{include_execution_events:true}));
        assert.ok(caught.events.some(event=>event.kind==='error'&&event.disposition==='caught'));
        const source='% \u5386\u53f2\r\nx=1';
        const range=await request(`${base}/execute`,'POST',{source,source_name:'unicode.m',include_execution_events:true});
        const event=collection(range).events[0];
        assert.equal(event.source.begin.offset,String(Buffer.byteLength(source.slice(0,source.indexOf('x=1')))));
        assert.equal(event.source.begin.line,2); assert.equal(event.source.begin.column,1);
        const current=(await request(`${base}/workspace`)).revision;
        for(const extra of [{include_execution_events:1},{include_execution_events:true,max_execution_events:-1},
            {include_execution_events:true,max_execution_events:1.5},{include_execution_events:true,max_execution_events:4097},
            {max_execution_events:2}]) {
            await request(`${base}/execute`,'POST',{command:'a=99',...extra},400);
        }
        await request(`${base}/execute`,'POST',{source:'a=99',compile_only:true,include_execution_events:true},400);
        assert.equal((await request(`${base}/workspace`)).revision,current,'invalid options execute no source');
        const stateless=await request('/execute','POST',{source:'stateless=7',include_execution_events:true});
        assert.equal(collection(stateless).events[0].label,'stateless');
        console.log('PASS original six display cases, actual order/value history, suppression, errors, evalc, UTF-8 and bounded opt-in REST');
    }
    const dll=path.join(path.dirname(path.resolve(executable)),'mparser_c.dll');
    fs.writeFileSync(path.join(root,'summary.json'),JSON.stringify({mode,executable:path.resolve(executable),
        kernel:dll,kernelSha256:crypto.createHash('sha256').update(fs.readFileSync(dll)).digest('hex'),requests:sequence},null,2));
    console.log(`PASS ordered event candidate ${mode}; evidence ${root}`);
}
main().catch(error=>{console.error(error);process.exitCode=1;}).finally(async()=>{
    if(server?.pid&&server.exitCode===null){const stopped=new Promise(resolve=>server.once('close',resolve));server.kill();await stopped;}
});
