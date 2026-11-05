'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');

const calls = [];
const context = new Proxy({
  measureText: text => ({width: String(text).length * 8}),
  stroke() { calls.push(['stroke', this.strokeStyle, this.lineWidth]); },
  fillText(...args) { calls.push(['fillText', ...args]); }
}, {get: (object, key) => object[key] || ((...args) => calls.push([key, ...args]))});
const canvas = {clientWidth:600, clientHeight:400, getContext:()=>context};
const sandbox = {MExec:{value:{formatNumber:String}}, addEventListener(){},
  document:{querySelector:selector=>selector === '#plot-canvas' ? canvas : null}};
sandbox.window = sandbox;
vm.runInNewContext(fs.readFileSync(path.resolve(process.argv[2], 'web/js/plot.js'), 'utf8'), sandbox);

const base = version => ({schema:'mparser.graphics', version, graphIdentity:'17', objects:[
  {id:1,type:'Figure',children:[2,5],properties:{}},
  {id:2,type:'Axes',parent:1,children:[3,4],properties:{XLim:[0,1],YLim:[0,1]}},
  {id:3,type:'Line',parent:2,properties:{XData:[0,1],YData:[0,1],DisplayName:'Do not infer',Color:[1,0,0]}},
  {id:4,type:'Line',parent:2,properties:{XData:[0,1],YData:[1,0],Color:[0,1,0],LineStyle:'--'}}
]});
function render(graph, expectedLabels) {
  const original = JSON.stringify(graph);
  calls.length = 0;
  assert.equal(sandbox.MExec.plot.renderGraphicsSnapshot(graph), true);
  const labels = calls.filter(call=>call[0] === 'fillText' && String(call[1]).startsWith('Legend '));
  assert.deepEqual(labels.map(call=>call[1]), expectedLabels);
  assert.equal(JSON.stringify(graph), original);
  let depth = 0;
  for (const [operation, ...args] of calls) {
    if (operation === 'save') depth++;
    if (operation === 'restore') depth--;
    assert.ok(depth >= 0);
    for (const value of args) if (typeof value === 'number') assert.ok(Number.isFinite(value));
  }
  assert.equal(depth, 0);
  return labels;
}
for (const version of [2,3]) {
  const graph = base(version);
  render(graph, []);
  assert.equal(sandbox.MExec.plot.state.legend, null);
  const legend = {id:5,type:'Legend',parent:1,axes:2,children:[],
    properties:{Visible:'on',Location:'northeast',Orientation:'vertical'},
    entries:[{line:4,label:'Legend second'},{line:3,label:'Legend first'}]};
  graph.objects.push(legend);
  const labels = render(graph, ['Legend second','Legend first']);
  assert.ok(labels[0][3] < labels[1][3]);
  assert.equal(sandbox.MExec.plot.state.legend.entries[0].color, 'rgb(0, 255, 0)');
  assert.equal(sandbox.MExec.plot.state.legend.entries[1].color, 'rgb(255, 0, 0)');
  legend.entries[0].label = 'Legend renamed';
  render(graph, ['Legend renamed','Legend first']);
  legend.properties.Visible = 'off';
  render(graph, []);
  legend.properties.Visible = 'on';
  legend.axes = 99;
  render(graph, []);
  legend.axes = 2;
  legend.parent = 99;
  render(graph, []);
  legend.parent = 1;
  legend.entries.unshift({line:999,label:'Legend dangling'});
  render(graph, ['Legend renamed','Legend first']);
  graph.objects = graph.objects.filter(object=>object.id !== 4);
  graph.objects[1].children = [3];
  render(graph, ['Legend first']);
  legend.properties.Orientation = 'horizontal';
  legend.entries = [{line:3,label:'Legend one'},{line:3,label:'Legend two'}];
  const horizontal = render(graph, ['Legend one','Legend two']);
  assert.equal(horizontal[0][3], horizontal[1][3]);
  assert.ok(horizontal[0][2] < horizontal[1][2]);
  for (const location of ['north','south','east','west','northeast','northwest','southeast','southwest',
    'northoutside','southoutside','eastoutside','westoutside','northeastoutside',
    'northwestoutside','southeastoutside','southwestoutside','best','bestoutside']) {
    legend.properties.Location = location;
    render(graph, ['Legend one','Legend two']);
  }
  canvas.clientWidth = 320;
  canvas.clientHeight = 240;
  legend.properties.Location = 'eastoutside';
  legend.entries = [{line:3,label:'Legend ' + 'Long label '.repeat(40)}];
  render(graph, [legend.entries[0].label]);
  assert.ok(calls.some(call=>call[0] === 'stroke' && call[1] === 'rgb(255, 0, 0)'));
  const labelCall = calls.find(call=>call[0] === 'fillText' && call[1] === legend.entries[0].label);
  assert.ok(labelCall[2] >= 0 && labelCall[2] + labelCall[4] <= canvas.clientWidth);
  canvas.clientWidth = 600;
  canvas.clientHeight = 400;
  graph.objects.push({id:6,type:'Figure',properties:{Visible:'off'},children:[7]},
    {id:7,type:'Axes',parent:6,children:[8],properties:{}},
    {id:8,type:'Line',parent:7,properties:{XData:[0,1],YData:[1,0],Color:[0,0,1]}});
  legend.entries=[{line:8,label:'Legend external'},{line:3,label:'Legend local'},{line:8,label:'Legend repeated'}];
  render(graph,['Legend external','Legend local','Legend repeated']);
  assert.equal(sandbox.MExec.plot.state.series.length,1,'external Legend reference does not draw a foreign Axes curve');
  assert.equal(sandbox.MExec.plot.state.legend.entries[0].color,'rgb(0, 0, 255)');
  graph.objects = graph.objects.filter(object=>object.id !== 5);
  render(graph, []);
  sandbox.MExec.plot.clear();
  assert.equal(sandbox.MExec.plot.state.legend, null);
}
console.log('PASS Legend v2/v3: explicit object, entry order/style, rename, hide, removal, references and layout');
