const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const base = path.resolve(__dirname, '..');
const flush = async () => { for (let i = 0; i < 50; ++i) await Promise.resolve(); };
function video(box = { left: 10, top: 20, width: 800, height: 450 }, properties = {}) {
  let nextCallback = 1;
  const callbacks = new Map();
  return { tagName: 'VIDEO', currentTime: 0, seeking: false, paused: false, ended: false, readyState: 4, currentSrc: 'synthetic.webm',
    videoWidth: 1920, videoHeight: 1080, loop: false, muted: false, controls: false,
    box, parentElement: null, callbacks, ...properties,
    getBoundingClientRect() { return { ...this.box, right: this.box.left + this.box.width, bottom: this.box.top + this.box.height }; },
    requestVideoFrameCallback(callback) { const id = nextCallback++; callbacks.set(id, callback); return id; },
    cancelVideoFrameCallback(id) { callbacks.delete(id); },
    frame(metadata) { const entries = [...callbacks]; callbacks.clear(); for (const [,callback] of entries) callback(0, metadata); }
  };
}
function contentHarness(initialEnabled = true, storageGet = null) {
  let now = 10000, nextTimer = 1, uuid = 0;
  const timers = new Map(), messages = [], handlers = {}, observers = {}, videos = [video()], ui = [];
  const runtimeListeners = [];
  const window = { innerWidth: 1280, innerHeight: 720, outerWidth: 1300, outerHeight: 850,
    devicePixelRatio: 1.5, screenX: 100, screenY: 200,
    addEventListener: (name, fn) => { handlers[name] = fn; },
    visualViewport: { offsetLeft: 0, offsetTop: 0, width: 1280, height: 720, scale: 1,
      addEventListener: (name,fn) => { handlers['visual-' + name] = fn; } } };
  window.top = window;
  const document = { title: 'Synthetic video', documentElement: {},
    querySelectorAll: selector => selector === 'video' ? videos : ui,
    addEventListener: (name,fn) => { handlers[name] = fn; } };
  const timer = (fn, delay, interval = false) => { const id = nextTimer++; timers.set(id, { fn, delay, interval, at: now + delay }); return id; };
  const ctx = vm.createContext({ window, document, location: { href: 'http://localhost/test' },
    crypto: { randomUUID: () => 'uuid-' + ++uuid }, Date: { now: () => now },
    setTimeout: (fn,delay) => timer(fn,delay), clearTimeout: id => timers.delete(id),
    setInterval: (fn,delay) => timer(fn,delay,true),
    getComputedStyle: element => ({ display: 'block', visibility: 'visible', opacity: '1',
      objectFit: 'contain', objectPosition: '50% 50%', transform: 'none', ...element.style }),
    ResizeObserver: class { constructor(fn) { observers.resize = fn; } observe(target) { observers.target = target; } disconnect() { observers.target = null; } },
    MutationObserver: class { constructor(fn) { observers.mutation = fn; } observe() {} },
    chrome: { storage: { local: { get: async () => storageGet ? storageGet() : ({ enabled: initialEnabled }) } },
      runtime: { onMessage: { addListener: fn => runtimeListeners.push(fn) },
        sendMessage: async message => messages.push({ at: now, message }) } } });
  vm.runInContext(fs.readFileSync(path.join(base,'video-geometry.js'),'utf8'),ctx);
  vm.runInContext(fs.readFileSync(path.join(base,'content.js'),'utf8'),ctx);
  return { window, document, videos, ui, messages, handlers, observers,
    setEnabled: value => runtimeListeners.forEach(listener => listener({ type: 'veyra-enabled-changed', enabled: value })),
    deliverEnabled: (value,version) => runtimeListeners.forEach(listener=>listener({type:'veyra-enabled-changed',enabled:value,stateVersion:version})),
    advance(ms) {
      const end = now + ms;
      for (;;) {
        const due = [...timers].filter(([,timer]) => timer.at <= end).sort((a,b) => a[1].at - b[1].at)[0];
        if (!due) break;
        const [id,timer] = due; now = timer.at;
        if (timer.interval) timer.at += timer.delay; else timers.delete(id);
        timer.fn();
      }
      now = end;
    }
  };
}
const latest = h => h.messages.at(-1).message;
test('normal source reports image region, original pixels and fractional DPR without pixel readback', async () => {
  const h = contentHarness(); await flush(); h.advance(0);
  assert.equal(latest(h).candidate.rect.width, 800);
  assert.equal(latest(h).candidate.sourceSize.width, 1920);
  assert.equal(latest(h).candidate.viewport.devicePixelRatio, 1.5);
  assert.equal(latest(h).candidate.viewport.visualViewport.scale, 1);
  assert.equal(h.observers.target, h.videos[0]);
});
test('actual steady content timers recover after a one-millisecond heartbeat boundary miss',async t=>{
  const h=contentHarness();await flush();h.advance(0);
  h.advance(151);h.handlers.visibilitychange();h.advance(0);h.advance(5000);
  const reports=h.messages.filter(item=>item.message.candidate).map(item=>item.at);
  assert.deepEqual(reports,[10000,10151,11650,12650,13650,14650]);
  const gaps=reports.slice(1).map((at,n)=>at-reports[n]);
  assert.equal(Math.max(...gaps),1499);assert.ok(gaps.every(gap=>gap<2500));
  t.diagnostic(JSON.stringify({contentAt:reports,maxGap:Math.max(...gaps),nativeTTL:2500}));
});
test('unchanged video metadata remains live through an event storm without trailing debounce starvation',async()=>{
  const h=contentHarness();await flush();h.advance(0);
  for(let n=0;n<300;n++){h.handlers.scroll();h.observers.mutation();h.advance(20);}
  const reports=h.messages.filter(item=>item.message.candidate).map(item=>item.at);
  assert.ok(reports.length>=6);
  assert.ok(reports.slice(1).every((at,n)=>at-reports[n]<=1500));
  assert.ok(reports.every(at=>at>=10000));
});
test('late initial ON storage cannot override a newer OFF broadcast',async()=>{
  let release;const h=contentHarness(true,()=>new Promise(resolve=>{release=resolve;}));
  h.deliverEnabled(false,10002);release({enabled:true});await flush();h.advance(0);
  assert.equal(latest(h).candidate,null);assert.equal(h.videos[0].callbacks.size,0);
});
test('an old delivered ON broadcast cannot override a newer OFF version',async()=>{
  const h=contentHarness();await flush();h.advance(0);
  h.deliverEnabled(false,10003);h.deliverEnabled(true,10002);h.advance(0);
  assert.equal(latest(h).candidate,null);assert.equal(h.videos[0].callbacks.size,0);
});
test('continuous animated DOM changes cannot starve resize coordinates', async () => {
  const h = contentHarness(); await flush(); h.advance(0);
  h.videos[0].box.width = 640; h.videos[0].box.height = 360; h.observers.resize();
  for (let i=0;i<100;i++) { h.observers.mutation(); h.advance(20); }
  const resized = h.messages.find(item => item.message.candidate?.rect.width === 640);
  assert.ok(resized); assert.ok(resized.at - h.messages[0].at <= 150);
});
test('OFF emits only metadata-free state probes and does not measure source frames', async () => {
  const h = contentHarness(false); await flush(); h.advance(0);
  assert.equal(latest(h).candidate, null); assert.equal(h.videos[0].callbacks.size,0);
  const count=h.messages.length; h.advance(3000);
  assert.ok(h.messages.length>count); assert.ok(h.messages.every(item=>item.message.candidate===null));
});
test('paused main video wins over smaller muted decorative loop', async () => {
  const h = contentHarness(); h.videos[0].paused = true;
  h.videos.push(video({ left:900,top:20,width:300,height:200 }, { loop:true,muted:true }));
  await flush(); h.advance(0);
  assert.equal(latest(h).candidate.playing,false); assert.equal(latest(h).candidate.rect.width,800);
});
test('main video always outranks a playing thumbnail; tiny sources do not qualify', async () => {
  const h = contentHarness();
  h.videos.push(video({ left:900,top:20,width:300,height:170 }));
  await flush(); h.advance(0); assert.equal(latest(h).candidate.rect.width,800);
  h.videos.splice(0,2,video({ left:0,top:0,width:160,height:90 })); h.handlers.play(); h.advance(150);
  assert.equal(latest(h).candidate,null);
});
test('UI rectangles are preserved with screen capture and require no decoded relay', async () => {
  const h = contentHarness();
  h.ui.push({ parentElement:null, getBoundingClientRect:()=>({ left:10,top:420,right:810,bottom:470 }) });
  await flush(); h.advance(0);
  const rectangles=latest(h).candidate.uiRects; assert.equal(rectangles.length,1);
  assert.equal(rectangles[0][0],0); assert.equal(rectangles[0][2],1); assert.equal(rectangles[0][3],1);
  assert.ok(rectangles[0][1]>.8);
});
test('rVFC measures 24fps source and a seek resets its history', async () => {
  const h = contentHarness(); await flush(); h.advance(0);
  for (let n=0;n<=36;n++) h.videos[0].frame({ mediaTime:n/24,presentedFrames:n+1 });
  h.advance(2100); assert.equal(latest(h).candidate.frameRate,24);
  h.videos[0].frame({ mediaTime:0,presentedFrames:40 }); h.handlers.seeked(); h.advance(150);
  assert.equal(latest(h).candidate.frameRate,0);
  h.setEnabled(false); h.advance(0); assert.equal(h.videos[0].callbacks.size,0);
});
test('object-fit letterbox remains outside video region and rotation safely falls back', async () => {
  const h = contentHarness(); h.videos[0].box.width=600; h.videos[0].box.height=600;
  await flush(); h.advance(0); assert.equal(latest(h).candidate.rect.height,337.5);
  assert.equal(latest(h).candidate.rect.top,151.25);
  h.videos[0].style={transform:'rotate(90deg)'}; h.handlers.resize(); h.advance(150);
  assert.equal(latest(h).candidate,null);
});
test('visual viewport offset and zoom metadata are reported rather than guessed', async () => {
  const h = contentHarness(); h.window.visualViewport.offsetLeft=12; h.window.visualViewport.scale=1.3;
  await flush(); h.advance(0);
  assert.equal(latest(h).candidate.viewport.visualViewport.offsetLeft,12);
  assert.equal(latest(h).candidate.viewport.visualViewport.scale,1.3);
});
test('media identity remains stable on heartbeat and normal play or resume', async () => {
  const h=contentHarness();await flush();h.advance(0);
  const original={...latest(h).candidate.mediaInfo};
  h.handlers.play({target:h.videos[0]});h.advance(150);h.advance(2100);
  assert.equal(latest(h).candidate.mediaInfo.videoId,original.videoId);
  assert.equal(latest(h).candidate.mediaInfo.sourceEpoch,original.sourceEpoch);
});
test('same-sized video element replacement gets a distinct media identity', async () => {
  const h=contentHarness();await flush();h.advance(0);
  const original=latest(h).candidate.mediaInfo.videoId;
  h.videos[0]=video();h.observers.mutation();h.advance(150);
  assert.notEqual(latest(h).candidate.mediaInfo.videoId,original);
  assert.equal(latest(h).candidate.mediaInfo.sourceEpoch,0);
});
test('actual source changes increment epoch but repeated loadedmetadata does not', async () => {
  const h=contentHarness();await flush();h.advance(0);const v=h.videos[0];
  v.currentSrc='other.webm';h.handlers.loadedmetadata({target:v});h.advance(150);
  assert.equal(latest(h).candidate.mediaInfo.sourceEpoch,1);
  h.handlers.loadedmetadata({target:v});h.advance(150);h.advance(2100);
  assert.equal(latest(h).candidate.mediaInfo.sourceEpoch,1);
  assert.ok(!JSON.stringify(latest(h).candidate.mediaInfo).includes('webm'));
});
test('seek restores the source with a new epoch and repeated seeked is stable', async () => {
  const h=contentHarness();await flush();h.advance(0);const v=h.videos[0];
  h.handlers.seeking({target:v});h.advance(150);
  assert.equal(latest(h).candidate.playing,false);assert.equal(latest(h).diagnostic.code,'seeking');
  v.currentTime=120;h.handlers.seeked({target:v});h.advance(150);
  assert.equal(latest(h).candidate.playing,true);assert.equal(latest(h).candidate.mediaInfo.sourceEpoch,1);
  h.handlers.seeked({target:v});h.advance(150);h.advance(2100);
  assert.equal(latest(h).candidate.mediaInfo.sourceEpoch,1);
});
test('same URL reload resets once across emptied and loadedmetadata events', async () => {
  const h=contentHarness();await flush();h.advance(0);const v=h.videos[0];
  h.handlers.emptied({target:v});h.handlers.emptied({target:v});h.handlers.loadedmetadata({target:v});h.advance(150);
  assert.equal(latest(h).candidate.mediaInfo.sourceEpoch,1);
});
test('changing source during a cancelled seek cannot leave the new video stuck seeking',async()=>{
  const h=contentHarness();await flush();h.advance(0);const v=h.videos[0];
  h.handlers.seeking({target:v});h.advance(150);assert.equal(latest(h).candidate.playing,false);
  v.currentSrc='';v.readyState=0;h.handlers.emptied({target:v});h.advance(150);
  v.currentSrc='replacement.webm';v.readyState=4;v.seeking=false;
  h.handlers.loadedmetadata({target:v});h.advance(150);
  assert.equal(latest(h).candidate.playing,true);assert.equal(latest(h).diagnostic.code,'ready');
  assert.equal(latest(h).candidate.mediaInfo.sourceEpoch,1);
});
test('loading and unsupported layout distinguish detected video from no video', async () => {
  const h=contentHarness();await flush();h.advance(0);const v=h.videos[0];
  v.readyState=0;h.handlers.loadedmetadata({target:v});h.advance(150);
  assert.equal(latest(h).candidate,null);assert.equal(latest(h).diagnostic.code,'video-loading');
  assert.equal(latest(h).diagnostic.videoDetected,true);
  v.readyState=4;v.style={transform:'matrix(0, 1, -1, 0, 0, 0)'};h.observers.mutation();h.advance(150);
  assert.equal(latest(h).diagnostic.code,'unsupported-geometry');
  h.videos.length=0;h.observers.mutation();h.advance(150);
  assert.equal(latest(h).diagnostic.code,'no-video');assert.equal(latest(h).diagnostic.videoDetected,false);
});
test('PiP is diagnosed and never captured using the obsolete inline rectangle',async()=>{
  const h=contentHarness();await flush();h.advance(0);const v=h.videos[0];
  h.document.pictureInPictureElement=v;h.handlers.enterpictureinpicture({target:v});h.advance(150);
  assert.equal(latest(h).candidate,null);assert.equal(latest(h).diagnostic.code,'picture-in-picture');
  h.document.pictureInPictureElement=null;h.handlers.leavepictureinpicture({target:v});h.advance(150);
  assert.equal(latest(h).candidate.playing,true);
});
