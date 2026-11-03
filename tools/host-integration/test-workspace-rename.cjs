'use strict';
const assert = require('node:assert/strict');
const {spawn} = require('node:child_process');
const fs = require('node:fs');
const net = require('node:net');
const path = require('node:path');

const [executable, evidenceDirectory, mode = 'on'] = process.argv.slice(2);
if (!executable || !evidenceDirectory || !['on', 'off'].includes(mode)) {
    throw new Error('Expected server executable, unique evidence directory, and on/off');
}
const root = path.resolve(evidenceDirectory);
fs.mkdirSync(root, {recursive: true});
assert.equal(fs.readdirSync(root).length, 0, 'evidence directory must be empty');
const workspace = path.join(root, 'workspace');
fs.mkdirSync(workspace);
let server, port, sequence = 0;

async function request(route, method = 'GET', body, expectedStatus = 200) {
    const response = await fetch(`http://127.0.0.1:${port}/api/v1${route}`, {
        method, headers: {'content-type': 'application/json'},
        body: body === undefined ? undefined : JSON.stringify(body),
        signal: AbortSignal.timeout(15000),
    });
    const data = await response.json();
    fs.writeFileSync(path.join(root, `${String(++sequence).padStart(3, '0')}.json`),
        JSON.stringify({route, method, body, status: response.status, response: data}, null, 2));
    if (expectedStatus !== null) assert.equal(response.status, expectedStatus, JSON.stringify(data));
    return {status: response.status, data};
}

