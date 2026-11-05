'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs'), path = require('node:path'), vm = require('node:vm');
const root = path.resolve(process.argv[2]);
const calls = [], listeners = new Map(), nodes = new Map();
const context = new Proxy({}, {get:(object,key) => object[key] || ((...args) => {
  calls.push([key,...args]);
  if (key === 'measureText') return {width:20};
})});
const canvas = {clientWidth:1000,clientHeight:600,isConnected:true,style:{},
  getContext:() => context,getClientRects:() => [{}],getBoundingClientRect:() => ({left:0,top:0}),
  addEventListener:(name,listener) => listeners.set(name,listener)};
const node = () => ({children:[],style:{},replaceChildren(){this.children=[];},
  appendChild(item){this.children.push(item);},addEventListener(){},setAttribute(){},click(){},
  remove(){this.removed=true;},getBoundingClientRect(){return this.style.font ? {width:65,height:18} : {width:96,height:1};}});
for (const id of ['plot-figure','plot-axes','plot-export']) nodes.set(id,node());
const probes = [];
const sandbox = {MExec:{value:{formatNumber:String}},devicePixelRatio:2,addEventListener(){},
  getComputedStyle:() => ({visibility:'visible',font:'12px TestFont',paddingLeft:'0px',paddingRight:'0px',paddingTop:'0px',paddingBottom:'0px'}),
  document:{querySelector:selector => selector === '#plot-canvas' ? canvas : nodes.get(selector.slice(1)),
    getElementById:() => null,createElement:node,fonts:{status:'loaded'},body:{appendChild:item => probes.push(item)}},
};
sandbox.window = sandbox;
for (const file of ['plot-layout.js','plot-view.js','plot.js']) {
  vm.runInNewContext(fs.readFileSync(path.join(root,'web/js',file),'utf8'),sandbox);
}
const {plotLayout:layout,plot,plotView:view} = sandbox.MExec;
const plain = value => JSON.parse(JSON.stringify(value));
const environment = {width_pixels:1000,height_pixels:600,pixels_per_inch:96,character_width_pixels:6.5,character_height_pixels:18};
const profile = {revision:'1',contentSizePixels:[1000,600],pixelsPerInch:96,characterSizePixels:[6.5,18]};
const normalized = {Position:[0.1,0.2,0.3,0.5],Units:'normalized'};
const expected = {left:100,top:180,width:300,height:300};
for (const [Units,Position] of [
  ['normalized',[0.1,0.2,0.3,0.5]],['pixels',[101,121,300,300]],
  ['inches',[100/96,120/96,300/96,300/96]],
  ['centimeters',[100*2.54/96,120*2.54/96,300*2.54/96,300*2.54/96]],
  ['points',[75,90,225,225]],['characters',[100/6.5,120/18,300/6.5,300/18]],
]) {
  const actual = layout.frame({Units,Position},profile,1000,600);
  for (const field of Object.keys(expected)) assert.ok(Math.abs(actual[field]-expected[field]) < 1e-9,`${Units}.${field}`);
}
assert.deepEqual(plain(layout.frame(normalized,null,1000,600)),expected);
for (const Units of ['pixels','inches','centimeters','points','characters','unknown'])
  assert.equal(layout.frame({Units,Position:[1,1,20,20]},null,1000,600),null,'no invented environment');
for (const Position of [[0,0,-1,1],[0,0,NaN,1],[0,0,1],[0,0,1,Infinity]])
  assert.equal(layout.frame({Position,Units:'normalized'},profile,1000,600),null);
assert.equal(layout.frame(normalized,profile,0,600),null);
assert.deepEqual(plain(layout.frame({Position:[-0.1,0.8,0.2,0.4],Units:'normalized'},null,1000,600)),
  {left:-100,top:-120,width:200,height:240},'outside origins remain outside');
