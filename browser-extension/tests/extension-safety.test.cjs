const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const base = path.resolve(__dirname, '..');
const source = name => fs.readFileSync(path.join(base, name), 'utf8');
const flush = async () => { for (let i = 0; i < 150; ++i) await Promise.resolve(); };
function event() {
  const listeners = [];
  return { addListener: fn => listeners.push(fn), emit: (...args) => listeners.map(fn => fn(...args)) };
}
function workerHarness(savedEnabled = false, options = {}) {
  let now = 10000, nextTimer = 1, uuid = 0, focused = true, tabId = 7, zoom = 1, windowState = "normal";
  const timers = new Map(), calls = [], sent = [], badges = [];
  const state = { enabled: savedEnabled, clientId: 'stable-client-id' };
  const native = { enabled: false, overlayVisible: false, status: 'off' };
  const listener = event();
  let statusHook = null, videoHook = null, zoomHook = null, storageHook = null, focusedHook = null, offline = false;
  const browserWindow = () => ({ id: 3, focused, state: windowState, left: 250, top: 100, width: 1300, height: 850,
    tabs: [{ id: tabId, windowId: 3, active: true }] });
  const setTimer = (fn, delay) => { const id = nextTimer++; timers.set(id, { fn, at: now + delay }); return id; };
  const chrome = {
    runtime: { onMessage: listener, onInstalled: event(), onStartup: event(),
      getURL: file => 'chrome-extension://test-extension/' + file },
    storage: { local: { get: async defaults => options.storageGetHook ? options.storageGetHook(defaults,state) : ({ ...defaults, ...state }),
      set: async patch => { if(storageHook) await storageHook(patch); return Object.assign(state, patch); } } },
    action: { setBadgeBackgroundColor: async () => {}, setBadgeText: async value => badges.push(value.text) },
    windows: { WINDOW_ID_NONE: -1, onFocusChanged: event(),
      getLastFocused: async () => focusedHook ? focusedHook() : browserWindow(), get: async () => browserWindow() },
    tabs: { onActivated: event(), onRemoved: event(), onUpdated: event(), onZoomChange: event(),
      query: async () => [{ id: tabId, windowId: 3, active: true }],
      getZoom: async () => zoomHook ? zoomHook() : zoom,
      sendMessage: async (id, message) => sent.push({ id, message }) }
  };
  const fetch = async (url, options) => {
    assert.ok(url.startsWith('http://127.0.0.1:24861/v1/'));
    const body = options.body ? JSON.parse(options.body) : null;
    calls.push({ url, body, options, at: now });
    if (offline) throw new Error('ECONNREFUSED');
    let result;
    if (url.endsWith('/status')) result = statusHook ? await statusHook(options.signal) : { ...native };
    else if (url.endsWith('/control')) { native.enabled = body.enabled; native.status = body.enabled ? 'waiting' : 'off'; result = { ok: true }; }
    else result = videoHook ? await videoHook(body, options.signal) : { ok: true, ...native };
    const status = result.httpStatus || 200;
    return { ok: status >= 200 && status < 300, status, json: async () => result };
  };
  vm.runInNewContext(source('service-worker.js'), { chrome, crypto: { randomUUID: () => 'uuid-' + ++uuid },
    Date: { now: () => now }, AbortController, fetch,
    setTimeout: setTimer, clearTimeout: id => timers.delete(id), console });
  const candidate = (width = 800) => ({ rect: { left: 20, top: 30, width, height: 450 },
    viewport: { innerWidth: 1280, innerHeight: 720, devicePixelRatio: 1.5,
      screenX: 250, screenY: 100, outerWidth: 1300, outerHeight: 850,
      visualViewport: { offsetLeft: 0, offsetTop: 0, width: 1280, height: 720, scale: 1 } },
    sourceSize: { width: 1920, height: 1080 }, mediaInfo: { videoId: 'video-a', sourceEpoch: 0 }, playing: true, frameRate: 29.97,
    uiRects: [[0, 0.9, 1, 1]], pageTitle: 'Synthetic source', pageUrl: 'http://localhost/test' });
  return {
    state, native, calls, sent, chrome, badges,
    statusHook: fn => { statusHook = fn; }, videoHook: fn => { videoHook = fn; }, zoomHook: fn => { zoomHook = fn; },
    storageHook: fn => { storageHook = fn; },
    focusedHook: fn => { focusedHook = fn; },
    setOffline: value => { offline = value; }, setZoom: value => { zoom = value; },
    setFocused: value => { focused = value; }, setWindowState: value => { windowState = value; },
    advance: async ms => {
      const end = now + ms;
      for (;;) {
        const due = [...timers].filter(([,timer]) => timer.at <= end).sort((a,b) => a[1].at - b[1].at)[0];
        if (!due) break;
        const [id,timer] = due; timers.delete(id); now = timer.at; timer.fn(); await flush();
      }
      now = end; await flush();
    },
    report: async (width = 800, properties = {}, senderProperties = {}, messageProperties = {}) => {
      listener.emit({ type: 'veyra-video-state', documentToken: 'doc-a', candidate: { ...candidate(width), ...properties }, ...messageProperties },
        { tab: { id: 7, windowId: 3 }, frameId: 0, documentId: 'doc-a', ...senderProperties }, () => {});
      await flush();
    },
    popup: message => new Promise(resolve => {
      listener.emit(message, { url: chrome.runtime.getURL('popup.html') }, resolve);
    }),
    focus: async value => { focused = value; await Promise.all(chrome.windows.onFocusChanged.emit(value ? 3 : -1)); await flush(); },
    activate: async id => { tabId = id; await Promise.all(chrome.tabs.onActivated.emit({ tabId: id, windowId: 3 })); await flush(); }
  };
}
const videos = h => h.calls.filter(call => call.body?.type === 'video');
const stops = h => h.calls.filter(call => call.body?.type === 'stop');
const controls = h => h.calls.filter(call => call.url.endsWith('/control'));

