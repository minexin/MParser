'use strict';
const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path'),vm=require('node:vm');
const root=path.resolve(process.argv[2]),elements=new Map(),storage=new Map(),windowEvents=new Map();
let active=null,serial=0,draws=0;
function make(id){
  const classes=new Set(),events=new Map();
  const element={id,dataset:{},attrs:{},children:[],inert:false,hidden:false,value:'',
    style:{values:{},setProperty(name,value){this.values[name]=value;}},
    classList:{add(name){classes.add(name);},contains:name=>classes.has(name),toggle(name,on){if(on)classes.add(name);else classes.delete(name);}},
    setAttribute(name,value){this.attrs[name]=value;},
    getAttribute(name){return this.attrs[name]??null;},removeAttribute(name){delete this.attrs[name];},
    appendChild(child){this.children.push(child);child.parent=this;},
    contains(child){for(let next=child;next;next=next.parent)if(next===this)return true;return false;},
    focus(){active=this;},closest:()=>null,
    addEventListener(name,handler){if(!events.has(name))events.set(name,[]);events.get(name).push(handler);},
    fire(name,extra={}){const event={target:this,preventDefault(){this.prevented=true;},...extra};for(const handler of events.get(name)||[])handler(event);return event;},
    querySelector(){const title=get(id+'-title');if(!title.parent)this.appendChild(title);return title;},
    getBoundingClientRect:()=>({left:0,right:1000,bottom:800,width:1000,height:800}),
  };return element;
}
function get(id){if(!elements.has(id))elements.set(id,make(id));return elements.get(id);}
const main=get('main'),navigation=get('compact-panel-tabs');
for(const id of ['files-panel','center-panel','right-panel','command-panel'])main.appendChild(get(id));
const editor=get('editor'),command=get('command-input');editor.value='unsaved = 42;';command.value='pending_command';
get('center-panel').appendChild(editor);get('command-panel').appendChild(command);
const tabs=['workbench','files-panel','right-panel','command-panel'].map((name,index)=>{
  const element=get('compact-'+index);element.dataset.panel=name;navigation.appendChild(element);return element;
});
navigation.querySelectorAll=()=>tabs;
function media(matches){return {matches,handlers:[],addEventListener(name,handler){assert.equal(name,'change');this.handlers.push(handler);},
  change(value){this.matches=value;for(const handler of this.handlers)handler({matches:value});}};}
const compact=media(true),short=media(false);
const previous=JSON.stringify({folder:19,workspace:24,command:35});storage.set('mexec.desktop-layout',previous);
const document={getElementById:get,createElement:()=>make('generated-'+(++serial)),get activeElement(){return active;},
  querySelector:selector=>selector.startsWith('.center-view-tabs')?get('view-plot-tab'):null};
const MExec={plot:{draw(){draws++;}},api:new Proxy({},{get(){throw Error('layout must not call kernel APIs');}})};
vm.runInNewContext(fs.readFileSync(path.join(root,'web/js/layout.js'),'utf8'),{MExec,document,
  localStorage:{getItem:key=>storage.get(key),setItem:(key,value)=>storage.set(key,value)},
  window:{matchMedia:query=>query.includes('max-width: 720px')?compact:short,addEventListener:(name,handler)=>windowEvents.set(name,handler)}});
const concealed=id=>get(id).classList.contains('compact-pane-concealed');
const selected=()=>tabs.find(tab=>tab.attrs['aria-selected']==='true').dataset.panel;
assert.equal(navigation.hidden,false);assert.equal(selected(),'workbench');
assert.equal(concealed('files-panel'),true);assert.equal(get('files-panel').inert,true);
assert.equal(concealed('center-panel'),false);assert.equal(concealed('command-panel'),false);
assert.equal(get('center-panel').attrs.role,'tabpanel');assert.equal(get('command-panel').attrs['aria-labelledby'],tabs[0].id);
command.focus();tabs[2].fire('click');
assert.equal(selected(),'right-panel');assert.equal(get('right-panel').inert,false);assert.equal(get('center-panel').inert,true);
assert.equal(get('right-panel').attrs['aria-labelledby'],tabs[2].id);
assert.equal(active,tabs[2],'concealed input focus moves to the selected panel tab');
MExec.layout.reveal('center-panel');assert.equal(selected(),'workbench');
assert.equal(editor.value,'unsaved = 42;');assert.equal(command.value,'pending_command');
assert.equal(get('center-panel').children.includes(editor),true,'existing editor DOM is retained');
command.focus();short.change(true);assert.equal(concealed('command-panel'),true);assert.equal(active,tabs[0]);
MExec.layout.reveal('command-panel');assert.equal(concealed('command-panel'),false);assert.equal(concealed('center-panel'),true);
assert.equal(selected(),'command-panel');
assert.equal(tabs[3].fire('keydown',{key:'Home'}).prevented,true);assert.equal(selected(),'workbench');
tabs[0].fire('keydown',{key:'ArrowLeft'});assert.equal(selected(),'command-panel');
tabs[3].fire('keydown',{key:'ArrowRight'});assert.equal(selected(),'workbench');
tabs[0].fire('keydown',{key:'End'});assert.equal(selected(),'command-panel');
for(const extra of [{isComposing:true},{keyCode:229},{ctrlKey:true},{altKey:true},{metaKey:true},{shiftKey:true},{defaultPrevented:true}]){
  assert.equal(tabs[3].fire('keydown',{key:'Home',...extra}).prevented,undefined);assert.equal(selected(),'command-panel');
}
assert.equal(tabs.filter(tab=>tab.tabIndex===0).length,1,'one roving tab stop');
compact.change(false);assert.equal(navigation.hidden,true);assert.equal(active,get('view-plot-tab'));
for(const id of ['files-panel','center-panel','right-panel','command-panel'])assert.equal(get(id).inert,false);
assert.equal(get('center-panel').getAttribute('role'),null,'desktop landmark roles are restored');
assert.equal(storage.get('mexec.desktop-layout'),previous,'viewport changes never overwrite desktop proportions');
const centerButton=get('center-panel-title').children[0];centerButton.fire('click');
assert.equal(main.classList.contains('has-maximized-panel'),true);
compact.change(true);assert.equal(main.classList.contains('has-maximized-panel'),false);
centerButton.fire('click');assert.equal(main.classList.contains('has-maximized-panel'),false,'compact layout does not combine with desktop maximization');
get('layout-reset').fire('click');assert.equal(selected(),'workbench');assert.equal(get('right-panel').inert,true);
assert.equal(concealed('command-panel'),true,'short-screen command remains reachable through its own tab');
short.change(false);assert.equal(concealed('command-panel'),false);
assert.equal(editor.value,'unsaved = 42;');assert.equal(command.value,'pending_command');assert.ok(draws>0);
const html=fs.readFileSync(path.join(root,'web/index.html'),'utf8');
assert.match(html,/id="compact-panel-tabs" role="tablist"/);
assert.ok(!html.includes('显示所选坐标区的内核快照'),'obsolete single-Axes helper copy is removed');
const css=fs.readFileSync(path.join(root,'web/css/mexec.css'),'utf8');
assert.match(css,/#main\.compact-layout \{min-width:0;/);
assert.match(css,/#compact-panel-tabs\[hidden\] \{display:none;/);
console.log('PASS compact host layout: panel/short-screen selection, reveal, inert/focus, keyboard/IME, desktop restore, live DOM/drafts, saved proportions and no kernel API calls (simulated DOM only)');
