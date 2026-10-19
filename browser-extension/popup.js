// SPDX-License-Identifier: GPL-3.0-or-later
const checkbox = document.getElementById("enabled"), status = document.getElementById("status");
const details = document.getElementById("details"), error = document.getElementById("error");
const retry = document.getElementById("retry");
const STATUS_TIMEOUT_MS = 2600, CONTROL_TIMEOUT_MS = 4000;
let busy = false, requestRevision = 0, refreshRevision = 0;
function sendRequest(message, milliseconds) {
  let timer;
  return Promise.race([chrome.runtime.sendMessage(message), new Promise((_, reject) => {
    timer = setTimeout(() => reject(new Error("等待扩展响应超时，请点击重新连接")), milliseconds);
  })]).finally(() => clearTimeout(timer));
}
const stateNames = { running: "运行", waiting: "等待", stopped: "停止", paused: "暂停", failed: "失败" };
function localReason(state) {
  const source = state.sourceDiagnostic;
  if (!source || Date.now() - source.updatedAt > 15000) return "";
  const reasons = {
    "foreground-mismatch": source.videoDetected ? "已检测到视频；浏览器在后台，保留原画面" : "浏览器在后台，保留原画面",
    paused: "已检测到视频；视频已暂停，保留原画面",
    seeking: "正在跳转视频位置，保留原画面",
    "video-loading": "已检测到视频；正在等待视频加载",
    "unsupported-geometry": "已检测到视频；当前画面布局暂不支持，保留原画面",
    "picture-in-picture": "视频已进入画中画；保留原画面，退出后恢复增强",
    "source-unavailable": "视频页面已变化或暂时无法读取，请等待页面重新就绪",
    "no-video": "当前页面未检测到普通视频元素",
    "no-visible-main-video": "已检测到视频；当前没有可增强的可见主视频",
    ready: "已检测到视频；等待 Veyra 接收并启动增强"
  };
  return reasons[source.code] || "";
}
function render(state) {
  checkbox.checked = Boolean(state.enabled);
  checkbox.disabled = busy;
  retry.disabled = busy;
  const detailError = state.error || state.connection?.initializationError ||
    state.connection?.persistenceError || state.connection?.broadcastError || state.connection?.rescanError;
  if (!state.online) {
    status.textContent = "未连接，请先打开 Veyra";
    details.textContent = state.connection?.lastStatusSuccessAt ? "曾连接成功，当前连接已中断" : "";
    error.hidden = !detailError;
    error.textContent = detailError ? String(detailError) : "";
    return;
  }
  if (!state.enabled) status.textContent = "已关闭";
  else if (state.overlayVisible && (state.srActive || state.fgActive || state.nrActive)) status.textContent = state.status || "正在增强当前视频";
  else if (state.overlayVisible) status.textContent = "正在显示，增强尚未确认";
  else {
    const local = localReason(state);
    const nativeSpecific = state.reasonCode && !["waiting-video", "disabled", "stopped"].includes(state.reasonCode) &&
      !(state.reasonCode === "paused" && state.sourceDiagnostic?.code === "seeking");
    status.textContent = state.statusNote || (nativeSpecific ? state.status : local || state.status) || "已开启，等待视频报告";
  }
  const parts = [];
  if (state.state) parts.push("Veyra 状态：" + (stateNames[state.state] || String(state.state)));
  const source = state.sourceDiagnostic;
  const recent = source && Date.now() - source.updatedAt <= 15000;
  const originalWidth = state.browserSourceWidth > 0 ? state.browserSourceWidth : recent ? source.sourceWidth : 0;
  const originalHeight = state.browserSourceHeight > 0 ? state.browserSourceHeight : recent ? source.sourceHeight : 0;
  if (originalWidth > 0 && originalHeight > 0) parts.push("原视频 " + originalWidth + "×" + originalHeight);
  if (state.sourceWidth > 0 && state.sourceHeight > 0) parts.push("捕获区域 " + state.sourceWidth + "×" + state.sourceHeight);
  if (state.enabled && state.overlayVisible) {
    parts.push("超分" + (state.srActive ? "运行中" : "未运行"));
    parts.push("补帧" + (state.fgActive ? "运行中" : "未运行"));
    if (state.nrActive) parts.push("画面重塑运行中");
  }
  details.textContent = parts.join(" · ");
  error.hidden = !detailError;
  error.textContent = detailError ? String(detailError) : "";
}
async function refresh(force = false) {
  if (busy || (refreshRevision && !force)) return;
  const revision = ++requestRevision;
  refreshRevision = revision;
  try {
    const state = await sendRequest({ type: "veyra-status" }, STATUS_TIMEOUT_MS);
    if (revision === requestRevision && !busy) render(state);
  } catch (failure) {
    if (revision === requestRevision && !busy) render({ enabled: false, online: false, error: failure?.message || "无法读取扩展状态，请重新连接" });
  } finally { if (refreshRevision === revision) refreshRevision = 0; }
}
checkbox.addEventListener("change", async () => {
  const next = checkbox.checked, revision = ++requestRevision;
  refreshRevision = 0;
  busy = true; checkbox.disabled = true;
  retry.disabled = true;
  try {
    const response = await sendRequest({ type: "veyra-control", enabled: next }, CONTROL_TIMEOUT_MS);
    if (revision === requestRevision) { busy = false; render(response); }
  } catch (failure) {
    if (revision === requestRevision) { busy = false; render({ enabled: false, online: false, error: failure?.message || "无法切换增强，请检查 Veyra 连接" }); }
  }
});
retry.addEventListener("click", () => { if (!busy) void refresh(true); });
void refresh();
setInterval(refresh, 1000);