test('first install and worker wake never call control or launch; saved ON is not authority', async () => {
  const h = workerHarness(true); await flush();
  assert.equal(h.state.enabled, false); assert.equal(h.calls.length, 0);
  await Promise.all(h.chrome.runtime.onInstalled.emit({ reason: 'install' }));
  await Promise.all(h.chrome.runtime.onStartup.emit());
  await h.report(); await h.advance(3000); await h.report();
  assert.equal(controls(h).length, 0); assert.equal(videos(h).length, 0);
  assert.equal(h.state.enabled, false);
});
test('status uses schema-only read-only POST; its heartbeat cannot enable or control native', async () => {
  const h = workerHarness(); await h.report(); await h.advance(2100); await h.report();
  const statusCalls = h.calls.filter(call => call.url.endsWith('/status'));
  assert.equal(statusCalls.length,2);
  for (const call of statusCalls) {
    assert.equal(call.options.method,'POST');
    assert.equal(call.options.headers['Content-Type'],'application/json');
    assert.deepEqual(call.body,{schema:1});
    assert.ok(!Object.hasOwn(call.body,'enabled'));
  }
  assert.equal(controls(h).length,0); assert.equal(videos(h).length,0);
  assert.equal(h.native.enabled,false); assert.equal(h.state.enabled,false);
});
test('background OFF probes only read POST status and never send periodic stops', async () => {
  const h = workerHarness(); h.setFocused(false);
  await h.report(); await h.advance(500); await h.advance(2100); await h.report(); await h.advance(500);
  assert.equal(h.calls.length,2);
  assert.ok(h.calls.every(call=>call.url.endsWith('/status')&&call.options.method==='POST'));
  assert.equal(stops(h).length,0); assert.equal(videos(h).length,0); assert.equal(controls(h).length,0);
  assert.equal(h.state.online,true); assert.equal(h.state.enabled,false);
  assert.equal(h.state.connection.workerVersion,'0.2.3');
  assert.equal(h.state.connection.statusMethod,'POST'); assert.ok(h.state.connection.lastStatusSuccessAt>0);
});
test('native ON synchronizes in the background while video delivery remains forbidden', async () => {
  const h = workerHarness(); h.native.enabled=true; h.setFocused(false);
  await h.report(); await h.advance(500);
  assert.equal(h.state.online,true); assert.equal(h.state.enabled,true);
  assert.equal(h.state.sourceDiagnostic.code,'foreground-mismatch');
  assert.equal(h.state.sourceDiagnostic.videoDetected,true); assert.equal(h.state.sourceDiagnostic.foreground,false);
  assert.equal(videos(h).length,0); assert.equal(stops(h).length,0); assert.equal(controls(h).length,0);
  assert.ok(!JSON.stringify(h.state.sourceDiagnostic).includes('localhost'));
});
test('a foreground loss during a delayed status query cannot deliver video', async () => {
  const h = workerHarness(); h.native.enabled=true;
  let release; h.statusHook(()=>new Promise(resolve=>{release=resolve;}));
  await h.report(); await h.focus(false); await h.advance(400);
  release({...h.native}); await flush();
  assert.equal(videos(h).length,0); assert.equal(controls(h).length,0);
  assert.equal(h.state.sourceDiagnostic.foreground,false);
});
test('latest media epoch wins while reports await the same status request', async () => {
  const h = workerHarness(); h.native.enabled=true;
  let release; h.statusHook(()=>new Promise(resolve=>{release=resolve;}));
  await h.report(800,{mediaInfo:{videoId:'video-a',sourceEpoch:1}});
  await h.report(800,{mediaInfo:{videoId:'video-a',sourceEpoch:2}});
  release({...h.native}); await flush();
  assert.equal(videos(h).length,1); assert.equal(videos(h)[0].body.mediaInfo.sourceEpoch,2);
});
test('another background tab cannot starve the foreground report sharing its status query', async () => {
  const h=workerHarness();h.native.enabled=true;
  let release;h.statusHook(()=>new Promise(resolve=>{release=resolve;}));
  await h.report();await h.report(800,{}, {tab:{id:99,windowId:9}});
  release({...h.native});await flush();
  assert.equal(videos(h).length,1);assert.equal(videos(h)[0].body.windowInfo.tabId,7);
});
test('a late geometry lookup cannot overwrite the newer media epoch', async () => {
  const h=workerHarness();h.native.enabled=true;
  let release,count=0;h.zoomHook(()=>++count===1?new Promise(resolve=>{release=resolve;}):1);
  await h.report(800,{mediaInfo:{videoId:'video-a',sourceEpoch:1}});
  await h.report(800,{mediaInfo:{videoId:'video-a',sourceEpoch:2}});
  release(1);await flush();
  assert.equal(videos(h).length,1);assert.equal(videos(h)[0].body.mediaInfo.sourceEpoch,2);
});
test('only trusted popup explicit toggle controls native enabling', async () => {
  const h = workerHarness(); await flush();
  h.chrome.runtime.onMessage.emit({ type: 'veyra-control', enabled: true },
    { tab: { id: 7 }, url: 'https://example.com' }, () => {});
  await flush(); assert.equal(controls(h).length, 0);
  const reply = await h.popup({ type: 'veyra-control', enabled: true });
  assert.equal(reply.enabled, true); assert.equal(h.state.enabled, true);
  assert.equal(controls(h).length, 1);
  assert.deepEqual(controls(h)[0].body, { schema: 1, enabled: true });
  await h.report(); assert.equal(videos(h).length, 1);
  assert.equal(controls(h).length, 1);
});
test('native UI can toggle extension state without an implicit control request', async () => {
  const h = workerHarness(); h.native.enabled = true; await h.report();
  assert.equal(h.state.enabled, true); assert.equal(videos(h).length, 1);
  const previousStops=stops(h).length;
  h.native.enabled = false; await h.advance(2100); await h.report();
  assert.equal(h.state.enabled, false); assert.equal(stops(h).length,previousStops);
  assert.equal(controls(h).length, 0);
});
test('window position, source size, page zoom, DPR and UI rectangles survive transport', async () => {
  const h = workerHarness(); h.native.enabled = true; h.setZoom(1.25); await h.report();
  const p = videos(h)[0].body;
  assert.equal(p.schema, 1); assert.equal(p.clientId, 'stable-client-id'); assert.ok(p.sessionId);
  assert.equal(p.viewport.pageZoom, 1.25); assert.equal(p.viewport.devicePixelRatio, 1.5);
  assert.deepEqual(p.sourceSize, { width: 1920, height: 1080 });
  assert.deepEqual(p.uiRects, [[0,0.9,1,1]]);
  assert.equal(p.windowInfo.left, 250); assert.equal(p.windowInfo.top, 100);
  assert.equal(p.windowInfo.focused, true); assert.equal(p.frameRate, 29.97);
});
test('same geometry dedupes short bursts but preserves one-second metadata heartbeats', async () => {
  const h = workerHarness(); h.native.enabled = true; await h.report();
  await h.advance(400); await h.report(); assert.equal(videos(h).length, 1);
  await h.advance(600); await h.report(); assert.equal(videos(h).length, 2);
  assert.ok(videos(h)[1].body.sequence > videos(h)[0].body.sequence);
});
test('temporary WINDOW_ID_NONE keeps source; real app switch stops and background cannot revive', async () => {
  const h = workerHarness(); h.native.enabled = true; await h.report();
  await h.focus(false); await h.advance(399); assert.equal(stops(h).length, 0);
  await h.focus(true); await h.advance(100); assert.equal(stops(h).length, 0);
  await h.focus(false); await h.advance(400); assert.equal(stops(h).length, 1);
  const count = videos(h).length;
  for (let i = 0; i < 3; ++i) { await h.advance(2100); await h.report(); }
  assert.equal(videos(h).length, count); assert.equal(controls(h).length, 0);
  assert.equal(h.state.enabled, true);
  await h.focus(true); await h.report(); assert.equal(videos(h).length, count + 1);
});
test('tab switch immediately stops before the replacement page reports', async () => {
  const h = workerHarness(); h.native.enabled = true; await h.report();
  await h.activate(8); assert.equal(stops(h).at(-1).body.reason, 'active-tab-changed');
  const count = videos(h).length; await h.report(); assert.equal(videos(h).length, count);
});
test('tab activation in another background browser window does not stop the foreground source', async () => {
  const h = workerHarness(); h.native.enabled=true; await h.report();
  await Promise.all(h.chrome.tabs.onActivated.emit({tabId:99,windowId:9})); await flush();
  assert.equal(stops(h).length,0); assert.equal(videos(h).length,1);
  assert.ok(!h.sent.some(item=>item.id===99));
});
test('OFF tab activation remains read-only and does not send a stop without an active source',async()=>{
  const h=workerHarness();await h.report();await h.activate(8);
  assert.equal(stops(h).length,0);assert.equal(videos(h).length,0);assert.equal(controls(h).length,0);
  assert.ok(h.calls.every(call=>call.url.endsWith('/status')));
});
test('an old activation snapshot cannot stop the newer foreground tab source',async()=>{
  const h=workerHarness();h.native.enabled=true;await h.report();
  const snapshot=id=>({id:3,focused:true,tabs:[{id,windowId:3,active:true}]});
  let release,count=0;h.focusedHook(()=>++count===1?new Promise(resolve=>{release=resolve;}):snapshot(8));
  const oldEvent=h.chrome.tabs.onActivated.emit({tabId:7,windowId:3});await flush();assert.ok(release);
  await h.activate(8);await h.report(800,{}, {tab:{id:8,windowId:3},documentId:'doc-b'});
  const stopCount=stops(h).length;assert.equal(videos(h).at(-1).body.windowInfo.tabId,8);
  release(snapshot(7));await Promise.all(oldEvent);await flush();
  assert.equal(stops(h).length,stopCount);assert.equal(videos(h).at(-1).body.windowInfo.tabId,8);
});
test('pause recovers on play without disabling the user switch', async () => {
  const h = workerHarness(); h.native.enabled = true; await h.report();
  await h.report(800, { playing: false }); assert.equal(stops(h).at(-1).body.reason, 'video-paused');
  assert.equal(h.state.enabled, true); await h.report(); assert.equal(videos(h).length, 2);
});
test('late zoom/window lookup is invalidated by a tab change', async () => {
  const h = workerHarness(); h.native.enabled = true;
  let release; h.zoomHook(() => new Promise(resolve => { release = resolve; }));
  await h.report(); assert.ok(release); await h.activate(8); release(1); await flush();
  assert.equal(videos(h).length, 0); assert.equal(stops(h).length, 0);
});
test('a delayed old pause diagnostic cannot stop or replace the newer playing source',async()=>{
  const h=workerHarness();h.native.enabled=true;await h.report();
  let release,blocked=false;
  h.storageHook(patch=>patch.sourceDiagnostic?.code==='paused'&&!blocked?
    (blocked=true,new Promise(resolve=>{release=resolve;})):Promise.resolve());
  await h.report(800,{playing:false});assert.ok(release);
  const previousStops=stops(h).length;
  await h.report(800,{mediaInfo:{videoId:'video-a',sourceEpoch:1}});
  assert.equal(videos(h).length,2);
  release();await flush();
  assert.equal(stops(h).length,previousStops);assert.equal(h.state.sourceDiagnostic.code,'ready');
  assert.equal(videos(h).at(-1).body.mediaInfo.sourceEpoch,1);
});
test('a rejected old geometry lookup cannot stop a newer media epoch',async()=>{
  const h=workerHarness();h.native.enabled=true;await h.report();
  let reject,count=0;h.zoomHook(()=>++count===1?new Promise((_,bad)=>{reject=bad;}):1);
  await h.report(800,{mediaInfo:{videoId:'video-a',sourceEpoch:1}});
  await h.report(800,{mediaInfo:{videoId:'video-a',sourceEpoch:2}});
  reject(new Error('old tab lookup expired'));await flush();
  assert.equal(stops(h).length,0);assert.equal(videos(h).at(-1).body.mediaInfo.sourceEpoch,2);
});
test('server exit turns storage OFF and later metadata never re-enables it', async () => {
  const h = workerHarness(); h.native.enabled = true; await h.report();
  h.setOffline(true); await h.advance(2100); await h.report();
  assert.equal(h.state.enabled, false); assert.equal(h.state.online, false);
  const count = videos(h).length;
  await h.advance(2500); await h.report();
  h.setOffline(false); h.native.enabled = false; await h.advance(2500); await h.report();
  assert.equal(videos(h).length, count); assert.equal(controls(h).length, 0);
});
test('source iframe cannot report or control native and malformed geometry is ignored', async () => {
  const h = workerHarness(); h.native.enabled = true;
  await h.report(800, {}, { frameId: 3 }); assert.equal(h.calls.length, 0);
  await h.report(800, { rect: { left: 0, top: 0, width: NaN, height: 450 } });
  assert.equal(videos(h).length, 0); assert.equal(h.state.enabled, true);
});
test('invalid media epochs are diagnosed without sending native video or control', async () => {
  const h = workerHarness(); h.native.enabled=true;
  await h.report(800,{mediaInfo:{videoId:'video-a',sourceEpoch:-1}});
  assert.equal(videos(h).length,0); assert.equal(controls(h).length,0);
  assert.equal(h.state.sourceDiagnostic.code,'unsupported-geometry'); assert.equal(h.state.enabled,true);
});
test('a paused detected video is diagnosed before any capture has started', async () => {
  const h = workerHarness(); h.native.enabled=true; await h.report(800,{playing:false});
  assert.equal(h.state.sourceDiagnostic.code,'paused'); assert.equal(h.state.sourceDiagnostic.videoDetected,true);
  assert.equal(videos(h).length,0); assert.equal(controls(h).length,0);
});
test('invalid native status does not advertise RUN or remain enabled', async () => {
  const h = workerHarness(); h.statusHook(async () => ({ overlayVisible: true }));
  await h.report(); assert.equal(h.state.enabled, false); assert.equal(h.state.online, false);
  assert.equal(h.badges.at(-1), '');
});
test('unsupported geometry response keeps native connection and user switch for recovery', async () => {
  const h = workerHarness(); h.native.enabled = true;
  h.videoHook(async () => ({ ok:false,error:'unsupported visual viewport',httpStatus:400 }));
  await h.report(); assert.equal(h.state.enabled,true); assert.equal(h.state.online,true);
  assert.match(h.state.serverStatus.statusNote,/恢复原画面/); assert.equal(controls(h).length,0);
  h.videoHook(null); await h.advance(2100); await h.report();
  assert.equal(h.state.enabled,true); assert.equal(h.state.serverStatus.statusNote,undefined);
});
test('zoom change sends stop and asks for fresh DOM geometry', async () => {
  const h = workerHarness(); h.native.enabled = true; await h.report();
  h.chrome.tabs.onZoomChange.emit({ tabId: 7 }); await flush();
  assert.equal(stops(h).at(-1).body.reason, 'page-zoom-changing');
  assert.ok(h.sent.some(item => item.message.type === 'veyra-rescan'));
});
test('a slow receiver retains only the latest geometry, never a backlog of stale sizes', async () => {
  const h = workerHarness(); h.native.enabled = true;
  let release, count = 0;
  h.videoHook(async () => ++count === 1 ? await new Promise(resolve => { release=resolve; }) : {ok:true,...h.native});
  await h.report(800); await h.report(900); await h.report(1000);
  assert.equal(videos(h).length,1);
  release({ok:true,...h.native}); await flush();
  assert.equal(videos(h).length,2);
  assert.equal(videos(h)[1].body.rect.width,1000);
  assert.ok(videos(h)[1].body.sequence>videos(h)[0].body.sequence);
});
test('explicit OFF defeats a delayed enabled video response even if the receiver ignores abort', async () => {
  const h = workerHarness(); h.native.enabled = true;
  let release, count=0;
  h.videoHook(async () => ++count===1 ? await new Promise(resolve=>{release=resolve;}) : {ok:true,...h.native});
  await h.report(); assert.ok(release);
  const reply=await h.popup({type:'veyra-control',enabled:false});
  assert.equal(reply.enabled,false);
  release({ok:true,enabled:true,overlayVisible:true,srActive:true}); await flush();
  assert.equal(h.state.enabled,false); assert.equal(h.badges.at(-1),'');
  assert.equal(stops(h).at(-1).body.reason,'toggle-off');
  assert.ok(controls(h).every(call=>call.body.enabled===false));
});
test('manifest has no native host/frame relay and endpoint is loopback only', () => {
  const manifest = JSON.parse(source('manifest.json'));
  assert.equal(manifest.version, '0.2.3'); assert.ok(manifest.key);
  assert.ok(!manifest.permissions.includes('nativeMessaging'));
  assert.deepEqual(manifest.content_scripts[0].js, ['video-geometry.js','content.js']);
  assert.ok(!source('content.js').includes('new VideoFrame'));
  assert.ok(!source('service-worker.js').includes('sendNativeMessage'));
});
test('a frozen background tab cannot block ON status polling or foreground video delivery',async()=>{
  const h=workerHarness();h.native.enabled=true;
  h.chrome.tabs.query=async()=>[{id:7,windowId:3,active:true},{id:99,windowId:3,active:false}];
  h.chrome.tabs.sendMessage=async(id,message)=>id===99&&message.type==='veyra-enabled-changed'?await new Promise(()=>{}):undefined;
  await h.report();assert.equal(videos(h).length,1);assert.equal(h.state.online,true);
  for(let n=0;n<5;n++){await h.advance(2100);await h.report();}
  assert.ok(h.calls.filter(c=>c.url.endsWith('/status')).length>=6);
  assert.equal(videos(h).length,6);assert.equal(controls(h).length,0);
  const reply=await h.popup({type:'veyra-control',enabled:false});
  assert.equal(reply.enabled,false);assert.equal(h.native.enabled,false);
});
test('an old ON tab query cannot broadcast after a newer explicit OFF',async()=>{
  const h=workerHarness();h.native.enabled=true;
  let release,count=0;
  h.chrome.tabs.query=async()=>++count===1?await new Promise(resolve=>{release=resolve;}):[{id:7,windowId:3,active:true}];
  await h.report();assert.ok(release);
  await h.popup({type:'veyra-control',enabled:false});
  release([{id:7,windowId:3,active:true}]);await flush();
  const broadcasts=h.sent.filter(item=>item.message.type==='veyra-enabled-changed');
  assert.ok(broadcasts.length>0);assert.ok(broadcasts.every(item=>item.message.enabled===false));
});
test('a hung old ON storage write does not block OFF and later persists only the latest state',async()=>{
  const h=workerHarness();await flush();h.native.enabled=true;
  let release,blocked=false,inFlight=0,maxInFlight=0;
  h.storageHook(async patch=>{
    inFlight++;maxInFlight=Math.max(maxInFlight,inFlight);
    if(patch.enabled&&!blocked){blocked=true;await new Promise(resolve=>{release=resolve;});}
    inFlight--;
  });
  await h.report();assert.ok(release);assert.equal(videos(h).length,1);
  const reply=await h.popup({type:'veyra-control',enabled:false});assert.equal(reply.enabled,false);
  await h.advance(1000);const status=await h.popup({type:'veyra-status'});
  assert.equal(status.enabled,false);assert.match(status.connection.persistenceError,/超时/);
  release();await flush();
  assert.equal(h.state.enabled,false);assert.equal(maxInFlight,1);
  assert.equal((await h.popup({type:'veyra-status'})).enabled,false);
});
test('initial storage failure recovers and never prevents native status from being read',async()=>{
  let reads=0;
  const h=workerHarness(false,{storageGetHook:async(defaults,state)=>{
    if(++reads===1)throw new Error('synthetic initialization failure');return {...defaults,...state};
  }});
  await h.report();let status=await h.popup({type:'veyra-status'});
  assert.equal(status.online,true);assert.match(status.connection.initializationError,/synthetic/);
  await h.advance(2100);await h.report();status=await h.popup({type:'veyra-status'});
  assert.ok(reads>=2);assert.equal(status.connection.initializationError,'');
  assert.equal(controls(h).length,0);
});
test('a hung initialization read is bounded while status remains available immediately',async()=>{
  const h=workerHarness(false,{storageGetHook:()=>new Promise(()=>{})});
  const status=await h.popup({type:'veyra-status'});assert.equal(status.online,true);
  const control=h.popup({type:'veyra-control',enabled:true});await h.advance(500);
  assert.equal((await control).enabled,true);
  await h.report();assert.equal(videos(h).length,1);
});
test('rejected storage publications retry without turning confirmed native ON into offline',async()=>{
  const h=workerHarness();await flush();h.native.enabled=true;let fail=true;
  h.storageHook(async()=>{if(fail)throw new Error('synthetic persistence failure');});
  await h.report();const status=await h.popup({type:'veyra-status'});
  assert.equal(status.enabled,true);assert.equal(status.online,true);assert.equal(videos(h).length,1);
  assert.match(status.connection.persistenceError,/synthetic/);
  fail=false;await h.advance(2100);await h.report();
  assert.equal(h.state.enabled,true);assert.equal(h.state.connection.persistenceError,'');
});
test('a selected page with stale OFF storage receives native ON without controlling native',async()=>{
  const h=workerHarness();h.native.enabled=true;
  await h.report(800,{}, {}, {candidate:null,diagnostic:{code:'disabled',videoDetected:null}});
  assert.ok(h.sent.some(item=>item.id===7&&item.message.type==='veyra-enabled-changed'&&item.message.enabled));
  assert.equal(controls(h).length,0);assert.equal(videos(h).length,0);
  await h.report();assert.equal(videos(h).length,1);
});
test('a selected page with stale ON storage receives native OFF and cannot send enhanced video',async()=>{
  const h=workerHarness();await h.report();
  assert.ok(h.sent.some(item=>item.id===7&&item.message.type==='veyra-enabled-changed'&&!item.message.enabled));
  assert.equal(controls(h).length,0);assert.equal(videos(h).length,0);
  assert.equal(h.state.enabled,false);
});
test('a hung optional focus rescan cannot block an accepted ON control reply',async()=>{
  const h=workerHarness();h.focusedHook(()=>new Promise(()=>{}));
  const reply=await h.popup({type:'veyra-control',enabled:true});
  assert.equal(reply.enabled,true);assert.equal(reply.online,true);assert.equal(h.native.enabled,true);
  await h.advance(800);const state=await h.popup({type:'veyra-status'});
  assert.equal(state.enabled,true);assert.match(state.connection.rescanError,/重新检测超时/);
});
test('an ignored network abort still releases shared status and late ON cannot overwrite recovered OFF',async()=>{
  const h=workerHarness();let release;
  h.statusHook(()=>new Promise(resolve=>{release=resolve;}));
  const pending=h.popup({type:'veyra-status'});await flush();await h.advance(1500);
  const timedOut=await pending;assert.equal(timedOut.enabled,false);assert.equal(timedOut.online,false);
  h.statusHook(null);const recovered=await h.popup({type:'veyra-status'});assert.equal(recovered.online,true);
  release({enabled:true,overlayVisible:true,srActive:true});await flush();
  assert.equal((await h.popup({type:'veyra-status'})).enabled,false);
  assert.equal(controls(h).length,0);assert.equal(videos(h).length,0);
});
test('confirmed online ON delivers fresh metadata without awaiting a pending status poll',async()=>{
  const h=workerHarness();h.native.enabled=true;await h.report();
  let release;h.statusHook(()=>new Promise(resolve=>{release=resolve;}));
  await h.advance(2100);await h.report();
  assert.ok(release);assert.equal(videos(h).length,2);
  assert.equal(videos(h)[1].at,12100);assert.equal(h.state.enabled,true);
  release({...h.native});await flush();assert.equal(controls(h).length,0);
});
test('a 30ms initial status delay cannot make worker dedupe skip the next metadata heartbeat',async t=>{
  const h=workerHarness();h.native.enabled=true;
  let release;h.statusHook(()=>new Promise(resolve=>{release=resolve;}));
  await h.report();await h.advance(30);release({...h.native});await flush();h.statusHook(null);
  await h.advance(1970);await h.report();await h.advance(2000);await h.report();
  const at=videos(h).map(call=>call.at);
  assert.deepEqual(at,[10030,12000,14000]);assert.equal(controls(h).length,0);
  t.diagnostic(JSON.stringify({nativeVideoAt:at,gaps:[1970,2000],oldObservedGap:3970,nativeTTL:2500}));
});
test('unknown native state still awaits authoritative status before delivering any video',async()=>{
  const h=workerHarness(true);h.native.enabled=true;
  let release;h.statusHook(()=>new Promise(resolve=>{release=resolve;}));
  await h.report();await h.advance(1000);assert.equal(videos(h).length,0);
  release({...h.native});await flush();assert.equal(videos(h).length,1);
  assert.equal(controls(h).length,0);
});
test('an older status ON cannot revive native OFF accepted from a newer video response',async()=>{
  const h=workerHarness();h.native.enabled=true;await h.report();
  let release;h.statusHook(()=>new Promise(resolve=>{release=resolve;}));
  h.videoHook(async()=>({ok:true,enabled:false,status:'disabled',overlayVisible:false}));
  await h.advance(2100);await h.report();assert.equal(h.state.enabled,false);
  release({enabled:true,status:'running',overlayVisible:true,srActive:true});await flush();
  assert.equal(h.state.enabled,false);assert.equal(h.badges.at(-1),'');
  assert.equal(controls(h).length,0);
});
test('an older poll failure cannot mark a newer healthy video response offline',async()=>{
  const h=workerHarness();h.native.enabled=true;await h.report();
  let reject;h.statusHook(()=>new Promise((_,bad)=>{reject=bad;}));
  await h.advance(2100);await h.report();assert.equal(videos(h).length,2);
  reject(new Error('obsolete status failure'));await flush();
  assert.equal(h.state.online,true);assert.equal(h.state.enabled,true);
  assert.equal(h.state.serverStatus.error,undefined);
});
test('newer status OFF is accepted even if an older video ON completes first',async()=>{
  const h=workerHarness();h.native.enabled=true;await h.report();
  let releaseVideo,releaseStatus;
  h.videoHook(()=>new Promise(resolve=>{releaseVideo=resolve;}));
  await h.advance(1000);await h.report();assert.ok(releaseVideo);
  h.statusHook(()=>new Promise(resolve=>{releaseStatus=resolve;}));
  const state=h.popup({type:'veyra-status'});await flush();assert.ok(releaseStatus);
  releaseVideo({ok:true,enabled:true,status:'running',overlayVisible:true});await flush();
  releaseStatus({enabled:false,status:'disabled',overlayVisible:false});
  const reply=await state;assert.equal(reply.enabled,false);assert.equal(h.state.enabled,false);
  assert.equal(controls(h).length,0);
});
test('an older video ON completing after accepted status OFF cannot restore enhancement',async()=>{
  const h=workerHarness();h.native.enabled=true;await h.report();
  let release;h.videoHook(()=>new Promise(resolve=>{release=resolve;}));
  await h.advance(1000);await h.report();assert.ok(release);
  h.native.enabled=false;const state=await h.popup({type:'veyra-status'});assert.equal(state.enabled,false);
  release({ok:true,enabled:true,status:'running',overlayVisible:true});await flush();
  assert.equal(h.state.enabled,false);assert.equal(h.badges.at(-1),'');
});
test('cached online ON still rechecks actual foreground while a status poll is pending',async()=>{
  const h=workerHarness();h.native.enabled=true;await h.report();
  let release;h.statusHook(()=>new Promise(resolve=>{release=resolve;}));
  h.setFocused(false);await h.advance(2100);await h.report();
  assert.ok(release);assert.equal(videos(h).length,1);
  assert.equal(h.state.sourceDiagnostic.code,'foreground-mismatch');
  release({...h.native});await flush();await h.advance(400);
  assert.equal(videos(h).length,1);assert.equal(controls(h).length,0);
});
test('real content timer and worker dedupe keep steady metadata below native TTL despite phase and status jitter',async t=>{
  // Reuse the actual content VM setup without registering its test cases.
  const file=fs.readFileSync(path.join(__dirname,'video-detection.test.cjs'),'utf8');
  const setup=file.slice(0,file.indexOf('\ntest('));
  const {contentHarness,flush:contentFlush}=new Function('require','__dirname',setup+'\nreturn {contentHarness,flush};')(require,__dirname);
  const content=contentHarness();await contentFlush();content.advance(0);
  content.advance(151);content.handlers.visibilitychange();content.advance(0);content.advance(6000);
  const reports=content.messages.filter(item=>item.message.candidate);
  const h=workerHarness();h.native.enabled=true;
  let release;h.statusHook(()=>new Promise(resolve=>{release=resolve;}));
  let now=10000;
  await h.report(800,reports[0].message.candidate,{},reports[0].message);
  await h.advance(30);now+=30;release({...h.native});await flush();h.statusHook(null);
  for(const item of reports.slice(1)){
    await h.advance(item.at-now);now=item.at;
    await h.report(800,item.message.candidate,{},item.message);
  }
  const received=videos(h).map(call=>call.at),gaps=received.slice(1).map((at,n)=>at-received[n]);
  assert.ok(received.length>=6);assert.ok(gaps.every(gap=>gap<2500));
  assert.equal(Math.max(...gaps),1620);assert.equal(controls(h).length,0);
  t.diagnostic(JSON.stringify({contentAt:reports.map(item=>item.at),nativeVideoAt:received,maxGap:Math.max(...gaps),nativeTTL:2500}));
});