assert.deepEqual(plain(layout.measure(canvas)),environment);
assert.ok(probes.every(item => item.removed),'measurement probes are removed');
canvas.clientWidth=0; assert.equal(layout.measure(canvas),null); canvas.clientWidth=1000;
canvas.isConnected=false; assert.equal(layout.measure(canvas),null); canvas.isConnected=true;
sandbox.document.fonts.status='loading'; assert.equal(layout.measure(canvas),null); sandbox.document.fonts.status='loaded';

function graph() {
  return {schema:'mparser.graphics',version:4,graphIdentity:'9007199254740993',objects:[
    {id:1,type:'Figure',objectIdentity:'9007199254740995',children:[2,4],properties:{},layout:profile},
    {id:2,type:'Axes',objectIdentity:'9007199254740997',parent:1,children:[3],properties:{...normalized,XLim:[0,1],YLim:[0,1]}},
    {id:3,type:'Line',objectIdentity:'9007199254740999',parent:2,properties:{XData:[0,1],YData:[0,1],Color:[1,0,0]}},
    {id:4,type:'Axes',objectIdentity:'9007199254741001',parent:1,children:[5],properties:{Position:[0.6,0.1,0.3,0.3],Units:'normalized',XLim:[0,1],YLim:[0,1]}},
    {id:5,type:'Line',objectIdentity:'9007199254741003',parent:4,properties:{XData:[0,1],YData:[1,0],Color:[0,0,1],Clipping:'off'}},
  ]};
}
const scene = graph(), before = JSON.stringify(scene);
assert.equal(plot.renderGraphicsSnapshot(scene),true);
assert.equal(canvas.width,2000); assert.equal(canvas.height,1200,'DPR affects only backing store');
assert.ok(calls.some(call => call[0] === 'rect' && JSON.stringify(call.slice(1)) === '[100,180,300,300]'));
assert.ok(calls.some(call => call[0] === 'moveTo' && call[1] === 600 && call[2] === 360),'second Axes draws in its own rectangle');
assert.deepEqual(plain(plot.layoutTarget()),{graph_identity:scene.graphIdentity,figure_identity:scene.objects[0].objectIdentity});
assert.equal(view.zoom(0.5),true); calls.length=0; plot.draw();
assert.ok(calls.some(call => call[0] === 'moveTo' && call[1] === 600 && call[2] === 360),'zooming selected Axes leaves neighbor unchanged');
const updated = graph(); updated.objects[0].layout={...profile,revision:'2'};
calls.length=0; plot.renderGraphicsSnapshot(updated,false,false);
assert.ok(calls.some(call => call[0] === 'moveTo' && call[1] === -50),'environment-only refresh preserves selected zoom');
const oldSelection = nodes.get('plot-axes').onchange;
nodes.get('plot-axes').value='4'; nodes.get('plot-axes').onchange();
assert.equal(plot.state.series[0].style.color,'rgb(0, 0, 255)');
assert.equal(view.zoom(0.5),true);
const replacement = graph(); replacement.graphIdentity='9007199254741007';
plot.renderGraphicsSnapshot(replacement); nodes.get('plot-axes').value='4'; oldSelection();
assert.equal(plot.state.series[0].style.color,'rgb(255, 0, 0)','detached callback cannot select in another graph');
assert.equal(JSON.stringify(scene),before,'rendering never mutates kernel snapshots');
canvas.clientWidth=0;
assert.equal(plot.exportPng(),false,'hidden v4 canvas must not export stale backing pixels');
canvas.clientWidth=1000;
const unavailable=graph();unavailable.objects[0].layout=null;unavailable.objects[1].properties.Units='pixels';
plot.renderGraphicsSnapshot(unavailable);
assert.equal(plot.exportPng(),false,'incomplete environment-dependent layout must not export a partial Figure');

