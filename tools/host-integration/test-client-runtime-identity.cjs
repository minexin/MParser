'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const root = path.resolve(process.argv[2]);
const source = fs.readFileSync(path.join(root,'web/js/app.js'),'utf8');
const html = fs.readFileSync(path.join(root,'web/index.html'),'utf8');
assert.ok(!/MParser V1\.14/.test(html),'initial edition must not assert an unloaded kernel version');

async function check(info,expectedVersion,expectedMilestone='') {
    const nodes = new Map();
    const node = selector => {
        if (!nodes.has(selector)) nodes.set(selector,{textContent:'',addEventListener(){}});
        return nodes.get(selector);
    };
    const messages = [];
    const MExec = {
        editor:{init(){}},command:{init(){},print:text=>messages.push(text)},
        workspace:{render(){}},
        api:{info:async()=>info,createSession:async()=>({id:'identity-session'}),
            session:async()=>({id:'identity-session'}),workspace:async()=>({variables:[]})},
    };
    const context = {MExec,document:{querySelector:node,querySelectorAll:()=>[]},
        sessionStorage:{getItem:()=>null,setItem(){}},
        setTimeout(){throw new Error('unexpected retry while testing runtime identity');}};
    context.window = {MExec,addEventListener(){}};
    vm.runInNewContext(source,context);
    await MExec.app.init();
    assert.equal(node('.edition').textContent,`MParser ${expectedVersion} · 科学计算工作台`);
    assert.equal(node('#status-runtime').textContent,
        `MExecServer ${info.version || '?'} · MParser ${expectedVersion}${expectedMilestone ? ' · '+expectedMilestone : ''}`);
    assert.ok(!messages.some(text=>text.includes('失败')),'initialization must reach a usable session');
    assert.equal(MExec.app.state.sessionId,'identity-session');
}
async function main() {
    await check({version:'0.1.0',runtime:{version:'1.15.0'},kernel_integration:{milestone:'development'}},'1.15.0','development');
    await check({version:'0.1.0',runtime:{version:'1.14.0'},kernel_integration:{milestone:'v1.14'}},'1.14.0','v1.14');
    await check({version:'0.1.0',runtime:{version:'1.15.0'}},'1.15.0');
    await check({version:'0.1.0',kernel_integration:{milestone:'development'}},'?','development');
    await check({runtime:{version:'<img src=x>'}},'<img src=x>');
    console.log('PASS runtime identity: actual product version, separate milestone, legacy/missing fields and literal text');
}
main().catch(error=>{console.error(error);process.exitCode=1;});
