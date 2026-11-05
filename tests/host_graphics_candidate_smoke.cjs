'use strict';
const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const fs = require('node:fs');
const path = require('node:path');
const {spawnSync} = require('node:child_process');

assert.ok(Number(process.versions.node.split('.')[0]) >= 18, 'Node.js 18 or newer is required');
const repo = path.resolve(__dirname, '..');
const fixture = path.join(__dirname, 'host_graphics_fixture');
const output = path.resolve(process.argv[2] || path.join(repo, 'build/host-graphics-candidate-smoke'));
fs.mkdirSync(output, {recursive:true});
const stage = fs.mkdtempSync(path.join(output, 'run-'));
const git = process.argv[3] || 'git';
function run(command, args) {
  const result = spawnSync(command, args, {encoding:'utf8', timeout:30_000, maxBuffer:2*1024*1024});
  if (result.stdout) process.stdout.write(result.stdout);
  if (result.stderr) process.stderr.write(result.stderr);
  if (result.error) throw result.error;
  assert.equal(result.status, 0, `Command failed: ${command} ${args.join(' ')}; evidence: ${stage}`);
  return result;
}
const manifest = JSON.parse(fs.readFileSync(path.join(fixture, 'manifest.json'), 'utf8'));
for (const artifact of manifest.artifacts) {
  const source = path.join(fixture, artifact.path);
  const text = fs.readFileSync(source, 'utf8').replace(/\r\n?/g, '\n');
  assert.equal(crypto.createHash('sha256').update(text).digest('hex'), artifact.sha256_lf,
    `Frozen client fixture changed: ${artifact.path}`);
  const target = path.join(stage, artifact.path);
  fs.mkdirSync(path.dirname(target), {recursive:true});
  fs.writeFileSync(target, text, {flag:'wx'});
}
run(git, ['-C',stage,'init','-q']);
const integration = path.join(repo,'tools/host-integration');
for (const patch of ['mexecclient-text-labels-candidate.patch','mexecclient-line-clipping-candidate.patch',
  'mexecclient-grid-candidate.patch','mexecclient-legend-candidate.patch','mexecclient-color-candidate.patch',
  'mexecclient-runtime-identity-candidate.patch','mexecclient-axes-layout-candidate.patch',
  'mexecclient-layout-sync-candidate.patch','mexecclient-legend-references-candidate.patch',
  'mexecclient-compact-layout-candidate.patch','mexecclient-ordered-events-candidate.patch']) {
  run(git, ['-C',stage,'apply','--unidiff-zero','--ignore-space-change',path.join(integration,patch)]);
}
for (const test of ['test-client-text-labels.cjs','test-client-clipping.cjs','test-client-legend.cjs','test-client-colors.cjs',
  'test-client-axes-layout.cjs','test-client-layout-bootstrap.cjs','test-client-runtime-identity.cjs',
  'test-client-compact-layout.cjs','test-client-compact-canvas.cjs','test-client-ordered-events.cjs']) {
  run(process.execPath, [path.join(integration,test),stage]);
}
if (process.argv[4]) {
  const result = run(process.argv[4], ['--run','--jit=off','--result-format=json-v1',
    path.join(__dirname,'host_graphics_scene.m')]);
  const resultPath = path.join(stage,'kernel-result.json');
  fs.writeFileSync(resultPath,result.stdout,{flag:'wx'});
  run(process.execPath,[path.join(integration,'test-client-kernel-scene.cjs'),stage,resultPath]);
}
console.log(`PASS frozen-client candidate replay; simulated canvas only; evidence: ${stage}`);