const deferred = () => {let resolve;const promise=new Promise(done=>{resolve=done;});return {promise,resolve};};
async function main() {
  let owner={sessionId:'first',epoch:0,enabled:true}, reads=0, writes=0, commits=0;
  const rendered=[], post=[], initial=graph(); initial.objects[0].layout=null;
  let server={graphics:initial,revision:'9007199254740993'};
  const api={graphicsLayout:async()=>{reads++;return structuredClone(server);},
    graphicsLayoutEnvironment:async(session,body)=>{
      writes++;post.push({session,body});server={graphics:graph(),revision:'9007199254740994'};
      return structuredClone(server);
    }};
  const options={api,current:()=>owner,render:value=>rendered.push(value),
    target:()=>({graph_identity:server.graphics.graphIdentity,figure_identity:server.graphics.objects[0].objectIdentity}),
    measure:()=>environment,onCommit:()=>{commits++;}};
  let sync=layout.controller(options);
  await sync.refresh(); await sync.refresh();
  assert.equal(writes,1);assert.equal(commits,1);assert.equal(reads,2);
  assert.equal(post[0].body.expected_revision,'9007199254740993','revision stays an exact decimal string');
  assert.deepEqual(plain(post[0].body.environment),environment);
  assert.equal(post[0].body.figure_identity,'9007199254740995');
  owner.enabled=false; await sync.refresh(); assert.equal(reads,2,'capability OFF performs no new requests'); owner.enabled=true;
  sync=layout.controller({...options,measure:()=>null}); server={graphics:initial,revision:'4'};
  await sync.refresh();assert.equal(writes,1,'hidden canvas does not post defaults');

  const late=deferred();rendered.length=0;
  sync=layout.controller({...options,api:{...api,graphicsLayout:()=>late.promise}});
  const pending=sync.refresh();owner={...owner,sessionId:'replacement',epoch:1};late.resolve({graphics:initial,revision:'4'});
  await pending;assert.equal(rendered.length,0);assert.equal(writes,1,'late old-session read is discarded');

  let posted=deferred();rendered.length=0;
  sync=layout.controller({...options,api:{...api,graphicsLayoutEnvironment:async()=>{posted.resolve();return latePost.promise;}}});
  const latePost=deferred(), pendingPost=sync.refresh();await posted.promise;
  owner={...owner,epoch:2};latePost.resolve(server);await pendingPost;
  assert.equal(rendered.length,1,'late post result is discarded after execution/reset epoch changes');

  let recovered=0; const unknown={...options,api:{...api,graphicsLayout:async()=>{recovered++;return structuredClone(server);},
    graphicsLayoutEnvironment:async()=>{server={graphics:graph(),revision:'5'};throw new Error('response lost');}}};
  sync=layout.controller(unknown);await assert.rejects(sync.refresh(),/response lost/);
  assert.equal(recovered,2,'uncertain mutation is followed by a fresh read');
  await sync.refresh();assert.equal(recovered,3,'committed metrics are recognized without another write');

  server={graphics:initial,revision:'6'};let conflictWrites=0;
  sync=layout.controller({...options,api:{...api,graphicsLayoutEnvironment:async()=>{conflictWrites++;const error=new Error('busy');error.status=409;throw error;}}});
  await sync.refresh();assert.equal(conflictWrites,1,'busy/conflict recovery does not spin or blindly replay');
  const gate=deferred();let queuedReads=0;
  sync=layout.controller({...options,measure:()=>null,api:{...api,graphicsLayout:async()=>{queuedReads++;if(queuedReads===1)await gate.promise;return server;}}});
  const first=sync.refresh(), second=sync.refresh(), third=sync.refresh();gate.resolve();await Promise.all([first,second,third]);
  assert.equal(queuedReads,2,'concurrent resize refreshes are coalesced');
  console.log('PASS v4 layout candidate: six units, explicit measurements, multi-Axes/DPR, independent zoom, immutable scenes, exact revisions, stale sessions/epochs, lost response and bounded conflict recovery (simulated Canvas only)');
}
main().catch(error=>{console.error(error);process.exitCode=1;});
