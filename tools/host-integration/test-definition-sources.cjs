'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const net = require('node:net');
const {spawn} = require('node:child_process');
const [exe,output,mode='on'] = process.argv.slice(2);
assert.ok(exe && output);
assert.ok(['on','off'].includes(mode));
const root = path.resolve(output);
fs.mkdirSync(root,{recursive:true});
assert.equal(fs.readdirSync(root).length,0);
let server,port,count=0;
async function request(route,body,status=200) {
    const response = await fetch(`http://127.0.0.1:${port}/api/v1${route}`,{
        method:body===undefined?'GET':'POST',headers:{'content-type':'application/json'},
        body:body===undefined?undefined:JSON.stringify(body),signal:AbortSignal.timeout(15000)});
    const result = await response.json();
    fs.writeFileSync(path.join(root,`${++count}.json`),JSON.stringify({route,body,status:response.status,result},null,2));
    assert.equal(response.status,status,JSON.stringify(result)); return result;
}
async function main() {
    const socket = net.createServer();
    await new Promise(resolve=>socket.listen(0,'127.0.0.1',resolve));
    port=socket.address().port; await new Promise(resolve=>socket.close(resolve));
    const log=fs.openSync(path.join(root,'server.log'),'w');
    server=spawn(path.resolve(exe),['--host','127.0.0.1','--port',String(port),'--file-root',root],
        {windowsHide:true,stdio:['ignore',log,log]}); fs.closeSync(log);
    let spawnError; server.on('error',error=>{spawnError=error;});
    let ready=false;
    for(let i=0;i<100;++i) {
        if(spawnError) throw spawnError;
        assert.equal(server.exitCode,null);
        try {await request('/health');ready=true;break;} catch {}
        await new Promise(resolve=>setTimeout(resolve,50));
    }
    assert.ok(ready);
    if(mode==='off') {
        for(const body of [{sources:[{name:'a.m',source:'x=1'}]},
            {source:'x=2',sources:[{name:'a.m',source:'x=1'}]}])
            await request('/compile',body,400);
        const legacy=await request('/compile',{source:'x=1'});
        assert.equal(legacy.valid,true);
        assert.equal(legacy.definition_directory,undefined);
        console.log('PASS disabled definition graph rejects sources including mixed legacy input');
        return;
    }
    const sources=[
        {name:'first.m',source:'% \u00e9\r\nfunction y=first(x)\r\ny=helper(x);\r\nfunction z=helper(x)\r\nz=x;\r\nend\r\nend\r\n'},
        {name:'second.m',source:'function y=second(x)\ny=helper(x);\nfunction z=helper(x)\nz=x+1;\nend\nend\n'}];
    const result=await request('/compile',{sources});
    assert.equal(result.valid,true);
    const directory=result.definition_directory;
    assert.equal(directory.complete,true);
    assert.deepEqual(directory.sources,sources.map(s=>({name:s.name,source_utf8:s.source})));
    assert.equal(directory.definitions.length,4);
    for(const [i,entry] of directory.definitions.entries()) {
        const sourceIndex=Math.floor(i/2),bytes=Buffer.from(sources[sourceIndex].source);
        assert.equal(entry.source_index,sourceIndex);
        assert.equal(entry.source.name,sources[sourceIndex].name);
        assert.equal(bytes.subarray(entry.source.begin.offset,entry.source.begin.offset+8).toString(),'function');
        assert.ok(entry.source.end.offset<=bytes.length);
        for(const position of [entry.source.begin,entry.source.end]) {
            const prefix=bytes.subarray(0,position.offset).toString('utf8');
            const lines=prefix.split('\n');
            assert.equal(position.line,lines.length);
            assert.equal(position.column,Buffer.byteLength(lines.at(-1))+1);
        }
        assert.equal(entry.parent_function,i%2?i-1:null);
        if(i%2) assert.equal(entry.name,'helper');
    }
    const noExecution=await request('/compile',{sources:[{name:'main.m',source:"error('must not run');\nfunction y=helper(x)\ny=x;\nend"}]});
    assert.equal(noExecution.valid,true);
    const invalid=await request('/compile',{sources:[sources[0],{name:'bad.m',source:'x=;'}]});
    assert.equal(invalid.valid,false); assert.equal(invalid.definition_directory.complete,false);
    assert.deepEqual(invalid.definition_directory.definitions,[]);
    for(const body of [{sources:[]},{sources:'bad'},{sources,source:'x=1'},
        {sources:[sources[0],sources[0]]},{sources:[{name:'',source:''}]},
        {sources:[{name:'bad.m',source:3}]},{sources:[{name:'bad\0.m',source:''}]},
        {sources:Array.from({length:65},(_,i)=>({name:`f${i}.m`,source:''}))},
        {sources,source_name:'ambiguous.m'},null]) await request('/compile',body,400);
    const legacy=await request('/compile',{source:sources[0].source,source_name:'first.m'});
    assert.equal(legacy.valid,true); assert.equal(legacy.definition_directory.definitions.length,2);
    console.log('PASS multi-source definition binding, nested parents, invalid graph, request validation and legacy compile');
}
main().catch(error=>{console.error(error);process.exitCode=1;}).finally(async()=>{
    if(server?.pid&&server.exitCode===null){const stopped=new Promise(resolve=>server.once('close',resolve));server.kill();await stopped;}
});
