'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const calls = [];
const context = new Proxy({
    measureText: text => ({width:String(text).length * 8}),
    stroke() { calls.push(['stroke',this.strokeStyle]); },
    fillText(text) { calls.push(['text',text,this.fillStyle]); },
}, {get:(target,key) => target[key] || (() => {})});
const canvas = {clientWidth:600,clientHeight:400,getContext:()=>context};
const sandbox = {MExec:{value:{formatNumber:String}},addEventListener(){},
    document:{querySelector:selector => selector === '#plot-canvas' ? canvas : null}};
sandbox.window = sandbox;
vm.runInNewContext(fs.readFileSync(path.resolve(process.argv[2],'web/js/plot.js'),'utf8'),sandbox);
for (const version of [2,3]) {
    const graph = {schema:'mparser.graphics',version,graphIdentity:'19',objects:[
        {id:1,type:'Figure',children:[2,7],properties:{}},
        {id:2,type:'Axes',parent:1,children:[3,4,5],labels:{Title:6},properties:{XLim:[0,1],YLim:[0,1]}},
        {id:3,type:'Line',parent:2,properties:{XData:[0,1],YData:[0,1],Color:[1,0,0]}},
        {id:4,type:'Line',parent:2,properties:{XData:[0,1],YData:[1,0],Color:[0,0,1,0.5]}},
        {id:5,type:'Line',parent:2,properties:{XData:[0,1],YData:[0.5,0.5],Color:'none'}},
        {id:6,type:'Text',parent:2,properties:{String:'Color title',Color:[0,1,0]}},
        {id:7,type:'Legend',parent:1,axes:2,entries:[
            {line:3,label:'Legend red'},{line:4,label:'Legend alpha'},{line:5,label:'Legend hidden'}],properties:{}},
    ]};
    const render = () => {
        const before = JSON.stringify(graph);
        calls.length = 0;
        assert.equal(sandbox.MExec.plot.renderGraphicsSnapshot(graph),true);
        assert.equal(JSON.stringify(graph),before);
    };
    render();
    assert.equal(calls.filter(item=>item[0]==='stroke' && item[1]==='rgb(255, 0, 0)').length,2);
    assert.equal(calls.filter(item=>item[0]==='stroke' && item[1]==='rgba(0, 0, 255, 0.5)').length,2);
    assert.ok(calls.some(item=>item[0]==='text' && item[1]==='Color title' && item[2]==='rgb(0, 255, 0)'));
    assert.ok(calls.some(item=>item[0]==='text' && item[1]==='Legend hidden'));
    assert.ok(!calls.some(item=>item[0]==='stroke' && item[1]==='none'));
    assert.equal(sandbox.MExec.plot.state.series[2].style.hidden,true);
    graph.objects[2].properties.Color = 'none';
    graph.objects[3].properties.Color = [0,0,1,0];
    graph.objects[5].properties.Color = 'none';
    render();
    assert.ok(!calls.some(item=>item[0]==='text' && item[1]==='Color title'));
    assert.equal(sandbox.MExec.plot.state.title,'Color title');
    assert.ok(!calls.some(item=>item[0]==='stroke' && item[1]==='rgb(255, 0, 0)'));
    assert.ok(calls.some(item=>item[0]==='stroke' && item[1]==='rgba(0, 0, 255, 0)'));
    assert.ok(calls.some(item=>item[0]==='text' && item[1]==='Legend red'));
    graph.objects[5].properties.Color = [1,0,0];
    render();
    assert.ok(calls.some(item=>item[0]==='text' && item[1]==='Color title' && item[2]==='rgb(255, 0, 0)'));
    sandbox.MExec.plot.clear();
    assert.equal(Object.keys(sandbox.MExec.plot.state.labelColors).length,0);
}
console.log('PASS Color rendering candidate: RGB/RGBA Line and legend keys, none, Text colors and restoration');
