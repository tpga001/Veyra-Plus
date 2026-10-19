// SPDX-License-Identifier: GPL-3.0-or-later
// Foreground/revision safeguards adapted from Magpie-VideoEnhance service-worker.js.
// Transport is new: metadata-only loopback HTTP, never launches an application.
// Source snapshot 931be4541e339612f39c80bca224dd4687872fcd; see README.md.
const ENDPOINT = "http://127.0.0.1:24861";
const EXTENSION_VERSION = "0.2.3";
const HEARTBEAT_MS = 2000, FOCUS_LOSS_GRACE_MS = 400, REQUEST_TIMEOUT_MS = 1500;
const METADATA_DEDUP_MS = 500, MONITOR_AUTHORITY_TTL_MS = 2500;
const SIDE_EFFECT_TIMEOUT_MS = 800, INITIALIZE_TIMEOUT_MS = 500;
const sessionId = crypto.randomUUID();
let clientId = crypto.randomUUID(), sequence = 0, enabled = false, online = false;
let generation = 0, reportRevision = 0, focusEventRevision = 0, controlRevision = 0;
// Order native authority by request start, not completion: late status/video
// responses or failures cannot replace a newer accepted native result.
let nativeRequestRevision = 0, nativeAcceptedRevision = 0;
let activeTabId = null, activeDocument = "", lastPayload = "", lastDeliveredAt = 0;
let activeWindowId = null, activeMedia = null, hasPlayingReport = false, nativeAuthorityAt = -Infinity;
let focusLossTimer = 0, pendingDelivery = null, deliveryRunning = false, deliveryAbort = null;
let statusPromise = null, statusAbort = null, lastStatusAt = -Infinity;
let messageRevision = 0, lastStatusSuccessAt = 0, publicationRevision = 0;
let initializationPromise = null, initialized = false, identityLocked = false, initializeRetryAt = 0;
let initializationError = "", persistenceError = "", broadcastError = "", rescanError = "", stateVersion = Date.now();
let stateWriteRunning = false, stateWritePending = false, stateWriteRetryAt = 0;
const latestMessageByTab = new Map();
let sourceDiagnostic = { code: "waiting-video", videoDetected: null, playing: null,
  foreground: false, updatedAt: 0 };
let serverStatus = { enabled: false, status: "offline", overlayVisible: false };
function bounded(operation, milliseconds, message) {
  let timer;
  return Promise.race([Promise.resolve(operation), new Promise((_, reject) => {
    timer = setTimeout(() => reject(new Error(message)), milliseconds);
  })]).finally(() => clearTimeout(timer));
}
function ensureReady() {
  if (initialized || Date.now() < initializeRetryAt) return Promise.resolve();
  if (initializationPromise) return initializationPromise;
  initializationPromise = (async () => {
    try {
      const saved = await bounded(chrome.storage.local.get({ clientId: "", enabled: false }),
        INITIALIZE_TIMEOUT_MS, "扩展初始化超时，将稍后重试");
      if (!identityLocked && typeof saved.clientId === "string" && saved.clientId) clientId = saved.clientId;
      initialized = true; initializationError = "";
    } catch (error) {
      initializationError = error?.message || "扩展初始化失败，将稍后重试";
      initializeRetryAt = Date.now() + HEARTBEAT_MS;
    } finally {
      initializationPromise = null;
      // Saved ON is never authority. Publication and badge cannot block startup.
      void publishState();
    }
  })();
  return initializationPromise;
}
void ensureReady();

