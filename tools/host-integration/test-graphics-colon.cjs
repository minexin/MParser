'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const net = require('node:net');
const path = require('node:path');
const {spawn} = require('node:child_process');
const [executable, output] = process.argv.slice(2);
assert.ok(executable && output, 'expected server executable and unique evidence directory');
const root = path.resolve(output);
fs.mkdirSync(root, {recursive:true});
assert.equal(fs.readdirSync(root).length, 0);
const corpus = JSON.parse(fs.readFileSync(path.join(__dirname, '../../tests/colon_reference_cases.json'), 'utf8'));
let server, port, sequence = 0;
async function request(route, body, status = 200, method = body === undefined ? 'GET' : 'POST') {
    const response = await fetch(`http://127.0.0.1:${port}/api/v1${route}`, {
        method, headers:{'content-type':'application/json'},
        body:body === undefined ? undefined : JSON.stringify(body), signal:AbortSignal.timeout(15000),
    });
    const result = await response.json();
    fs.writeFileSync(path.join(root, `${++sequence}.json`), JSON.stringify({route, body, status:response.status, result}, null, 2));
    assert.equal(response.status, status, JSON.stringify(result));
    return result;
}
function value(result, name) {
    const found = result.workspace.find(item => item.name === name)?.value;
    assert.ok(found, `missing ${name}`);
    return found;
}
function checkPlot(result, id) {
    assert.equal(result.status, 'succeeded', JSON.stringify(result.diagnostics));
    assert.equal(result.workspace_export_failed, false);
    const reference = corpus.cases.find(item => item.id === id);
    const expected = reference.hex.map(hex => hex.length === 8
        ? Buffer.from(hex, 'hex').readFloatBE() : Buffer.from(hex, 'hex').readDoubleBE());
    assert.equal(value(result, 'x').class, reference.class);
    assert.deepEqual(value(result, 'x').dimensions, reference.dimensions);
    assert.deepEqual(value(result, 'x').data, expected);
    assert.equal(result.graphics.version, 3);
    const lines = result.graphics.objects.filter(item => item.type === 'Line');
    assert.equal(lines.length, 1);
    assert.deepEqual(lines[0].properties.XData, expected);
    assert.equal(lines[0].properties.YData.length, expected.length);
}
async function main() {
    const listener = net.createServer();
    await new Promise((resolve, reject) => { listener.once('error', reject); listener.listen(0, '127.0.0.1', resolve); });
    port = listener.address().port;
    await new Promise(resolve => listener.close(resolve));
    const log = fs.openSync(path.join(root, 'server.log'), 'w');
    server = spawn(path.resolve(executable), ['--host', '127.0.0.1', '--port', String(port), '--file-root', root],
        {windowsHide:true, stdio:['ignore', log, log]});
    fs.closeSync(log);
    let spawnError;
    server.on('error', error => { spawnError = error; });
    let ready = false;
    for (let index = 0; index < 100; ++index) {
        if (spawnError) throw spawnError;
        assert.equal(server.exitCode, null);
        try { await request('/health'); ready = true; break; } catch {}
        await new Promise(resolve => setTimeout(resolve, 50));
    }
    assert.ok(ready);
    const info = await request('/info');
    assert.equal(info.runtime.version, '1.15.0');
    const {id} = await request('/sessions', {name:'colon-graphics'}, 201);
    const base = `/sessions/${id}`;
    for (const id of ['host_plot', 'descending_plot', 'single_plot', 'nondividing']) {
        const reference = corpus.cases.find(item => item.id === id);
        const result = await request(`${base}/execute`, {
            command:`x=${reference.expression}; h=plot(x,sin(x));`, include_workspace_values:true,
        });
        checkPlot(result, id);
        const followup = await request(`${base}/execute`, {
            command:'sample_count=numel(h.XData); sample_last=h.XData(end);', include_workspace_values:true,
        });
        assert.equal(followup.status, 'succeeded', JSON.stringify(followup.diagnostics));
        assert.equal(value(followup, 'sample_count').data[0], reference.dimensions[1]);
        assert.equal(value(followup, 'sample_last').data[0], value(result, 'x').data.at(-1));
    }
    await request(`${base}/files`, {path:'colon_file.m', content:'x=0:0.05:10; h=plot(x,sin(x));'}, 200, 'PUT');
    checkPlot(await request(`${base}/execute`, {source_path:'colon_file.m', include_workspace_values:true}), 'host_plot');
    const loops = await request(`${base}/execute`, {command:[
        'count=0; last=-1;', 'for outer=1:12', 'for point=0:0.05:10',
        'count=count+1; last=point;', 'end', 'end', 'empty=1:0:3;',
    ].join('\n'), include_workspace_values:true});
    assert.equal(loops.status, 'succeeded', JSON.stringify(loops.diagnostics));
    assert.equal(value(loops, 'count').data[0], 2412);
    assert.equal(value(loops, 'last').data[0], 10);
    assert.deepEqual(value(loops, 'empty').dimensions, [1, 0]);
    assert.deepEqual(value(loops, 'empty').data, []);
    console.log('PASS colon graphics: exact R2024b XData, single/descending/nondividing, source file and cross-request loops');
}
main().catch(error => { console.error(error); process.exitCode = 1; }).finally(async () => {
    if (server?.pid && server.exitCode === null) {
        const stopped = new Promise(resolve => server.once('close', resolve));
        server.kill();
        await stopped;
    }
});
