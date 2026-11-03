'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const calls = [];
const context = new Proxy({}, {get: (o,k) => o[k] || ((...a) => calls.push([k,...a]))});
context.stroke = () => calls.push(['stroke', context.strokeStyle]);
const canvas = {clientWidth:600,clientHeight:400,getContext:()=>context};
const sandbox = {
  MExec:{value:{formatNumber:String}}, addEventListener(){},
  document:{querySelector:s=>s === '#plot-canvas' ? canvas : null}
};
sandbox.window = sandbox;
vm.runInNewContext(fs.readFileSync(path.resolve(process.argv[2],'web/js/plot.js'),'utf8'), sandbox);
const graph = {schema:'mparser.graphics',version:3,graphIdentity:'8',objects:[
  {id:1,type:'Figure',children:[2],properties:{}},
  {id:2,type:'Axes',parent:1,children:[3,4],properties:{XLim:[0,1],YLim:[0,1]}},
  {id:3,type:'Line',parent:2,properties:{XData:[-1,2],YData:[-1,2],Clipping:'off'}},
  {id:4,type:'Line',parent:2,properties:{XData:[-1,2],YData:[2,-1],Clipping:'on'}}
]};
const original = JSON.stringify(graph);
function render(expectedClips) {
  calls.length = 0;
  assert.equal(sandbox.MExec.plot.renderGraphicsSnapshot(graph),true);
  assert.equal(calls.filter(c=>c[0]==='clip').length,expectedClips);
  let depth = 0;
  for (const [op] of calls) {
    if(op==='save') depth++;
    if(op==='restore') depth--;
    assert.ok(depth>=0);
  }
  assert.equal(depth,0);
}
render(1);
assert.equal(JSON.stringify(graph),original);
assert.equal(sandbox.MExec.plot.state.series[0].style.clipping,false);
assert.equal(sandbox.MExec.plot.state.series[1].style.clipping,true);
graph.objects[3].properties.Clipping='off';
render(0);
delete graph.objects[2].properties.Clipping;
render(1);
graph.objects[2].properties.LineStyle='none';
render(0);
console.log('PASS per-Line clipping: mixed modes, default, hidden strokes, balanced canvas state');
function grid(x, y) {
  graph.objects[1].properties.XGrid = x;
  graph.objects[1].properties.YGrid = y;
  calls.length = 0;
  sandbox.MExec.plot.renderGraphicsSnapshot(graph);
  assert.equal(sandbox.MExec.plot.state.xgrid, x === 'on');
  assert.equal(sandbox.MExec.plot.state.ygrid, y === 'on');
  return calls.filter(c=>c[0]==='stroke' && c[1]==='#33373d').length;
}
assert.equal(grid('off','off'), 0);
const xStrokes = grid('on','off');
const yStrokes = grid('off','on');
assert.ok(xStrokes > 0 && yStrokes > 0);
assert.equal(grid('on','on'), xStrokes + yStrokes);
assert.equal(grid('off','off'), 0);
console.log('PASS independent X/Y grid rendering and toggles');
