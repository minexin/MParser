'use strict';
const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path'),vm=require('node:vm');
const root=path.resolve(process.argv[2]),calls=[];
const context=new Proxy({measureText:text=>({width:String(text).length*7})},
  {get:(object,key)=>object[key]||((...args)=>calls.push([key,...args]))});
const canvas={clientWidth:390,clientHeight:300,getContext:()=>context};
const sandbox={MExec:{value:{formatNumber:String}},devicePixelRatio:2,addEventListener(){},
  document:{querySelector:selector=>selector==='#plot-canvas'?canvas:null}};sandbox.window=sandbox;
for(const file of ['plot-layout.js','plot.js'])vm.runInNewContext(fs.readFileSync(path.join(root,'web/js',file),'utf8'),sandbox);
const scene={schema:'mparser.graphics',version:4,graphIdentity:'11',objects:[
  {id:1,type:'Figure',objectIdentity:'1',children:[2,4,6],properties:{},layout:null},
  {id:2,type:'Axes',objectIdentity:'2',parent:1,children:[3],properties:{Position:[0.1,0.18,0.35,0.68],Units:'normalized',XLim:[0,1],YLim:[0,1]}},
  {id:3,type:'Line',objectIdentity:'3',parent:2,properties:{XData:[0,1],YData:[0,1],Color:[1,0,0]}},
  {id:4,type:'Axes',objectIdentity:'4',parent:1,children:[5],properties:{Position:[0.6,0.18,0.3,0.68],Units:'normalized',XLim:[0,1],YLim:[0,1]}},
  {id:5,type:'Line',objectIdentity:'5',parent:4,properties:{XData:[0,1],YData:[1,0],Color:[0,1,0]}},
  {id:6,type:'Legend',objectIdentity:'6',parent:1,axes:2,properties:{Location:'southwest',Orientation:'horizontal',Visible:'on'},
    entries:[{line:5,label:'Remote Again'},{line:3,label:'Red'},{line:5,label:'Remote Again'}]},
]};
const before=JSON.stringify(scene);
assert.equal(sandbox.MExec.plot.renderGraphicsSnapshot(scene),true);
const labels=calls.filter(call=>call[0]==='fillText');
const numeric=labels.filter(call=>Number.isFinite(Number(call[1])));
assert.ok(numeric.length<20,'small Axes reduce tick density instead of overwriting labels');
assert.deepEqual(labels.filter(call=>String(call[1]).startsWith('Remote')||call[1]==='Red').map(call=>call[1]),
  ['Remote Again','Red','Remote Again']);
const legendBox=calls.filter(call=>call[0]==='fillRect').at(-1);
assert.ok(legendBox[3]<=390*0.35-16,'inside Legend wraps within its explicit Axes width');
assert.ok(legendBox[1]>=39 && legendBox[1]+legendBox[3]<=390*0.45,'Legend no longer spans the neighboring Axes');
let depth=0;
for(const [operation,...values]of calls){if(operation==='save')depth++;if(operation==='restore')depth--;assert.ok(depth>=0);
  for(const value of values)if(typeof value==='number')assert.ok(Number.isFinite(value));}
assert.equal(depth,0);assert.equal(JSON.stringify(scene),before,'tick/Legend presentation does not mutate kernel properties or data');
assert.equal(canvas.width,780);assert.equal(canvas.height,600);
canvas.clientWidth=838;canvas.clientHeight=84;calls.length=0;
scene.objects.at(-1).entries.splice(1,0,{line:3,label:'Blue'});
sandbox.MExec.plot.renderGraphicsSnapshot(scene);
const shortBox=calls.filter(call=>call[0]==='fillRect').at(-1);
const shortLabels=calls.filter(call=>call[0]==='fillText'&&['Remote Again','Blue','Red'].includes(call[1]));
assert.equal(shortLabels.length,4);
assert.ok(shortLabels.every(call=>call[3]<=shortBox[2]+shortBox[4]-2),'two-row Legend labels remain inside the short landscape box');
console.log('PASS compact Canvas: bounded ticks and Axes-local Legend wrapping, repeated/foreign keys, DPR, balanced state and unchanged kernel snapshot (simulated Canvas only)');