function grantMonitorContinuation(h, patch = {}) {
  Object.assign(h.native, { ownMonitorForeground: true, boundBrowserWindowId: 3,
    boundTabId: 7, boundDocumentId: 'doc-a', boundVideoId: 'video-a', boundSourceEpoch: 0 }, patch);
}
test('native monitor foreground continues only the already-bound playing source and actual browser focus stays false', async () => {
  const h=workerHarness();h.native.enabled=true;await h.report();grantMonitorContinuation(h);
  await h.focus(false);await h.advance(400);assert.equal(stops(h).length,0);
  await h.report();assert.equal(videos(h).length,2);
  const payload=videos(h).at(-1).body;
  assert.equal(payload.windowInfo.focused,false);assert.equal(payload.windowInfo.tabId,7);
  assert.equal(payload.mediaInfo.videoId,'video-a');assert.equal(payload.mediaInfo.sourceEpoch,0);
  assert.equal(payload.viewport.devicePixelRatio,1.5);assert.equal(payload.viewport.pageZoom,1);
  assert.equal(Object.hasOwn(payload,'ownMonitorForeground'),false);
  assert.equal(controls(h).length,0);assert.equal(h.state.enabled,true);
});
test('native monitor hint cannot start an unbound inactive tab even with native ON', async () => {
  const h=workerHarness();h.native.enabled=true;grantMonitorContinuation(h);h.setFocused(false);
  await h.report();await h.advance(500);
  assert.equal(videos(h).length,0);assert.equal(stops(h).length,0);assert.equal(controls(h).length,0);
});
test('page monitor claims cannot grant continuation without authoritative native permission', async () => {
  const h=workerHarness();h.native.enabled=true;await h.report();await h.focus(false);
  await h.report(800,{}, {}, {ownMonitorForeground:true,boundBrowserWindowId:3,boundTabId:7});
  await h.advance(400);assert.equal(stops(h).length,1);assert.equal(videos(h).length,1);
});
test('native monitor hint requires exact window tab document media and epoch identity', async () => {
  for(const patch of [{ownMonitorForeground:1},{boundBrowserWindowId:4},{boundTabId:8},
      {boundDocumentId:'doc-b'},{boundVideoId:'background-animation'},{boundSourceEpoch:1}]) {
    const h=workerHarness();h.native.enabled=true;await h.report();grantMonitorContinuation(h,patch);
    await h.focus(false);await h.advance(400);await h.report();
    assert.equal(stops(h).length,1,JSON.stringify(patch));assert.equal(videos(h).length,1,JSON.stringify(patch));
  }
});
test('minimized browser refuses native monitor continuation and preserves native ON preference', async () => {
  const h=workerHarness();h.native.enabled=true;await h.report();grantMonitorContinuation(h);
  h.setWindowState('minimized');await h.focus(false);await h.advance(400);await h.report();
  assert.equal(stops(h).length,1);assert.equal(videos(h).length,1);assert.equal(h.state.enabled,true);
});
test('pause and ended-like nonplaying reports stop monitor capture without turning the switch off', async () => {
  for(const properties of [{playing:false},{playing:false,ended:true}]) {
    const h=workerHarness();h.native.enabled=true;await h.report();grantMonitorContinuation(h);
    await h.focus(false);await h.advance(400);await h.report(800,properties);
    assert.equal(stops(h).at(-1).body.reason,'video-paused');assert.equal(h.state.enabled,true);
    const count=videos(h).length;await h.report(800,properties);assert.equal(videos(h).length,count);
    assert.equal(stops(h).length,1);await h.report();assert.equal(videos(h).length,count+1);
    assert.equal(videos(h).at(-1).body.windowInfo.focused,false);
    assert.equal(controls(h).length,0);
  }
});
test('monitor source replacement or epoch change stops immediately and cannot capture another animation', async () => {
  for(const mediaInfo of [{videoId:'background-animation',sourceEpoch:0},{videoId:'video-a',sourceEpoch:1}]) {
    const h=workerHarness();h.native.enabled=true;await h.report();grantMonitorContinuation(h);
    await h.focus(false);await h.advance(400);await h.report(800,{mediaInfo});
    assert.equal(stops(h).at(-1).body.reason,'monitor-source-changed');assert.equal(videos(h).length,1);
    await h.advance(1000);await h.report(800,{mediaInfo});assert.equal(videos(h).length,1);
  }
});
test('tab changes beneath monitor stop existing capture and never scan the new inactive tab', async () => {
  const h=workerHarness();h.native.enabled=true;await h.report();grantMonitorContinuation(h);
  await h.focus(false);await h.advance(400);await h.activate(8);
  assert.equal(stops(h).at(-1).body.reason,'active-tab-changed');
  assert.ok(!h.sent.some(item=>item.id===8&&item.message.type==='veyra-rescan'));
  await h.report(800,{}, {tab:{id:8,windowId:3},documentId:'doc-b'});assert.equal(videos(h).length,1);
});
test('monitor permission revoked by another foreground app stops and cannot revive in background', async () => {
  const h=workerHarness();h.native.enabled=true;await h.report();grantMonitorContinuation(h);
  await h.focus(false);await h.advance(400);await h.report();
  h.native.ownMonitorForeground=false;await h.focus(false);await h.advance(400);
  assert.equal(stops(h).length,1);const count=videos(h).length;
  await h.advance(2100);await h.report();assert.equal(videos(h).length,count);assert.equal(controls(h).length,0);
});
test('expired monitor authority cannot admit metadata while a status read is delayed', async () => {
  const h=workerHarness();h.native.enabled=true;await h.report();grantMonitorContinuation(h);
  await h.focus(false);await h.advance(400);await h.report();
  let release;h.statusHook(()=>new Promise(resolve=>{release=resolve;}));
  await h.advance(2501);const count=videos(h).length;await h.report();
  assert.equal(videos(h).length,count);assert.ok(release);
  h.native.ownMonitorForeground=false;h.statusHook(null);
  release({...h.native});await flush();await h.advance(400);
  assert.equal(stops(h).length,1);assert.equal(videos(h).length,count);
});

test('paused monitor binding cannot resume after a foreign foreground transition', async () => {
  const h=workerHarness();h.native.enabled=true;await h.report();grantMonitorContinuation(h);
  await h.focus(false);await h.advance(400);await h.report(800,{playing:false});
  h.native.ownMonitorForeground=false;await h.focus(false);await h.advance(400);
  const count=videos(h).length;grantMonitorContinuation(h);await h.advance(2100);await h.report();
  assert.equal(videos(h).length,count);assert.equal(stops(h).length,1);assert.equal(h.state.enabled,true);
});
test('paused monitor binding rejects a replaced video or document instead of resuming an inactive source', async () => {
  for(const [properties,sender] of [[{mediaInfo:{videoId:'background-animation',sourceEpoch:0}},{}],
      [{},{documentId:'doc-b'}]]) {
    const h=workerHarness();h.native.enabled=true;await h.report();grantMonitorContinuation(h);
    await h.focus(false);await h.advance(400);await h.report(800,{playing:false});
    const count=videos(h).length;await h.report(800,properties,sender);await h.report();
    assert.equal(videos(h).length,count);assert.equal(stops(h).length,1);assert.equal(controls(h).length,0);
  }
});