async function main() {
    const listener = net.createServer();
    await new Promise((resolve, reject) => {
        listener.once('error', reject);
        listener.listen(0, '127.0.0.1', resolve);
    });
    port = listener.address().port;
    await new Promise(resolve => listener.close(resolve));
    const log = fs.openSync(path.join(root, 'server.log'), 'w');
    server = spawn(path.resolve(executable), ['--host', '127.0.0.1', '--port', String(port), '--file-root', workspace],
        {windowsHide: true, stdio: ['ignore', log, log]});
    fs.closeSync(log);
    let spawnError;
    server.on('error', error => { spawnError = error; });
    let ready = false;
    for (let i = 0; i < 100; ++i) {
        if (spawnError) throw spawnError;
        assert.equal(server.exitCode, null, 'server exited before readiness');
        try { await request('/health'); ready = true; break; } catch {}
        await new Promise(resolve => setTimeout(resolve, 50));
    }
    assert.ok(ready, 'server ready');
    const {data: info} = await request('/info');
    assert.equal(info.runtime.version, '1.15.0');
    assert.equal(info.features.includes('conditional-workspace-rename'), mode === 'on');
    const {data: session} = await request('/sessions', 'POST', {name: 'rename-probe'}, 201);
    const base = `/sessions/${session.id}`;
    const snapshot = async () => (await request(`${base}/workspace?include_values=true`)).data;
    const value = async name => (await request(`${base}/workspace/${encodeURIComponent(name)}`)).data.value;
    const rename = (from, to, revision, status = 200) => request(
        `${base}/workspace/${encodeURIComponent(from)}/rename`, 'POST',
        {new_name: to, expected_revision: revision}, status);
    const execute = async body => {
        const {data} = await request(`${base}/execute`, 'POST', body);
        assert.equal(data.status, 'succeeded', JSON.stringify(data));
        assert.equal(data.workspace_export_failed, false);
        return data;
    };
    await execute({command: 'x=42; occupied=99; offset=7; fn=@(v) v+offset; line=plot([1,2,3]); lineAlias=line;'});
    let before = await snapshot();
    if (mode === 'off') {
        await rename('x', 'renamed', before.revision, 404);
        assert.deepEqual(await snapshot(), before);
        console.log(`PASS rename OFF: absent capability and route; evidence ${root}`);
        return;
    }
    const {data: initialSession} = await request(base);
    const original = await value('x');
    const result = (await rename('x', 'renamed', before.revision)).data;
    assert.equal(result.renamed, true);
    assert.equal(BigInt(result.revision), BigInt(before.revision) + 1n);
    assert.equal((await snapshot()).revision, result.revision);
    assert.deepEqual(await value('renamed'), original);
    await request(`${base}/workspace/x`, 'GET', undefined, 404);
    const currentSession = (await request(base)).data;
    assert.equal(currentSession.run_count, initialSession.run_count, 'rename must not execute');
    assert.equal(currentSession.source, initialSession.source, 'rename must not compile a replacement source');
    assert.equal((await rename('renamed', 'other', before.revision, 409)).data.error.code, 'workspace_conflict');
    before = await snapshot();
    assert.equal((await rename('renamed', 'occupied', before.revision, 409)).data.error.code, 'variable_name_conflict');
    assert.equal((await rename('absent', 'absent', before.revision, 404)).data.error.code, 'variable_not_found');
    assert.equal((await rename('renamed', 'renamed', before.revision)).data.renamed, false);
    assert.deepEqual(await snapshot(), before);

    // Reason codes are the public kernel enum, not a second host-side rule set.
    for (const [name, reason] of [
        ['', 1], ['a'.repeat(64), 2], ['_bad', 3], ['1bad', 3], ['bad-name', 4],
        ['with space', 4], ['a\0b', 4], ['a\u4e2d', 4], ['end', 5], ['classdef', 5],
        ["bad; occupied=0;", 4],
    ]) {
        const rejected = (await rename('renamed', name, before.revision, 400)).data.error;
        assert.equal(rejected.code, 'invalid_variable_name');
        assert.equal(rejected.name_status, reason);
        assert.equal(rejected.max_length, 63);
        assert.deepEqual(await snapshot(), before, 'invalid name changes nothing');
    }
    for (const name of ['a'.repeat(63), 'arguments', 'properties', 'methods', 'events', 'enumeration', 'import', 'plot', 'Renamed', 'Occupied']) {
        const revision = (await snapshot()).revision;
        await rename('renamed', name, revision);
        assert.deepEqual(await value(name), original);
        await rename(name, 'renamed', (await snapshot()).revision);
    }
    assert.equal((await value('occupied')).data[0], 99, 'case-distinct existing binding is untouched');
    before = await snapshot();
    for (const revision of [null, 3, '', '-1', '1x', '18446744073709551616']) {
        assert.equal((await rename('renamed', 'other', revision, 400)).data.error.code, 'invalid_revision');
    }
    for (const body of [{}, null, [], {new_name: null, expected_revision: before.revision},
        {new_name: 'other'}, {new_name: 5, expected_revision: before.revision}]) {
        await request(`${base}/workspace/renamed/rename`, 'POST', body, 400);
    }
    await request(`${base}/workspace/renamed/rename`, 'GET', undefined, 405);
    assert.deepEqual(await snapshot(), before);

    const functionValue = await value('fn');
    await rename('fn', 'functionAlias', before.revision);
    assert.deepEqual(await value('functionAlias'), functionValue);
    await execute({command: "offset=100; if functionAlias(5)~=12; error('closure capture lost'); end;"});
    const graphicsValue = await value('line');
    await rename('line', 'renamedLine', (await snapshot()).revision);
    assert.deepEqual(await value('renamedLine'), graphicsValue);
    await execute({command: "renamedLine.Color=[1,0,0]; if ~isequal(lineAlias.Color,[1,0,0]); error('graphics alias lost'); end;"});
    const graphics = (await value('renamedLine')).graphics;
    assert.deepEqual(graphics, (await value('lineAlias')).graphics, 'graphics alias identity retained');
    await execute({source: [
        'classdef RenameRecord < dynamicprops',
        'properties', 'Backing = 5', 'end', 'end',
        "record=RenameRecord(); recordAlias=record; descriptor=addprop(record,'Score'); record.Score=9;",
    ].join('\n'), source_name: 'rename_record.m'});
    await rename('record', 'renamedRecord', (await snapshot()).revision);
    await rename('descriptor', 'renamedDescriptor', (await snapshot()).revision);
    await execute({command: "renamedRecord.Score=17; if recordAlias.Score~=17 || ~strcmp(renamedDescriptor.Name,'Score') || ~(findprop(renamedRecord,'Score')==renamedDescriptor); error('object identity lost'); end;"});
    await execute({command: "renamedDescriptor.delete(); if isprop(recordAlias,'Score'); error('descriptor identity lost'); end;"});

    const numeric = number => ({kind: 'numeric', class: 'double', dimensions: [1, 1], data: [number]});
    await request(`${base}/workspace/race`, 'PUT', numeric(11));
    before = await snapshot();
    const racers = await Promise.all(['left', 'right'].map(name => rename('race', name, before.revision, null)));
    assert.deepEqual(racers.map(item => item.status).sort(), [200, 409]);
    const winner = racers.find(item => item.status === 200).data.new_name;
    assert.equal((await value(winner)).data[0], 11);
    assert.equal(BigInt((await snapshot()).revision), BigInt(before.revision) + 1n);
    await request(`${base}/workspace/race`, 'GET', undefined, 404);
    await request(`${base}/workspace/race`, 'PUT', numeric(21));
    before = await snapshot();
    const mixed = await Promise.all([
        rename('race', 'mixedWinner', before.revision, null),
        request(`${base}/workspace/race?expected_revision=${before.revision}`, 'PUT', numeric(22), null),
    ]);
    assert.deepEqual(mixed.map(item => item.status).sort(), [200, 409]);
    assert.equal(BigInt((await snapshot()).revision), BigInt(before.revision) + 1n);
    if (mixed[0].status === 200) {
        assert.equal((await value('mixedWinner')).data[0], 21);
        await request(`${base}/workspace/race`, 'GET', undefined, 404);
    } else {
        assert.equal((await value('race')).data[0], 22);
        await request(`${base}/workspace/mixedWinner`, 'GET', undefined, 404);
    }
    before = await snapshot();
    await request(`${base}/reset`, 'POST', {});
    assert.equal((await rename('renamed', 'afterReset', before.revision, 409)).data.error.code, 'workspace_conflict');
    assert.equal((await snapshot()).variables.length, 0);
    console.log(`PASS rename ON: kernel validation, no execution, values/aliases/closures, conflicts, races, reset; evidence ${root}`);
}

main().catch(error => { console.error(error); process.exitCode = 1; }).finally(async () => {
    if (server?.pid && server.exitCode === null) {
        const stopped = new Promise(resolve => server.once('close', resolve));
        server.kill();
        await stopped;
    }
});
