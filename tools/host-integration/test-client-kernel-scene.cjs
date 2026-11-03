'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const result = JSON.parse(fs.readFileSync(process.argv[3], 'utf8'));
assert.equal(result.status, 'succeeded');
assert.equal(result.graphics.schema, 'mparser.graphics');
assert.equal(result.graphics.version, 2);
const calls = [];
const context = new Proxy({measureText:text=>({width:String(text).length*8})}, {
  get:(object,key)=>object[key] || ((...args)=>calls.push([key,...args]))
});
const canvas = {clientWidth:600,clientHeight:400,getContext:()=>context};
const sandbox = {MExec:{value:{formatNumber:String}},addEventListener(){},
  document:{querySelector:selector=>selector === '#plot-canvas' ? canvas : null}};
sandbox.window = sandbox;
vm.runInNewContext(fs.readFileSync(path.resolve(process.argv[2],'web/js/plot.js'),'utf8'),sandbox);
const original = JSON.stringify(result.graphics);
assert.equal(sandbox.MExec.plot.renderGraphicsSnapshot(result.graphics),true);
const state = sandbox.MExec.plot.state;
assert.equal(state.title,'Gain');
assert.equal(state.xlabel,'Time');
assert.equal(state.ylabel,'Power');
assert.equal(state.xgrid,true);
assert.equal(state.ygrid,true);
assert.deepEqual(Array.from(state.ylim),[0,5]);
assert.equal(state.series.length,2);
assert.deepEqual(Array.from(state.series[0].y),[1,2,3]);
assert.deepEqual(Array.from(state.series[1].y),[10,20,30]);
assert.equal(state.series[0].style.clipping,false);
assert.equal(state.series[1].style.clipping,true);
assert.equal(state.series[0].style.color,'rgba(255, 0, 0, 0.5)');
assert.equal(state.series[1].style.hidden,true);
assert.equal(state.labelColors.Title,'rgb(0, 255, 0)');
assert.equal(state.legend.entries[0].color,'rgba(255, 0, 0, 0.5)');
assert.equal(state.legend.entries[1].hidden,true);
assert.deepEqual(Array.from(state.legend.entries,entry=>entry.label),['Measured','data2']);
for (const label of ['Gain','Time','Power','Measured','data2']) {
  assert.ok(calls.some(call=>call[0]==='fillText' && call[1]===label),`Missing rendered text: ${label}`);
}
assert.equal(JSON.stringify(result.graphics),original);
console.log('PASS real kernel v2 snapshot: matrix data, Text colors/labels, RGBA/none, grids, Line clipping and Legend');
