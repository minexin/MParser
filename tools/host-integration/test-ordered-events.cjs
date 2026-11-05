'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const net = require('node:net');
const crypto = require('node:crypto');
const vm = require('node:vm');
const {spawn} = require('node:child_process');
const [executable,evidenceDirectory,mode='on',clientRoot] = process.argv.slice(2);
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
    assert.equal(info.runtime.version,'1.15.0'); assert.ok(info.runtime.abi_revision>=10);
    assert.equal(info.features.includes('ordered-display-events'),mode==='on');
    const {id}=await request('/sessions','POST',{name:'ordered-events'},201);
    const base=`/sessions/${id}`;
    let client;
    if (clientRoot) {
        const rows=[],label={hidden:true},checkbox={checked:false,disabled:false,closest:()=>label};
        const MExec={command:{print:(text,cls)=>rows.push({text,cls}),writeOutput:text=>rows.push({text,cls:'text'})},
            value:{stringify:JSON.stringify,toDisplay(){throw Error('ordered history must not use legacy value groups');}}};
        const context={MExec,window:{addEventListener(){}},location:{origin:`http://127.0.0.1:${port}`},
            fetch,AbortController,setTimeout,clearTimeout,
            document:{querySelector:selector=>selector==='#ordered-events-enabled'?checkbox:null,querySelectorAll:()=>[]}};
        for (const name of ['ordered-events.js','api.js','app.js'])
            vm.runInNewContext(fs.readFileSync(path.resolve(clientRoot,'web/js',name),'utf8'),context);
        MExec.orderedEvents.configure(info);
        assert.equal(label.hidden,mode!=='on');assert.equal(checkbox.checked,false);
        client={MExec,rows,checkbox,render(result){rows.length=0;MExec.app.handleExecuteResult(result,'command');
            assert.equal(rows.some(row=>row.text.includes('事件协议')),false,'real server event protocol is accepted');}};
    }
    const execute=(command,extra={})=>request(`${base}/execute`,'POST',
        {command,include_workspace_values:true,...extra});
    const legacy=await execute('a=1');
    assert.equal(legacy.status,'succeeded'); assert.equal(legacy.execution_events,undefined);
    assert.equal(value(legacy,'a').data[0],1);
    const before=(await request(`${base}/workspace`)).revision;
    if(mode==='off') {
        await request(`${base}/execute`,'POST',{command:'a=99',include_execution_events:true},400);
        assert.equal((await request(`${base}/workspace`)).revision,before);
        if(client){
            const response=await client.MExec.api.execute(id,{command:'client_legacy=73',include_workspace_values:true});
            assert.equal(response.status,'succeeded');assert.equal(response.execution_events,undefined);
            assert.equal(value(response,'client_legacy').data[0],73);
            assert.equal(client.checkbox.disabled,true);
        }
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
            if (client) {
                client.render(result);
                const visible=client.rows.filter(row=>row.cls==='out'||row.cls==='text'||row.text.startsWith('stop'));
                const wanted=expected.filter(item=>!item[3]);assert.equal(visible.length,wanted.length,name);
                wanted.forEach(([kind,label,text],index)=>{
                    if(kind==='assignment') assert.equal(visible[index].text.trimEnd(),`${label} =\n${text}`,name);
                    else if(kind==='text') assert.equal(visible[index].text,text,name);
                    else assert.ok(visible[index].text.startsWith(text),name);
                });
                fs.writeFileSync(path.join(root,`client-${name}.json`),JSON.stringify(client.rows,null,2));
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
        await request(`${base}/files`,'PUT',{path:'SecureSignal.m',content:[
            'classdef SecureSignal < handle',
            'properties (SetObservable)', 'Count=0', 'end',
            'properties', 'PropertyEvents=0', 'end',
            'properties (Access=private)', 'InternalSecret=91234567', 'end',
            'events', 'Changed', 'end', 'methods',
            'function watcher=watch(obj)', 'capturedSecret=obj.InternalSecret;',
            "watcher=addlistener(obj,'Changed',@(src,data)src.record(capturedSecret));", 'end',
            'function watcher=watchProperty(obj)', 'capturedSecret=obj.InternalSecret;',
            "watcher=addlistener(obj,'Count','PostSet',@(meta,data)data.AffectedObject.recordProperty(capturedSecret));", 'end',
            'function recordProperty(obj,token)', 'if token==obj.InternalSecret',
            'obj.PropertyEvents=obj.PropertyEvents+1;', 'end', 'end',
            'function record(obj,token)', 'if token==obj.InternalSecret',
            'obj.Count=obj.Count+1;', 'end', 'end',
            'function fire(obj)', "notify(obj,'Changed');", 'end', 'end', 'end',
        ].join('\n')});
        await request(`${base}/files`,'PUT',{path:'create_signal.m',
            content:'signal=SecureSignal(); watcher=signal.watch(); propertyWatcher=signal.watchProperty();'});
        const created=await request(`${base}/execute`,'POST',{
            source_path:'create_signal.m',include_workspace_values:true});
        assert.equal(created.status,'succeeded',JSON.stringify(created.diagnostics));
        assert.equal(created.execution_events,undefined,'default capture remains disabled for listener creation');
        const inspected=await execute(
            'mirror=watcher; origin=watcher.Source; callback=watcher.Callback; '+
            'propertyMirror=propertyWatcher; propertyOrigin=propertyWatcher.Object{1}; '+
            'wrapped={mirror,callback,origin,propertyMirror,propertyOrigin}; '+
            'fire(signal); total=signal.Count; propertyTotal=signal.PropertyEvents;',
            {include_execution_events:true});
        assert.equal(inspected.status,'succeeded',JSON.stringify(inspected.diagnostics));
        assert.equal(value(inspected,'total').data[0],1);
        assert.equal(value(inspected,'propertyTotal').data[0],1);
        const history=collection(inspected);
        for(const label of ['mirror','origin','callback','wrapped','propertyMirror','propertyOrigin']) {
            const displayed=history.events.find(item=>item.label===label);
            assert.ok(displayed?.snapshot,label);
            assert.equal(displayed.value_unavailable,null,label);
        }
        for(const displayed of history.events) {
            const automatic=JSON.stringify({snapshot:displayed.snapshot,text:displayed.text});
            const secretBits=Buffer.alloc(8); secretBits.writeDoubleBE(91234567);
            for(const forbidden of ['InternalSecret','capturedSecret','91234567',secretBits.toString('hex')])
                assert.equal(automatic.includes(forbidden),false,`${displayed.label}: ${forbidden}`);
        }
        const callbackWrite=history.events.findIndex(item=>item.kind==='assignment'&&item.label==='obj');
        const totalWrite=history.events.findIndex(item=>item.label==='total');
        assert.ok(callbackWrite>=0&&callbackWrite<totalWrite,'callback assignment precedes consuming command result');
        assert.equal(path.basename(history.events[callbackWrite].source.name),'SecureSignal.m');
        assert.notEqual(history.events[callbackWrite].source_context,history.events[totalWrite].source_context);
        const again=await execute('fire(signal); total=signal.Count; propertyTotal=signal.PropertyEvents;',
            {include_execution_events:true});
        assert.equal(again.status,'succeeded',JSON.stringify(again.diagnostics));
        assert.equal(value(again,'total').data[0],2);
        assert.equal(value(again,'propertyTotal').data[0],2);
        const frozenTotal=history.events.find(item=>item.label==='total').snapshot.nodes[0].numeric.real_bits[0];
        const bits=Buffer.from(frozenTotal,'hex');
        assert.equal(bits.readDoubleBE(),1,'history preserves the original callback result');
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
        if (client) {
            const disabled=await client.MExec.api.execute(id,{command:'client_default=10'});
            assert.equal(disabled.execution_events,undefined,'default checkbox does not request capture');
            client.checkbox.checked=true;
            const enabled=await client.MExec.api.execute(id,{command:'client_history=41\nclient_history=42'});
            assert.equal(collection(enabled).events.length,2);client.render(enabled);
            assert.deepEqual(client.rows.map(row=>row.text.trimEnd()),['client_history =\n41','client_history =\n42']);
            const limited=await client.MExec.api.execute(id,{command:'client_history=43\nclient_history=44',
                include_execution_events:true,max_execution_events:1});
            client.render(limited);assert.ok(client.rows.some(row=>row.text.includes('保留 1/2')));
            assert.equal(client.rows.some(row=>row.text.includes('44')),false,'truncation does not display final workspace history');
            const captured=await client.MExec.api.execute(id,{command:"outside=1\ntranscript=evalc('inside=2; 3');\noutside=4"});
            client.render(captured);assert.deepEqual(client.rows.filter(row=>row.cls==='out').map(row=>row.text.trimEnd()),
                ['outside =\n1','outside =\n4'],'evalc records remain captured');
            const privacy=await client.MExec.api.execute(id,{command:'client_origin=watcher.Source\nclient_callback=watcher.Callback'});
            client.render(privacy);
            const rendered=JSON.stringify(client.rows);
            for(const forbidden of ['InternalSecret','capturedSecret','91234567'])assert.equal(rendered.includes(forbidden),false);
            assert.equal(client.rows.filter(row=>row.cls==='out').length,2,'public object/callback summaries remain usable');
            const privacyRows=client.rows.slice(),strictFormatter=client.MExec.value.toDisplay;
            client.MExec.value.toDisplay=value=>JSON.stringify(value.data);
            const returned=await client.MExec.api.execute(id,{source:'function y=client_return()\ny=17;\nend',
                source_name:'client_return.m',entry_function:'client_return',requested_output_count:1});
            assert.equal(returned.status,'succeeded');assert.equal(returned.entry_function,'client_return');
            client.render(returned);assert.deepEqual(client.rows.map(row=>row.text),['ans = [17]'],'explicit function return is displayed once');
            client.MExec.value.toDisplay=strictFormatter;
            fs.writeFileSync(path.join(root,'client-api.json'),JSON.stringify({enabled,limited,captured,privacy,returned,privacyRows,returnRows:client.rows},null,2));
            console.log('PASS actual installed-SDK API opt-in and client history, suppression, errors, evalc, truncation and privacy (simulated DOM)');
        }
        console.log('PASS display history, retained-listener privacy/order, suppression, errors, evalc, UTF-8 and bounded opt-in REST');
    }
    const dll=path.join(path.dirname(path.resolve(executable)),'mparser_c.dll');
    fs.writeFileSync(path.join(root,'summary.json'),JSON.stringify({mode,executable:path.resolve(executable),
        kernel:dll,kernelSha256:crypto.createHash('sha256').update(fs.readFileSync(dll)).digest('hex'),requests:sequence},null,2));
    console.log(`PASS ordered event candidate ${mode}; evidence ${root}`);
}
main().catch(error=>{console.error(error);process.exitCode=1;}).finally(async()=>{
    if(server?.pid&&server.exitCode===null){const stopped=new Promise(resolve=>server.once('close',resolve));server.kill();await stopped;}
});