async function updateBadge() {
  await bounded(chrome.action.setBadgeBackgroundColor({ color: "#168A45" }), SIDE_EFFECT_TIMEOUT_MS, "扩展图标更新超时");
  const enhancing = serverStatus.overlayVisible && (serverStatus.srActive || serverStatus.fgActive || serverStatus.nrActive);
  await bounded(chrome.action.setBadgeText({ text: enabled ? (enhancing ? "RUN" : "ON") : "" }), SIDE_EFFECT_TIMEOUT_MS, "扩展图标更新超时");
}
function connectionState() {
  return { workerVersion: EXTENSION_VERSION, statusMethod: "POST", lastStatusSuccessAt,
    initializationError, persistenceError, broadcastError, rescanError };
}
function storedState() {
  return { clientId, enabled, online, serverStatus, sourceDiagnostic, connection: connectionState() };
}
function publishState() {
  ++publicationRevision;
  stateWritePending = true;
  if (!stateWriteRunning && Date.now() >= stateWriteRetryAt) void drainStateWrites();
  void updateBadge().catch(() => {});
  return Promise.resolve();
}
async function drainStateWrites() {
  stateWriteRunning = true;
  try {
    while (stateWritePending) {
      stateWritePending = false;
      const revision = publicationRevision;
      let timedOut = false;
      const timer = setTimeout(() => {
        timedOut = true; persistenceError = "状态保存超时；连接与开关仍以 Veyra 实际状态为准";
        stateWritePending = true;
      }, SIDE_EFFECT_TIMEOUT_MS);
      try {
        // At most one actual write is in flight. A hung Chrome API cannot block
        // status/control, and its late completion is followed by the newest state.
        await chrome.storage.local.set(storedState());
        const hadError = Boolean(persistenceError);
        persistenceError = "";
        if (revision !== publicationRevision || timedOut || hadError) stateWritePending = true;
      } catch (error) {
        persistenceError = error?.message || "状态保存失败；稍后重试";
        stateWritePending = true; stateWriteRetryAt = Date.now() + HEARTBEAT_MS;
        break;
      } finally { clearTimeout(timer); }
    }
  } finally { stateWriteRunning = false; }
}
function popupState() {
  return { ...serverStatus, enabled, online, sourceDiagnostic,
    connection: connectionState() };
}
async function recordSource(message, foreground, code) {
  const video = message.candidate, detail = message.diagnostic;
  sourceDiagnostic = { code, foreground, updatedAt: Date.now(),
    videoDetected: typeof detail?.videoDetected === "boolean" ? detail.videoDetected : video ? true : null,
    playing: video ? Boolean(video.playing) : null,
    sourceWidth: Number.isInteger(video?.sourceSize?.width) ? video.sourceSize.width : 0,
    sourceHeight: Number.isInteger(video?.sourceSize?.height) ? video.sourceSize.height : 0 };
  // Only coarse current diagnostics are stored. No URL, title or pixel data.
  await publishState();
}
function broadcastEnabled() {
  const version = stateVersion = Math.max(Date.now(), stateVersion + 1), next = enabled;
  void (async () => {
    const tabs = await bounded(chrome.tabs.query({}), SIDE_EFFECT_TIMEOUT_MS, "标签状态查询超时");
    if (version !== stateVersion) return;
    const results = await Promise.allSettled(tabs.map(tab => tab.id == null || version !== stateVersion ? Promise.resolve() :
      sendEnabled(tab.id, next, version)));
    if (version === stateVersion) broadcastError = results.some(result => result.status === "rejected") ?
      "部分后台标签暂未响应；当前视频与开关仍可继续使用" : "";
  })().catch(error => { if (version === stateVersion) broadcastError = error?.message || "标签同步失败"; });
  return Promise.resolve();
}
function sendEnabled(tabId, next = enabled, version = stateVersion) {
  return bounded(Promise.resolve().then(() => chrome.tabs.sendMessage(tabId,
    { type: "veyra-enabled-changed", enabled: next, stateVersion: version })), SIDE_EFFECT_TIMEOUT_MS,
    "部分后台标签暂未响应；当前视频与开关仍可继续使用");
}
function invalidate() {
  ++generation; ++reportRevision; ++sequence; lastPayload = ""; lastDeliveredAt = 0;
  deliveryAbort?.abort(); pendingDelivery = null;
}
async function applyStatus(value, authority) {
  if (authority < nativeAcceptedRevision) return;
  nativeAcceptedRevision = authority; nativeAuthorityAt = Date.now();
  const next = Boolean(value.enabled), changed = enabled !== next;
  online = true; serverStatus = { ...value, enabled: next };
  if (changed) {
    invalidate(); enabled = next;
    // Native OFF has already stopped capture; this read-only poll needs no stop.
    if (!enabled) { activeTabId = null; activeDocument = ""; activeWindowId = null; activeMedia = null; hasPlayingReport = false; rescanError = ""; }
    await broadcastEnabled();
  }
  await publishState();
}
async function markOffline(error, authority) {
  if (authority < nativeAcceptedRevision) return;
  nativeAcceptedRevision = authority;
  const changed = enabled;
  invalidate(); enabled = false; online = false;
  activeTabId = null; activeDocument = ""; activeWindowId = null; activeMedia = null; hasPlayingReport = false; nativeAuthorityAt = -Infinity;
  serverStatus = { enabled: false, status: "offline", overlayVisible: false,
    error: error?.message || "无法连接 Veyra" };
  if (changed) await broadcastEnabled();
  await publishState();
}
async function request(path, body, signal) {
  const response = await fetch(ENDPOINT + path, {
    method: body ? "POST" : "GET", cache: "no-store", credentials: "omit",
    headers: body ? { "Content-Type": "application/json" } : undefined,
    body: body ? JSON.stringify(body) : undefined, signal
  });
  const result = await response.json();
  if (!result || typeof result !== "object") throw new Error("Veyra 状态格式无效");
  if (!response.ok || result.ok === false) {
    const error = new Error(result.error || "Veyra 连接返回 " + response.status);
    error.httpStatus = response.status;
    throw error;
  }
  return result;
}
async function refreshStatus(force = false) {
  // Native status does not depend on Chrome storage, badges or tab fanout.
  void ensureReady();
  if (statusPromise) return statusPromise;
  if (!force && Date.now() - lastStatusAt < HEARTBEAT_MS) return serverStatus;
  const revision = controlRevision, authority = ++nativeRequestRevision, controller = new AbortController();
  statusAbort = controller;
  const timeout = setTimeout(() => controller.abort(), REQUEST_TIMEOUT_MS);
  statusPromise = (async () => {
    try {
      // Host-authorized Chromium GET requests can omit Origin. A read-only
      // POST retains the fixed extension Origin without relaxing native auth.
      const result = await bounded(request("/v1/status", { schema: 1 }, controller.signal),
        REQUEST_TIMEOUT_MS, "Veyra 响应超时，请检查本地连接");
      if (revision === controlRevision && authority >= nativeAcceptedRevision) {
        if (typeof result.enabled !== "boolean") throw new Error("Veyra 状态缺少开关");
        lastStatusSuccessAt = Date.now();
        await applyStatus(result, authority);
      }
    } catch (error) {
      if (revision === controlRevision && authority >= nativeAcceptedRevision) await markOffline(error, authority);
    } finally {
      clearTimeout(timeout); lastStatusAt = Date.now();
      if (statusAbort === controller) statusAbort = null;
      statusPromise = null;
    }
    return serverStatus;
  })();
  return statusPromise;
}
async function selectedContext() {
  const window = await chrome.windows.getLastFocused({ populate: true });
  if (!window) return null;
  const tab = (window.tabs || []).find(tab => tab.active);
  return tab?.id == null ? null : { window, tab };
}
async function focusedContext() {
  const context = await selectedContext();
  return context?.window.focused ? context : null;
}
function monitorContextAllowed(context, documentId = activeDocument, media = activeMedia) {
  // Only native can grant this narrow continuation. A page or stored ON value
  // cannot select another inactive tab, document, source or browser window.
  return enabled && online && context && !context.window.focused && context.window.state !== "minimized" &&
    Date.now() - nativeAuthorityAt <= MONITOR_AUTHORITY_TTL_MS && serverStatus.ownMonitorForeground === true &&
    activeTabId !== null && context.tab.id === activeTabId && context.window.id === activeWindowId &&
    documentId === activeDocument && activeDocument.length > 0 && media && activeMedia &&
    media.videoId === activeMedia.videoId && media.sourceEpoch === activeMedia.sourceEpoch &&
    serverStatus.boundBrowserWindowId === activeWindowId && serverStatus.boundTabId === activeTabId &&
    serverStatus.boundDocumentId === activeDocument && serverStatus.boundVideoId === activeMedia.videoId &&
    serverStatus.boundSourceEpoch === activeMedia.sourceEpoch;
}
function queuePayload(payload) {
  identityLocked = true;
  pendingDelivery = payload; // capacity one: newer geometry or stop wins
  if (!deliveryRunning) void drainDelivery();
}
function clearSelectedSource() {
  activeTabId = null; activeDocument = ""; activeWindowId = null; activeMedia = null; hasPlayingReport = false;
}
function queueStop(reason, retainPausedIdentity = false) {
  hasPlayingReport = false;
  if (!retainPausedIdentity) clearSelectedSource();
  lastPayload = ""; lastDeliveredAt = 0;
  if (!clientId) return;
  queuePayload({ schema: 1, clientId, sessionId, sequence: ++sequence, type: "stop", reason });
}
async function drainDelivery() {
  deliveryRunning = true;
  try {
    while (pendingDelivery) {
      const payload = pendingDelivery; pendingDelivery = null;
      if (payload.type === "video" && !enabled) continue;
      const controller = new AbortController(), authority = ++nativeRequestRevision; deliveryAbort = controller;
      const timeout = setTimeout(() => controller.abort(), REQUEST_TIMEOUT_MS);
      try {
        const response = await bounded(request("/v1/video", payload, controller.signal),
          REQUEST_TIMEOUT_MS, "Veyra 响应超时，请检查本地连接");
        // Native sequence checks also prevent an older geometry overwriting stop.
        const explicitOffStop = payload.type === "stop" && payload.reason === "toggle-off";
        // This stop can reply before /control OFF reaches native. Its old ON
        // must not undo the user's OFF or schedule a fresh ON broadcast.
        if (!explicitOffStop && payload.sequence === sequence && typeof response.enabled === "boolean") await applyStatus(response, authority);
      } catch (error) {
        if (payload.sequence === sequence && payload.type === "video" && [400,409,422].includes(error.httpStatus)) {
          // Unsupported geometry is recoverable. Keep the user's ON preference
          // and expose the native fallback instead of claiming the server exited.
          await refreshStatus(true);
          if (online && enabled) {
            serverStatus = { ...serverStatus, statusNote: "当前视频暂时无法增强，已恢复原画面", error: error.message };
            await publishState();
          }
        } else if ((!controller.signal.aborted || !pendingDelivery) && payload.sequence === sequence) await markOffline(error, authority);
      } finally {
        clearTimeout(timeout);
        if (deliveryAbort === controller) deliveryAbort = null;
      }
    }
  } finally { deliveryRunning = false; }
}
async function suspend(reason, guard = () => true) {
  await ensureReady();
  if (!guard()) return;
  const hadPlayingSource = activeTabId !== null && hasPlayingReport;
  const retainPausedIdentity = reason === "video-paused";
  invalidate();
  if (hadPlayingSource) queueStop(reason, retainPausedIdentity);
  else if (!retainPausedIdentity) clearSelectedSource();
}
function scheduleFocusLossCheck(revision) {
  focusLossTimer = setTimeout(async () => {
    focusLossTimer = 0;
    await ensureReady();
    if (revision !== focusEventRevision) return;
    let context = await selectedContext();
    if (revision !== focusEventRevision) return;
    if (!context?.window.focused && activeTabId !== null) {
      // Discover the native monitor permission at the actual focus transition,
      // without waiting for the ordinary two-second read-only heartbeat.
      await refreshStatus(true);
      context = await selectedContext();
    }
    if (revision !== focusEventRevision) return;
    if (!context?.window.focused) {
      if (!monitorContextAllowed(context) && activeTabId !== null)
        await suspend("browser-focus-lost", () => revision === focusEventRevision);
    } else await handleFocusedWindow(context.window.id, revision);
  }, FOCUS_LOSS_GRACE_MS);
}
async function handleFocusedWindow(windowId, revision) {
  await ensureReady();
  if (revision !== focusEventRevision) return;
  const tabs = await chrome.tabs.query({ active: true, windowId });
  if (revision !== focusEventRevision) return;
  const tab = tabs[0];
  if (tab?.id == null) return;
  if (activeTabId != null && activeTabId !== tab.id) await suspend("browser-window-changed",
    () => revision === focusEventRevision && activeTabId != null && activeTabId !== tab.id);
  if (revision === focusEventRevision) chrome.tabs.sendMessage(tab.id, { type: "veyra-rescan" }).catch(() => {});
}

