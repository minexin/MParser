'use strict';
const assert = require('node:assert/strict');
const path = require('node:path');
require(path.resolve(process.argv[2], 'tests/graphics_snapshot.test.js'));
for (const version of [2, 3]) {
  const graph = {schema:'mparser.graphics',version,graphIdentity:'17',objects:[
    {id:1,type:'Figure',children:[2],properties:{}},
    {id:2,type:'Axes',parent:1,children:[3,4,5],labels:{Title:3,XLabel:4,YLabel:5},properties:{}},
    {id:3,type:'Text',parent:2,properties:{String:'Gain'}},
    {id:4,type:'Text',parent:2,properties:{String:'Time'}},
    {id:5,type:'Text',parent:2,properties:{String:'Power'}}
  ]};
  assert.equal(MExec.plot.renderGraphicsSnapshot(graph), true);
  assert.equal(MExec.plot.state.title, 'Gain');
  assert.equal(MExec.plot.state.xlabel, 'Time');
  assert.equal(MExec.plot.state.ylabel, 'Power');
  graph.objects[2].properties.String = 'Changed';
  graph.objects[3].properties.Visible = 'off';
  graph.objects[4].parent = 999;
  MExec.plot.renderGraphicsSnapshot(graph);
  assert.equal(MExec.plot.state.title, 'Changed');
  assert.equal(MExec.plot.state.xlabel, '');
  assert.equal(MExec.plot.state.ylabel, '');
}
console.log('PASS v2/v3 Text labels: mutation, visibility and parent binding');
