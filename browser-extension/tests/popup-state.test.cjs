const {test}=require('node:test');
const assert=require('node:assert/strict');
const fs=require('node:fs'),path=require('node:path'),vm=require('node:vm');
const flush=async()=>{for(let n=0;n<30;n++)await Promise.resolve();};
function popupHarness(initial){
  const elements=Object.fromEntries(['enabled','status','details','error','retry'].map(id=>[id,{checked:false,disabled:id==='enabled',hidden:false,textContent:'',listeners:{},addEventListener(name,fn){this.listeners[name]=fn;}}]));
  let now=10000,nextTimer=1,interval;
  const timers=new Map();
  let hook=async()=>initial;
  const calls=[];
  vm.runInNewContext(fs.readFileSync(path.join(__dirname,'..','popup.js'),'utf8'),{
    document:{getElementById:id=>elements[id]},Date:{now:()=>now},
    chrome:{runtime:{sendMessage:async message=>{calls.push(message);return hook(message);}}},
    setInterval:fn=>{interval=fn;return 0;},
    setTimeout:(fn,delay)=>{const id=nextTimer++;timers.set(id,{fn,at:now+delay});return id;},clearTimeout:id=>timers.delete(id)
  });
  return {elements,calls,setHook:fn=>{hook=fn;},toggle:async value=>{elements.enabled.checked=value;await elements.enabled.listeners.change();},
    tick:async()=>{interval();await flush();},retry:async()=>{elements.retry.listeners.click();await flush();},
    advance:async ms=>{const end=now+ms;for(;;){const due=[...timers].filter(([,t])=>t.at<=end).sort((a,b)=>a[1].at-b[1].at)[0];if(!due)break;const[id,t]=due;timers.delete(id);now=t.at;t.fn();await flush();}now=end;await flush();}};
}
test('original video and screen-capture dimensions are displayed separately',async()=>{
  const h=popupHarness({online:true,enabled:true,state:'running',status:'运行中',overlayVisible:true,srActive:true,
    browserSourceWidth:1920,browserSourceHeight:1080,sourceWidth:3747,sourceHeight:1979,
    presentSubmitFps:120,xessSdkGenerated:999,realPresented:888});await flush();
  assert.match(h.elements.details.textContent,/原视频 1920×1080/);
  assert.match(h.elements.details.textContent,/捕获区域 3747×1979/);
  assert.ok(!h.elements.details.textContent.includes('原视频 3747'));
  assert.ok(!/120|999|888|FPS|fps/.test(h.elements.details.textContent));
});
test('a hung initial status request times out, unlocks the switch and can retry',async()=>{
  const h=popupHarness(new Promise(()=>{}));await flush();
  assert.equal(h.elements.enabled.disabled,true);
  await h.tick();await h.tick();assert.equal(h.calls.length,1);
  await h.advance(2600);
  assert.equal(h.elements.enabled.disabled,false);assert.equal(h.elements.retry.disabled,false);
  assert.match(h.elements.error.textContent,/超时/);assert.equal(h.elements.enabled.checked,false);
  h.setHook(async()=>({online:true,enabled:false,state:'stopped',status:'已关闭'}));await h.retry();
  assert.equal(h.elements.status.textContent,'已关闭');assert.equal(h.elements.error.hidden,true);
});
test('a hung ON control times out without leaving the switch enabled or unclickable',async()=>{
  const h=popupHarness({online:true,enabled:false,state:'stopped'});await flush();
  h.setHook(()=>new Promise(()=>{}));const action=h.toggle(true);await flush();
  assert.equal(h.elements.enabled.disabled,true);await h.advance(4000);await action;
  assert.equal(h.elements.enabled.checked,false);assert.equal(h.elements.enabled.disabled,false);
  assert.equal(h.elements.retry.disabled,false);assert.match(h.elements.error.textContent,/超时/);
});
test('capture dimensions alone are never described as original video pixels',async()=>{
  const h=popupHarness({online:true,enabled:true,state:'waiting',sourceWidth:3840,sourceHeight:2160});await flush();
  assert.match(h.elements.details.textContent,/捕获区域 3840×2160/);
  assert.ok(!h.elements.details.textContent.includes('原视频'));
});
test('specific native reason and state replace the generic waiting message',async()=>{
  const h=popupHarness({online:true,enabled:true,state:'waiting',reasonCode:'engine-failed',status:'增强引擎初始化失败，已保留原画面',
    error:'GPU initialization failed',sourceDiagnostic:{code:'ready',videoDetected:true,updatedAt:10000}});await flush();
  assert.match(h.elements.status.textContent,/增强引擎初始化失败/);
  assert.match(h.elements.details.textContent,/Veyra 状态：等待/);
  assert.equal(h.elements.error.hidden,false);assert.equal(h.elements.error.textContent,'GPU initialization failed');
});
test('background detection explains why no overlay is running',async()=>{
  const h=popupHarness({online:true,enabled:true,state:'waiting',reasonCode:'waiting-video',status:'等待报告',
    sourceDiagnostic:{code:'foreground-mismatch',videoDetected:true,sourceWidth:1920,sourceHeight:1080,updatedAt:10000}});await flush();
  assert.match(h.elements.status.textContent,/已检测到视频.*浏览器在后台/);
  assert.match(h.elements.details.textContent,/原视频 1920×1080/);
  assert.ok(h.calls.every(call=>call.type==='veyra-status'));
});
test('a generic native stop keeps the specific current unsupported or PiP explanation',async()=>{
  const h=popupHarness({online:true,enabled:true,state:'waiting',reasonCode:'stopped',status:'当前网页视频已停止；显示原画',
    sourceDiagnostic:{code:'picture-in-picture',videoDetected:true,updatedAt:10000}});await flush();
  assert.match(h.elements.status.textContent,/画中画/);assert.ok(!h.elements.status.textContent.includes('等待前台'));
});
test('real connection errors remain visible when native is offline',async()=>{
  const h=popupHarness({online:false,enabled:false,error:'Veyra extension origin required',connection:{lastStatusSuccessAt:9000}});await flush();
  assert.equal(h.elements.error.hidden,false);assert.equal(h.elements.error.textContent,'Veyra extension origin required');
  assert.match(h.elements.details.textContent,/当前连接已中断/);
});
test('a stale source diagnostic cannot masquerade as the current reason',async()=>{
  const h=popupHarness({online:true,enabled:true,status:'等待新报告',sourceDiagnostic:{code:'paused',videoDetected:true,updatedAt:-10000}});await flush();
  assert.equal(h.elements.status.textContent,'等待新报告');
});
test('a delayed refresh cannot reverse the displayed explicit OFF result',async()=>{
  let release;
  const h=popupHarness(new Promise(resolve=>{release=resolve;}));
  h.setHook(async()=>({online:true,enabled:false,state:'stopped',status:'已关闭'}));
  await h.toggle(false);release({online:true,enabled:true,state:'running',overlayVisible:true,srActive:true});await flush();
  assert.equal(h.elements.enabled.checked,false);assert.equal(h.elements.status.textContent,'已关闭');
});