async function handleVideoState(message, sender) {
  await ensureReady();
  if (!sender.tab || (sender.frameId != null && sender.frameId !== 0)) return;
  const arrival = ++messageRevision;
  latestMessageByTab.set(sender.tab.id, arrival);
  const isLatestMessage = () => latestMessageByTab.get(sender.tab.id) === arrival;
  // Connectivity is read-only and independent of overlay foreground authority.
  // Known native ON can deliver fresh metadata while a read-only poll is pending.
  // Every /video reply remains authoritative; unknown/OFF still waits for status.
  if (online && enabled) void refreshStatus().catch(() => {});
  else await refreshStatus();
  if (!isLatestMessage()) return;
  const selected = await selectedContext();
  if (!isLatestMessage() || !selected || selected.tab.id !== sender.tab.id) return;
  const pageEnabled = Boolean(message.candidate) || Boolean(message.diagnostic && message.diagnostic.code !== "disabled");
  if ((message.candidate || message.diagnostic) && enabled !== pageEnabled) {
    // A new page can have stale ON or OFF storage. Synchronize both directions
    // without waiting, controlling native or measuring source frames while OFF.
    void sendEnabled(sender.tab.id).catch(() => {});
  }
  const documentId = sender.documentId || message.documentToken || "";
  const monitorContinuation = monitorContextAllowed(selected, documentId, message.candidate?.mediaInfo);
  const initial = selected.window.focused || monitorContinuation ? selected : null;
  if (!initial) {
    if (monitorContextAllowed(selected) && activeTabId !== null &&
        (documentId !== activeDocument || message.candidate?.mediaInfo &&
          (message.candidate.mediaInfo.videoId !== activeMedia.videoId ||
           message.candidate.mediaInfo.sourceEpoch !== activeMedia.sourceEpoch))) {
      const sourceGeneration = generation, boundTab = activeTabId;
      const stillBound = () => isLatestMessage() && generation === sourceGeneration && activeTabId === boundTab;
      await recordSource(message, false, "source-changed");
      if (stillBound()) await suspend("monitor-source-changed", stillBound);
      return;
    }
    await recordSource(message, false, enabled ? "foreground-mismatch" : "disabled");
    if (activeTabId !== null && !focusLossTimer) scheduleFocusLossCheck(++focusEventRevision);
    return;
  }
  if (!enabled) { await recordSource(message, true, "disabled"); return; }
  const report = ++reportRevision, currentGeneration = generation;
  const isCurrent = () => enabled && isLatestMessage() && currentGeneration === generation && report === reportRevision;
  let pageZoom, browserWindow;
  try {
    [pageZoom, browserWindow] = await Promise.all([
      chrome.tabs.getZoom(sender.tab.id), chrome.windows.get(sender.tab.windowId)
    ]);
  } catch {
    if (!isCurrent()) return;
    await recordSource(message, true, "source-unavailable");
    if (isCurrent()) await suspend("source-unavailable", isCurrent);
    return;
  }
  const current = await selectedContext();
  const continued = monitorContextAllowed(current, documentId, message.candidate?.mediaInfo);
  if (!isCurrent() || !current || current.tab.id !== sender.tab.id || browserWindow.state === "minimized" ||
      (!current.window.focused || !browserWindow.focused) && !continued) return;
  if (activeTabId !== null && (activeTabId !== sender.tab.id || activeDocument !== documentId)) {
    invalidate(); queueStop("document-changed");
    chrome.tabs.sendMessage(sender.tab.id, { type: "veyra-rescan" }).catch(() => {});
    return;
  }
  const video = message.candidate;
  if (!video || !video.playing) {
    const known = new Set(["no-video", "no-visible-main-video", "video-loading", "unsupported-geometry", "seeking", "picture-in-picture"]);
    const code = video ? (message.diagnostic?.code === "seeking" ? "seeking" : "paused") :
      known.has(message.diagnostic?.code) ? message.diagnostic.code : "waiting-video";
    await recordSource(message, true, code);
    if (!isCurrent()) return;
    if (activeTabId !== null) await suspend(video ? "video-paused" : message.reason || "no-visible-main-video", isCurrent);
    return;
  }
  const rect = video.rect, viewport = video.viewport, size = video.sourceSize;
  if (!rect || !viewport || !size || ![rect.left,rect.top,rect.width,rect.height,
      viewport.innerWidth,viewport.innerHeight,viewport.devicePixelRatio,size.width,size.height,pageZoom].every(Number.isFinite) ||
      rect.width < 64 || rect.height < 64 || size.width < 64 || size.height < 64 ||
      viewport.devicePixelRatio < 0.5 || viewport.devicePixelRatio > 8 || pageZoom <= 0 || pageZoom > 5 ||
      (video.mediaInfo && (typeof video.mediaInfo.videoId !== "string" || !video.mediaInfo.videoId.length ||
        video.mediaInfo.videoId.length > 128 || !Number.isSafeInteger(video.mediaInfo.sourceEpoch) ||
        video.mediaInfo.sourceEpoch < 0))) {
    await recordSource(message, true, "unsupported-geometry");
    if (isCurrent() && activeTabId !== null) await suspend("invalid-video-metadata", isCurrent);
    return;
  }
  await recordSource(message, true, "ready");
  if (!isCurrent()) return;
  const payload = {
    schema: 1, clientId, sessionId, type: "video", rect,
    viewport: { ...viewport, pageZoom }, sourceSize: size, playing: true,
    windowInfo: { left: browserWindow.left, top: browserWindow.top,
      width: browserWindow.width, height: browserWindow.height, focused: browserWindow.focused,
      browserWindowId: sender.tab.windowId, tabId: sender.tab.id, documentId,
      pageTitle: video.pageTitle || "", pageUrl: video.pageUrl || "" },
    uiRects: Array.isArray(video.uiRects) ? video.uiRects.slice(0,96) : [],
    frameRate: Number.isFinite(video.frameRate) ? video.frameRate : 0
  };
  if (video.mediaInfo) payload.mediaInfo = { videoId: video.mediaInfo.videoId, sourceEpoch: video.mediaInfo.sourceEpoch };
  const signature = JSON.stringify(payload);
  if (signature === lastPayload && Date.now() - lastDeliveredAt < METADATA_DEDUP_MS) return;
  if (!isCurrent()) return;
  activeTabId = sender.tab.id; activeDocument = documentId; activeWindowId = sender.tab.windowId; hasPlayingReport = true;
  activeMedia = video.mediaInfo ? { videoId: video.mediaInfo.videoId, sourceEpoch: video.mediaInfo.sourceEpoch } : null;
  lastPayload = signature; lastDeliveredAt = Date.now();
  queuePayload({ ...payload, sequence: ++sequence });
}

