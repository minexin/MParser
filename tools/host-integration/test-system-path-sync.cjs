'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const net = require('node:net');
const path = require('node:path');
const {spawn} = require('node:child_process');
const [executable, evidenceDirectory] = process.argv.slice(2);
assert.ok(executable && evidenceDirectory, 'expected server executable and unique evidence directory');
const root = path.resolve(evidenceDirectory);
fs.mkdirSync(root, {recursive:true});
assert.equal(fs.readdirSync(root).length,0);
const workspace = path.join(root,'workspace');
fs.mkdirSync(workspace);
let server, port, sequence = 0;
async function request(route, method='GET', body, expectedStatus=200) {
    const response = await fetch(`http://127.0.0.1:${port}/api/v1${route}`, {
        method, headers:{'content-type':'application/json'},
        body:body === undefined ? undefined : JSON.stringify(body), signal:AbortSignal.timeout(15000),
    });
    const data = await response.json();
    fs.writeFileSync(path.join(root,`${String(++sequence).padStart(3,'0')}.json`),
        JSON.stringify({route,method,body,status:response.status,response:data},null,2));
    assert.equal(response.status,expectedStatus,JSON.stringify(data));
    return data;
}
function scalar(result,name) {
    const value = result.workspace.find(variable=>variable.name === name)?.value;
    assert.ok(value && value.kind === 'numeric' && value.data.length === 1,JSON.stringify(result));
    return value.data[0];
}
async function execute(base,command,expected='succeeded') {
    const result = await request(`${base}/execute`,'POST',{command,include_workspace_values:true});
    assert.equal(result.status,expected,JSON.stringify(result));
    assert.equal(result.workspace_export_failed,false);
    return result;
}
async function main() {
    const listener = net.createServer();
    await new Promise((resolve,reject)=>{listener.once('error',reject);listener.listen(0,'127.0.0.1',resolve);});
    port = listener.address().port;
    await new Promise(resolve=>listener.close(resolve));
    const log = fs.openSync(path.join(root,'server.log'),'w');
    server = spawn(path.resolve(executable),['--host','127.0.0.1','--port',String(port),'--file-root',workspace],
        {windowsHide:true,stdio:['ignore',log,log]});
    fs.closeSync(log);
    let spawnError;
    server.on('error',error=>{spawnError=error;});
    let ready = false;
    for (let index=0;index<100;++index) {
        if (spawnError) throw spawnError;
        assert.equal(server.exitCode,null);
        try { await request('/health'); ready=true; break; } catch {}
        await new Promise(resolve=>setTimeout(resolve,50));
    }
    assert.ok(ready);
    const info = await request('/info');
    assert.equal(info.runtime.version,'1.15.0');
    assert.ok(info.runtime.abi_revision>=10);
    assert.ok(info.features.includes('kernel-search-path-sync'));
    const {id} = await request('/sessions','POST',{name:'path-sync'},201);
    const base = `/sessions/${id}`;
    const paths = async () => (await request(`${base}/path`)).path;
    const write = (name,content) => request(`${base}/files`,'PUT',{path:name,content});
    await write('lib/path_sync_unique.m','function y=path_sync_unique(x)\ny=x+731;\nend\n');
    await write('other/path_sync_unique.m','function y=path_sync_unique(x)\ny=x+831;\nend\n');
    await write('work/path_sync_unique.m','function y=path_sync_unique(x)\ny=x+931;\nend\n');
    await write('work/caller.m','fromFile=path_sync_unique(11);\n');
    assert.deepEqual(await paths(),['/']);
    const beforeRead = await request(base);
    const revision = (await request(`${base}/workspace`)).revision;
    for(let index=0;index<5;++index) await paths();
    assert.equal((await request(base)).run_count,beforeRead.run_count,'GET path executes no query source');
    assert.equal((await request(`${base}/workspace`)).revision,revision);
    await execute(base,"offset=7; closure=@(x) x+offset; h=plot([1,2]); addpath('lib');");
    assert.deepEqual(await paths(),['/lib','/']);
    assert.equal(scalar(await execute(base,'picked=path_sync_unique(11);'),'picked'),742);
    await execute(base,"rmpath('lib');");
    assert.deepEqual(await paths(),['/']);
    await execute(base,'picked=path_sync_unique(11);','runtime-failed');
    await request(`${base}/path`,'POST',{action:'add',path:'/lib'});
    assert.deepEqual(await paths(),['/','/lib']);
    assert.equal(scalar(await execute(base,'picked=path_sync_unique(11);'),'picked'),742);
    await request(`${base}/path`,'POST',{action:'remove',path:'/lib'});
    await execute(base,'picked=path_sync_unique(11);','runtime-failed');
    await execute(base,"addpath('lib'); addpath('other');");
    assert.deepEqual(await paths(),['/other','/lib','/']);
    assert.equal(scalar(await execute(base,'picked=path_sync_unique(11);'),'picked'),842);
    await execute(base,"rmpath('other'); addpath('other','-end');");
    assert.deepEqual(await paths(),['/lib','/','/other']);
    assert.equal(scalar(await execute(base,'picked=path_sync_unique(11);'),'picked'),742);
    await execute(base,"addpath('other'); error('PathProbe:Expected','after mutation');",'runtime-failed');
    assert.deepEqual(await paths(),['/other','/lib','/']);
    assert.equal(scalar(await execute(base,'picked=path_sync_unique(11);'),'picked'),842);
    await request(`${base}/path`,'POST',{action:'remove',path:'/other'});
    assert.equal(scalar(await execute(base,'picked=path_sync_unique(11);'),'picked'),742);
    const beforeInvalid = await paths();
    await execute(base,"addpath('missing');",'runtime-failed');
    assert.deepEqual(await paths(),beforeInvalid);
    await execute(base,"addpath('../outside');",'runtime-failed');
    assert.deepEqual(await paths(),beforeInvalid);
    await request(`${base}/path`,'POST',{action:'add',path:'../../outside'},400);
    assert.deepEqual(await paths(),beforeInvalid);
    await execute(base,"if closure(5)~=12 || ~isvalid(h); error('path update lost module-bound values'); end;");

    const {id: controlId} = await request('/sessions','POST',{name:'rng-control'},201);
    const reference = await execute(`/sessions/${controlId}`,'rng(9123); first=rand(); second=rand();');
    const first = await execute(base,'rng(9123); first=rand();');
    assert.equal(scalar(first,'first'),scalar(reference,'first'));
    await request(`${base}/path`,'POST',{action:'add',path:'/other'});
    await paths();
    await request(`${base}/path`,'POST',{action:'remove',path:'/other'});
    assert.equal(scalar(await execute(base,'second=rand();'),'second'),scalar(reference,'second'),
        'path REST mutation/read must not recreate the SystemContext or runtime');
    await request(`${base}/cwd`,'POST',{path:'/work'});
    assert.equal(scalar(await execute(base,'picked=path_sync_unique(11);'),'picked'),942,'cwd precedes path');
    const fromFile = await request(`${base}/execute`,'POST',{source_path:'caller.m',include_workspace_values:true});
    assert.equal(fromFile.status,'succeeded',JSON.stringify(fromFile));
    assert.equal(scalar(fromFile,'fromFile'),942);
    await execute(base,"path('');");
    assert.deepEqual(await paths(),[],'REST must not invent a root path entry');
    assert.equal(scalar(await execute(base,'picked=path_sync_unique(11);'),'picked'),942,'cwd is independent of path');
    await request(`${base}/cwd`,'POST',{path:'/'});
    await execute(base,'picked=path_sync_unique(11);','runtime-failed');
    await request(`${base}/reset`,'POST',{});
    assert.deepEqual(await paths(),[],'reset does not silently restore stale host path state');
    await execute(base,"addpath('lib');");
    assert.equal(scalar(await execute(base,'picked=path_sync_unique(11);'),'picked'),742);
    assert.deepEqual((await request(`/sessions/${controlId}/path`)).path,['/'],'session paths are isolated');
    console.log(`PASS authoritative kernel paths: next-command/file lookup, order, removal, clear, cwd, failure state, no hidden execution/runtime reset; evidence ${root}`);
}
main().catch(error=>{console.error(error);process.exitCode=1;}).finally(async()=>{
    if(server?.pid && server.exitCode===null) {
        const stopped = new Promise(resolve=>server.once('close',resolve));
        server.kill(); await stopped;
    }
});
