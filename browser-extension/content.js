// SPDX-License-Identifier: GPL-3.0-or-later
// Detection/geometry/UI exclusions adapted from Magpie-VideoEnhance content.js.
// Source snapshot 931be4541e339612f39c80bca224dd4687872fcd; see README.md.
// The worker and Veyra own foreground authority. No pixels are read here.
(() => {
  if (window.top !== window) return;
  const REPORT_DELAY_MS = 150, HEARTBEAT_MS = 1000, POLL_MS = 500;
  let enabled = false, reportTimer = 0, lastSignature = "", lastReportAt = -Infinity;
  let enabledStateVersion = -1, enabledMessageSeen = false;
  let observedVideo = null, pointerActiveUntil = 0;
  let frameCallback = 0, measuredVideo = null, frameRate = 0, rateSamples = [];
  const documentToken = crypto.randomUUID();
  const mediaStates = new WeakMap();
  let detection = { code: "no-video", videoDetected: false };
  const resizeObserver = new ResizeObserver(() => scheduleReport());

  function mediaState(video, checkSource = true) {
    const key = video.currentSrc || video.src || "";
    let state = mediaStates.get(video);
    if (!state) {
      state = { videoId: crypto.randomUUID(), sourceEpoch: 0, sourceKey: key,
        resetPending: false, seeking: false, lastSeekTime: null };
      mediaStates.set(video, state);
    } else if (checkSource && key !== state.sourceKey) {
      if (!state.resetPending) ++state.sourceEpoch;
      state.sourceKey = key;
      state.seeking = false; state.lastSeekTime = null;
      if (video === measuredVideo || video === observedVideo) { rateSamples = []; frameRate = 0; }
    }
    return state;
  }
  function mediaEvent(video, name) {
    if (!video || video.tagName !== "VIDEO") return;
    const state = mediaState(video, name !== "emptied");
    if (name === "emptied") {
      if (!state.resetPending) ++state.sourceEpoch;
      state.resetPending = true;
      state.seeking = false; state.lastSeekTime = null;
    } else if (name === "loadedmetadata") state.resetPending = false;
    else if (name === "seeking") state.seeking = true;
    else if (name === "seeked") {
      const time = Number.isFinite(video.currentTime) ? video.currentTime : null;
      if (state.seeking || time !== state.lastSeekTime) ++state.sourceEpoch;
      state.lastSeekTime = time; state.seeking = false;
    }
    if (video === measuredVideo && ["emptied", "seeking", "seeked"].includes(name)) { rateSamples = []; frameRate = 0; }
  }

  function visibleStyle(element) {
    for (let node = element, depth = 0; node && depth < 8; node = node.parentElement, ++depth) {
      const style = getComputedStyle(node);
      if (style.display === "none" || style.visibility === "hidden" || Number(style.opacity || 1) < 0.05) return false;
    }
    return true;
  }

  function chooseMainVideo() {
    let best = null;
    const viewportArea = window.innerWidth * window.innerHeight;
    const videos = document.querySelectorAll("video");
    detection = { code: videos.length ? "no-visible-main-video" : "no-video", videoDetected: videos.length > 0 };
    if (document.pictureInPictureElement) {
      detection = { code: "picture-in-picture", videoDetected: true };
      return null; // The floating surface no longer matches its DOM rectangle.
    }
    let rejectedArea = -1;
    for (const video of videos) {
      const box = video.getBoundingClientRect();
      const visibleWidth = Math.max(0, Math.min(window.innerWidth, box.right) - Math.max(0, box.left));
      const visibleHeight = Math.max(0, Math.min(window.innerHeight, box.bottom) - Math.max(0, box.top));
      const area = visibleWidth * visibleHeight;
      if (visibleWidth < 240 || visibleHeight < 135 || area < Math.max(32400, viewportArea * 0.035) || !visibleStyle(video)) continue;
      if (video.readyState < 1 || !(video.currentSrc || video.src) || !video.videoWidth || !video.videoHeight) {
        if (area > rejectedArea) { detection.code = "video-loading"; rejectedArea = area; }
        continue;
      }
      // Small muted repeating animations must not replace a paused main player.
      if (video.loop && video.muted && !video.controls && area < viewportArea * 0.25) continue;
      const geometry = globalThis.VeyraVideoGeometry(video, window, getComputedStyle(video));
      if (!geometry) {
        if (area > rejectedArea) { detection.code = "unsupported-geometry"; rejectedArea = area; }
        continue;
      }
      const media = mediaState(video);
      const playing = !video.paused && !video.ended && !video.seeking && !media.seeking && video.readyState >= 2;
      // Motion alone cannot make a thumbnail beat a substantially larger player.
      const score = area * (playing ? 1.15 : 1);
      if (!best || score > best.score) best = { video, rect: geometry.rect, geometry, playing, score, media };
    }
    if (best) detection.code = best.media.seeking || best.video.seeking ? "seeking" : best.playing ? "ready" : "paused";
    return best;
  }

  function stopRateMeasurement() {
    if (measuredVideo && frameCallback) measuredVideo.cancelVideoFrameCallback?.(frameCallback);
    measuredVideo = null; frameCallback = 0; frameRate = 0; rateSamples = [];
  }
  function measureRate(video) {
    if (measuredVideo === video) return;
    stopRateMeasurement();
    if (!video || !video.requestVideoFrameCallback) return;
    measuredVideo = video;
    const callback = (_, metadata) => {
      if (measuredVideo !== video || !enabled) return;
      frameCallback = 0;
      const t = metadata.mediaTime, frames = metadata.presentedFrames;
      if (Number.isFinite(t) && Number.isFinite(frames)) {
        const previous = rateSamples.at(-1);
        if (previous && (t <= previous.t || t - previous.t > 1 || frames <= previous.frames)) {
          rateSamples = []; frameRate = 0;
        }
        rateSamples.push({ t, frames });
        while (rateSamples.length > 2 && (t - rateSamples[0].t > 2 || rateSamples.length > 240)) rateSamples.shift();
        const first = rateSamples[0], duration = t - first.t;
        if (duration >= 0.5) {
          const fps = (frames - first.frames) / duration;
          frameRate = fps > 0 && fps <= 240 ? Math.round(fps * 1000) / 1000 : 0;
        }
      }
      frameCallback = video.requestVideoFrameCallback(callback);
    };
    frameCallback = video.requestVideoFrameCallback(callback);
  }

  function playerUiRects(rect) {
    const result = [];
    const add = box => {
      const left = Math.max(rect.left, box.left - 2), top = Math.max(rect.top, box.top - 2);
      const right = Math.min(rect.left + rect.width, box.right + 2), bottom = Math.min(rect.top + rect.height, box.bottom + 2);
      if (right > left && bottom > top) result.push([
        (left - rect.left) / rect.width, (top - rect.top) / rect.height,
        (right - rect.left) / rect.width, (bottom - rect.top) / rect.height
      ].map(x => Math.round(x * 10000) / 10000));
    };
    const selectors = ".bpx-player-control-wrap,.bpx-player-top-wrap,.bpx-player-subtitle-panel," +
      ".bpx-player-subtitle-wrap,.bpx-player-context-menu,.bpx-player-toast-wrap," +
      ".bpx-player-ending-panel,.bpx-player-setting-panel,.bili-danmaku-x-dm," +
      ".ytp-chrome-bottom,.ytp-chrome-top,.ytp-caption-window-container," +
      ".ytp-settings-menu,[role=menu],[role=slider],[role=dialog]";
    for (const element of document.querySelectorAll(selectors)) {
      if (result.length >= 90) break;
      if (visibleStyle(element)) add(element.getBoundingClientRect());
    }
    if (Date.now() < pointerActiveUntil) {
      add({ left: rect.left, right: rect.left + rect.width,
        top: rect.top + Math.max(0, rect.height - 112), bottom: rect.top + rect.height });
    }
    return result;
  }

  const rounded = value => Math.round(value * 1000) / 1000;
  function currentReport() {
    if (!enabled) return { candidate: null, documentToken, diagnostic: { code: "disabled", videoDetected: null } };
    const candidate = chooseMainVideo();
    if (candidate?.video !== observedVideo) {
      resizeObserver.disconnect(); observedVideo = candidate?.video || null;
      if (observedVideo) resizeObserver.observe(observedVideo);
      measureRate(observedVideo);
    }
    if (!candidate) { stopRateMeasurement(); return { candidate: null, documentToken, diagnostic: detection, reason: detection.code }; }
    const { rect, video, playing, media } = candidate;
    const vv = window.visualViewport;
    return { documentToken, diagnostic: detection, candidate: {
      rect: Object.fromEntries(Object.entries(rect).map(([key,value]) => [key, rounded(value)])),
      viewport: {
        innerWidth: window.innerWidth, innerHeight: window.innerHeight,
        devicePixelRatio: window.devicePixelRatio || 1,
        screenX: window.screenX, screenY: window.screenY,
        outerWidth: window.outerWidth, outerHeight: window.outerHeight,
        visualViewport: { offsetLeft: vv?.offsetLeft || 0, offsetTop: vv?.offsetTop || 0,
          width: vv?.width || window.innerWidth, height: vv?.height || window.innerHeight,
          scale: vv?.scale || 1 }
      },
      sourceSize: { width: video.videoWidth, height: video.videoHeight },
      mediaInfo: { videoId: media.videoId, sourceEpoch: media.sourceEpoch },
      playing, uiRects: playerUiRects(rect), frameRate: playing ? frameRate : 0,
      pageTitle: document.title, pageUrl: location.href
    }};
  }
  function reportNow(force = false) {
    reportTimer = 0;
    const report = currentReport(), now = Date.now();
    const signature = JSON.stringify({ candidate: report.candidate, diagnostic: report.diagnostic });
    // OFF pages send source-metadata-free status probes. Read-only connectivity
    // also works in the background; video delivery still requires foreground.
    if (!force && signature === lastSignature && now - lastReportAt < HEARTBEAT_MS) return;
    lastSignature = signature; lastReportAt = now;
    chrome.runtime.sendMessage({ type: "veyra-video-state", ...report }).catch(() => {});
  }
  function scheduleReport(force = false) {
    if (reportTimer && !force) return;
    if (reportTimer) clearTimeout(reportTimer);
    reportTimer = setTimeout(() => reportNow(force), force ? 0 : REPORT_DELAY_MS);
  }
  function setEnabled(value) {
    const next = Boolean(value);
    if (next === enabled) return;
    enabled = next;
    if (!enabled) { stopRateMeasurement(); resizeObserver.disconnect(); observedVideo = null; }
    lastSignature = ""; scheduleReport(true);
  }
  chrome.storage.local.get({ enabled: false }).then(state => {
    if (!enabledMessageSeen) setEnabled(state.enabled);
    scheduleReport(true);
  }).catch(() => scheduleReport(true));
  chrome.runtime.onMessage.addListener(message => {
    if (message?.type === "veyra-enabled-changed") {
      if (Number.isSafeInteger(message.stateVersion) && message.stateVersion >= 0) {
        if (message.stateVersion <= enabledStateVersion) return;
        enabledStateVersion = message.stateVersion;
      } else if (enabledStateVersion >= 0) return;
      enabledMessageSeen = true; setEnabled(message.enabled);
    }
    if (message?.type === "veyra-rescan") scheduleReport(true);
  });
  for (const name of ["play", "pause", "loadedmetadata", "emptied", "ended", "seeking", "seeked",
      "ratechange", "enterpictureinpicture", "leavepictureinpicture"]) {
    document.addEventListener(name, event => { mediaEvent(event?.target, name); scheduleReport(); }, true);
  }
  document.addEventListener("visibilitychange", () => scheduleReport(true), true);
  document.addEventListener("fullscreenchange", () => scheduleReport(true), true);
  window.addEventListener("resize", () => scheduleReport(), true);
  window.addEventListener("scroll", () => scheduleReport(), true);
  window.visualViewport?.addEventListener("resize", () => scheduleReport(), true);
  window.visualViewport?.addEventListener("scroll", () => scheduleReport(), true);
  document.addEventListener("pointermove", event => {
    if (!enabled || !observedVideo) return;
    const box = observedVideo.getBoundingClientRect();
    if (event.clientX >= box.left && event.clientX <= box.right && event.clientY >= box.top && event.clientY <= box.bottom) {
      const wasInactive = Date.now() >= pointerActiveUntil; pointerActiveUntil = Date.now() + 1500;
      if (wasInactive) scheduleReport(true);
    }
  }, true);
  new MutationObserver(() => scheduleReport()).observe(document.documentElement, {
    childList: true, subtree: true, attributes: true, attributeFilter: ["style", "class", "src"]
  });
  setInterval(() => scheduleReport(), POLL_MS);
})();