async function controlFromPopup(next) {
  await ensureReady();
  const revision = ++controlRevision;
  rescanError = "";
  statusAbort?.abort();
  if (statusPromise) await statusPromise;
  invalidate();
  if (!next) {
    enabled = false; queueStop("toggle-off");
    await broadcastEnabled(); await publishState();
  }
  const controller = new AbortController(), authority = ++nativeRequestRevision;
  const timeout = setTimeout(() => controller.abort(), REQUEST_TIMEOUT_MS);
  try {
    await bounded(request("/v1/control", { schema: 1, enabled: Boolean(next) }, controller.signal),
      REQUEST_TIMEOUT_MS, "Veyra 响应超时，请检查本地连接");
    lastStatusAt = -Infinity;
    await refreshStatus(true);
    if (enabled) void rescanSelected(revision);
  } catch (error) { await markOffline(error, authority); }
  finally { clearTimeout(timeout); }
  return popupState();
}
async function rescanSelected(revision) {
  try {
    const context = await bounded(focusedContext(), SIDE_EFFECT_TIMEOUT_MS, "视频窗口重新检测超时");
    if (revision !== controlRevision || !enabled || !context) return;
    await bounded(chrome.tabs.sendMessage(context.tab.id, { type: "veyra-rescan" }),
      SIDE_EFFECT_TIMEOUT_MS, "视频页面重新检测超时");
    if (revision === controlRevision && enabled) rescanError = "";
  } catch (error) {
    if (revision === controlRevision && enabled) rescanError = error?.message || "视频重新检测暂未完成";
  }
}
chrome.runtime.onMessage.addListener((message, sender, sendResponse) => {
  if (message?.type === "veyra-video-state" && sender.tab) {
    // A stale API/storage failure must not suspend a newer admitted source.
    handleVideoState(message, sender).catch(error => console.warn("Veyra metadata unavailable:", error?.message));
    return false;
  }
  const isPopup = !sender.tab && sender.url === chrome.runtime.getURL("popup.html");
  if (!isPopup) return false;
  if (message?.type === "veyra-status") {
    refreshStatus(true).then(() => sendResponse(popupState()));
    return true;
  }
  if (message?.type === "veyra-control" && typeof message.enabled === "boolean") {
    controlFromPopup(message.enabled).then(sendResponse);
    return true;
  }
  return false;
});
chrome.runtime.onInstalled.addListener(() => { void ensureReady(); });
chrome.runtime.onStartup.addListener(() => { void ensureReady(); });
chrome.tabs.onActivated.addListener(async ({ tabId, windowId }) => {
  const focusRevision = focusEventRevision, sourceGeneration = generation;
  const isCurrentEvent = () => focusRevision === focusEventRevision && sourceGeneration === generation;
  await ensureReady();
  const context = await selectedContext();
  // Changing the bound tab beneath the monitor stops the old source, but
  // cannot scan or bind the new inactive tab until the browser is foreground.
  if (!isCurrentEvent() || !context || context.window.id !== windowId || context.tab.id !== tabId) return;
  if (!context.window.focused) {
    if (activeWindowId === windowId && activeTabId !== null && activeTabId !== tabId)
      await suspend("active-tab-changed", isCurrentEvent);
    return;
  }
  // Tab changes in another browser window must not stop the foreground source.
  await suspend("active-tab-changed", isCurrentEvent);
  if (focusRevision === focusEventRevision) chrome.tabs.sendMessage(tabId, { type: "veyra-rescan" }).catch(() => {});
});
chrome.tabs.onRemoved.addListener(async tabId => {
  latestMessageByTab.delete(tabId);
  if (activeTabId === tabId) await suspend("source-tab-closed", () => activeTabId === tabId);
});
chrome.tabs.onUpdated.addListener(async (tabId, changeInfo) => {
  if (tabId === activeTabId && changeInfo.status === "loading") await suspend("source-navigation", () => activeTabId === tabId);
});
chrome.tabs.onZoomChange.addListener(({ tabId }) => {
  if (activeTabId === tabId) {
    void suspend("page-zoom-changing", () => activeTabId === tabId).then(() =>
      chrome.tabs.sendMessage(tabId, { type: "veyra-rescan" }).catch(() => {}));
  }
});
chrome.windows.onFocusChanged.addListener(async windowId => {
  const revision = ++focusEventRevision;
  if (focusLossTimer) clearTimeout(focusLossTimer);
  focusLossTimer = 0;
  if (windowId === chrome.windows.WINDOW_ID_NONE) scheduleFocusLossCheck(revision);
  else await handleFocusedWindow(windowId, revision);
});
