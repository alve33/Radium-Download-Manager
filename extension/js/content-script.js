// SPDX-License-Identifier: Apache-2.0
// Adapted from xifangczy/cat-catch (js/content-script.js) & Brisk Browser Extension.
// Observes <video> elements (including dynamic insertion & resize), positions the closed Shadow DOM
// "Download with Radium" cosmic pill badge, provides real-time status/error feedback on click, intercepts link clicks
// (with Alt-key bypass), captures media events, and relays page MAIN world messages.

function isVideoOrDocFullscreen(video, doc) {
  if (doc) {
    const fsEl = doc.fullscreenElement || doc.webkitFullscreenElement || doc.mozFullScreenElement || doc.msFullscreenElement;
    if (fsEl) return true;
  }
  if (video) {
    if (typeof video.closest === "function") {
      if (video.closest(".ytp-fullscreen") || video.closest(".fullscreen")) return true;
    }
    let p = video.parentElement;
    while (p) {
      if (p.classList && typeof p.classList.contains === "function" && (p.classList.contains("ytp-fullscreen") || p.classList.contains("fullscreen"))) {
        return true;
      }
      p = p.parentElement;
    }
  }
  return false;
}

function positionOverlayHost(video, hostEl) {
  if (!video || !hostEl) return;
  const doc = (video && video.ownerDocument) ? video.ownerDocument : (typeof document !== "undefined" ? document : null);

  const fsElement = doc && (doc.fullscreenElement || doc.webkitFullscreenElement || doc.mozFullScreenElement || doc.msFullscreenElement);
  const parent = fsElement || (doc && doc.body);
  if (parent && hostEl.parentElement !== parent) {
    try { parent.appendChild(hostEl); } catch (_) {}
  }

  const rect = typeof video.getBoundingClientRect === "function"
    ? video.getBoundingClientRect()
    : { width: video.width || 640, height: video.height || 360, top: 20, left: 20, right: 660, bottom: 380 };

  // Hide unobtrusively if the video player is collapsed or scrolled out of the visible viewport
  const isCollapsed = (rect.width !== undefined && rect.width < 120) || (rect.height !== undefined && rect.height < 80);
  const isOffscreen = (rect.bottom !== undefined && rect.bottom < 28) || (rect.right !== undefined && rect.right < 48);

  const viewport = doc && doc.defaultView ? doc.defaultView : (typeof window !== "undefined" ? window : null);
  const maxLeft = Math.max(8, (viewport && viewport.innerWidth ? viewport.innerWidth : 1920) - (hostEl.offsetWidth || 240) - 8);
  const maxTop = Math.max(8, (viewport && viewport.innerHeight ? viewport.innerHeight : 1080) - (hostEl.offsetHeight || 40) - 8);
  // Manual placement persists across player movement and fullscreen transitions.
  const desiredLeft = hostEl.__radiumManualPosition ? hostEl.__radiumManualPosition.left
    : Math.round(Number.isFinite(rect.left) ? rect.left : 20);
  const desiredTop = hostEl.__radiumManualPosition ? hostEl.__radiumManualPosition.top
    : Math.round((Number.isFinite(rect.top) ? rect.top : 20) - 34);
  const leftPx = Math.max(8, Math.min(maxLeft, desiredLeft));
  const topPx = Math.max(8, Math.min(maxTop, desiredTop));

  const visCss = (!hostEl.__radiumManualPosition && (isCollapsed || isOffscreen))
    ? "display:none;opacity:0;pointer-events:none;"
      : "display:inline-flex;opacity:1;visibility:visible;pointer-events:auto;";
  const nextCss = `position:fixed;top:${topPx}px;left:${leftPx}px;z-index:2147483647;${visCss}`;
  if (hostEl.__radiumLastCss !== nextCss) {
    hostEl.__radiumLastCss = nextCss;
    hostEl.style.cssText = nextCss;
  }
}

function extractVideoElementSrc(video) {
  if (!video) return "";
  let src = video.currentSrc || video.src || "";
  if (!src && typeof video.querySelector === "function") {
    const sourceEl = video.querySelector("source[src]");
    if (sourceEl) src = sourceEl.src || sourceEl.getAttribute("src") || "";
  }
  return src;
}

let g_contentScriptCleanedUp = false;
let g_videoObserver = null;
let g_messageListener = null;

function isExtensionContextValid() {
  try {
    if (typeof chrome === "undefined") {
      // In test/Node.js unit test environments where chrome is undefined, allow DOM testing
      return typeof module !== "undefined";
    }
    if (!chrome.runtime) return false;
    // In test/sandbox environments where chrome.runtime.id is not mocked, check sendMessage
    if (typeof chrome.runtime.id === "undefined") {
      return typeof chrome.runtime.sendMessage === "function";
    }
    return Boolean(chrome.runtime.id);
  } catch (_) {
    return false;
  }
}

function cleanupContentScript() {
  if (g_contentScriptCleanedUp) return;
  g_contentScriptCleanedUp = true;
  try {
    if (typeof window !== "undefined") {
      window.removeEventListener("click", handleLinkClick, { capture: true, passive: false });
      window.removeEventListener("pointerdown", recordTrustedUserGesture, { capture: true });
      window.removeEventListener("mousedown", recordTrustedUserGesture, { capture: true });
      window.removeEventListener("click", recordTrustedUserGesture, { capture: true });
      window.removeEventListener("keydown", recordTrustedUserGesture, { capture: true });
      if (g_messageListener) {
        window.removeEventListener("message", g_messageListener, { capture: true });
        g_messageListener = null;
      }
    }
    if (typeof document !== "undefined") {
      document.removeEventListener("play", handleMediaPlaybackEvent, true);
      document.removeEventListener("playing", handleMediaPlaybackEvent, true);
      document.removeEventListener("canplay", handleMediaPlaybackEvent, true);
      document.removeEventListener("loadedmetadata", handleMediaPlaybackEvent, true);
    }
    if (g_videoObserver && typeof g_videoObserver.disconnect === "function") {
      g_videoObserver.disconnect();
      g_videoObserver = null;
    }
    if (typeof document !== "undefined" && document.querySelectorAll) {
      const badges = document.querySelectorAll("[data-radium-shadow-host], [data-flux-shadow-host]");
      badges.forEach(b => { try { b.remove(); } catch (_) {} });
    }
  } catch (_) {}
}

function safeSendRuntimeMessage(msg, callback) {
  if (!isExtensionContextValid()) {
    cleanupContentScript();
    if (typeof callback === "function") callback(null, "Extension reloaded. Please refresh the page.");
    return;
  }
  try {
    chrome.runtime.sendMessage(msg, (response) => {
      const err = (typeof chrome !== "undefined" && chrome.runtime) ? chrome.runtime.lastError : null;
      const errMsg = err ? (err.message || String(err)) : null;
      if (errMsg) {
        if (errMsg.includes("context invalidated") || errMsg.includes("Extension context invalidated")) {
          cleanupContentScript();
        }
      }
      if (typeof callback === "function") {
        callback(response, errMsg);
      }
    });
  } catch (ex) {
    const exStr = ex ? ex.message : String(ex);
    if (exStr.includes("context invalidated") || exStr.includes("Extension context invalidated")) {
      cleanupContentScript();
    }
    if (typeof callback === "function") {
      callback(null, exStr || "Extension communication error");
    }
  }
}

function notifyDirectVideoElementDiscovered(video, doc) {
  if (!isExtensionContextValid() || g_contentScriptCleanedUp) return;
  try {
    const src = extractVideoElementSrc(video);
    if (!src || src.startsWith("blob:") || src.startsWith("data:")) return;
    if (video.__radiumLastReportedSrc === src) return;
    video.__radiumLastReportedSrc = src;

    const lower = src.toLowerCase();
    const streamType = lower.includes(".m3u8") ? "HLS" : (lower.includes(".mpd") ? "DASH" : "HTTP_FILE");
    safeSendRuntimeMessage({
      action: "addMedia",
      url: src,
      streamType,
      pageTitle: (doc && doc.title) ? doc.title : "video",
      tabUrl: (doc && doc.location) ? doc.location.href : ""
    });
  } catch (_) {}
}

function scanAndAttachVideoOverlays(doc, onDownloadClick, minWidth = 240, minHeight = 135) {
  if (!isExtensionContextValid() || g_contentScriptCleanedUp) return [];
  if (!doc || !doc.querySelectorAll) return [];
  const videos = Array.from(doc.querySelectorAll("video"));
  const win = doc.defaultView || (typeof window !== "undefined" ? window : null);
  const viewportW = (win && Number.isFinite(win.innerWidth) && win.innerWidth > 0) ? win.innerWidth : 0;
  const viewportH = (win && Number.isFinite(win.innerHeight) && win.innerHeight > 0) ? win.innerHeight : 0;
  const isYouTube = Boolean(doc.location && doc.location.hostname && (doc.location.hostname.includes("youtube.com") || doc.location.hostname === "youtu.be"));

  if (isYouTube && doc && doc.location && doc.location.href && !doc.__radiumYtPrewarmed) {
    doc.__radiumYtPrewarmed = true;
    safeSendRuntimeMessage({
      action: "triggerYouTubePrewarm",
      url: doc.location.href
    });
  }

  const removeDuplicateHosts = (keepHostEl = null) => {
    try {
      const existingHosts = doc.querySelectorAll("[data-radium-shadow-host], [data-flux-shadow-host], .radium-download-btn");
      existingHosts.forEach((el) => {
        if (el !== keepHostEl && typeof el.remove === "function") {
          el.remove();
        }
      });
    } catch (_) {}
  };

  let bestCandidate = null;
  let bestScore = -1;

  for (const video of videos) {
    notifyDirectVideoElementDiscovered(video, doc);

    const rect = typeof video.getBoundingClientRect === "function"
      ? video.getBoundingClientRect()
      : { width: video.width || 640, height: video.height || 360, top: 20, left: 20, right: 660, bottom: 380 };

    const isPlayerCandidate = (rect.width >= minWidth && rect.height >= minHeight) ||
      (isYouTube && (rect.width >= 160 || (video.parentElement && video.parentElement.id === "movie_player")));

    if (!isPlayerCandidate) {
      continue;
    }

    // Filter out hidden, blurred ambient background, or non-interactive videos (e.g. Facebook Reels background)
    let computed = null;
    if (win && typeof win.getComputedStyle === "function") {
      try { computed = win.getComputedStyle(video); } catch (_) {}
    }
    const styleFilter = (computed && computed.filter) || (video.style && video.style.filter) || "";
    const stylePointer = (computed && computed.pointerEvents) || (video.style && video.style.pointerEvents) || "";
    const styleDisplay = (computed && computed.display) || (video.style && video.style.display) || "";
    const styleVisibility = (computed && computed.visibility) || (video.style && video.style.visibility) || "";
    const styleOpacity = (computed && computed.opacity !== undefined && computed.opacity !== "")
      ? parseFloat(computed.opacity)
      : ((video.style && video.style.opacity !== undefined && video.style.opacity !== "") ? parseFloat(video.style.opacity) : 1);

    if (styleDisplay === "none" || styleVisibility === "hidden" || styleOpacity === 0 ||
        String(styleFilter).includes("blur") || stylePointer === "none") {
      continue;
    }

    // Filter out offscreen or preloaded Reels peeking at the top/bottom edges
    const inFullscreen = Boolean(doc.fullscreenElement && doc.fullscreenElement.contains && doc.fullscreenElement.contains(video));
    if (!inFullscreen && viewportH > 0 && Number.isFinite(rect.top) && Number.isFinite(rect.bottom)) {
      if (rect.bottom < 80 || rect.top > viewportH - 140) {
        continue;
      }
    }

    const src = extractVideoElementSrc(video);
    // On video sites like YouTube, video elements initially stream via MSE blob: or delayed src.
    // Never skip attaching the overlay button to an active video player just because src is pending.
    if (!src && !isYouTube && (!video.readyState || video.readyState === 0 || rect.width < 320 || rect.height < 180)) {
      continue;
    }

    // Score candidate to select strictly ONE primary active <video> per document
    const playingScore = (!video.paused && !video.ended) ? 1000000 : 0;
    const readyScore = (video.readyState >= 2) ? 500000 : 0;
    const fsScore = inFullscreen ? 2000000 : 0;
    let visibleArea = (rect.width || 0) * (rect.height || 0);
    if (viewportW > 0 && viewportH > 0 && Number.isFinite(rect.left) && Number.isFinite(rect.top) &&
        Number.isFinite(rect.right) && Number.isFinite(rect.bottom)) {
      const visW = Math.max(0, Math.min(rect.right, viewportW) - Math.max(rect.left, 0));
      const visH = Math.max(0, Math.min(rect.bottom, viewportH) - Math.max(rect.top, 0));
      visibleArea = visW * visH;
    }
    const score = fsScore + playingScore + readyScore + visibleArea;
    if (score > bestScore) {
      bestScore = score;
      bestCandidate = { video, src, rect };
    }
  }

  if (!bestCandidate) {
    removeDuplicateHosts(null);
    return [];
  }

  const attached = [];
  {
    const { video, src } = bestCandidate;
    const isBlob = Boolean(src && src.startsWith("blob:"));
    const isDrm = Boolean(video.mediaKeys);

    // If host element is already attached and in DOM, ensure it's positioned correctly and remove any duplicates
    if (video.__radiumHostEl && video.__radiumHostEl.isConnected) {
      video.__radiumHostEl.id = "radium-singleton-video-btn";
      removeDuplicateHosts(video.__radiumHostEl);
      positionOverlayHost(video, video.__radiumHostEl);
      return [{
        video,
        hostEl: video.__radiumHostEl,
        btn: video.__radiumBtn,
        selectEl: video.__radiumSelect,
        isBlob,
        isDrm,
        src,
        reposition: () => positionOverlayHost(video, video.__radiumHostEl)
      }];
    }

    // If host element was previously created but detached during DOM updates/SPA navigation, re-append it
    if (video.__radiumHostEl) {
      const targetContainer = doc.fullscreenElement || doc.body;
      if (targetContainer && targetContainer.appendChild) {
        video.__radiumHostEl.id = "radium-singleton-video-btn";
        removeDuplicateHosts(video.__radiumHostEl);
        targetContainer.appendChild(video.__radiumHostEl);
        positionOverlayHost(video, video.__radiumHostEl);
        return [{
          video,
          hostEl: video.__radiumHostEl,
          btn: video.__radiumBtn,
          selectEl: video.__radiumSelect,
          isBlob,
          isDrm,
          src,
          reposition: () => positionOverlayHost(video, video.__radiumHostEl)
        }];
      }
    }

    removeDuplicateHosts(null);

    video.dataset.radiumOverlayAttached = "true";
    video.dataset.fluxOverlayAttached = "true";
    video.dataset.openidmOverlayAttached = "true";

    const hostEl = doc.createElement("div");
    hostEl.id = "radium-singleton-video-btn";
    hostEl.setAttribute("data-radium-shadow-host", "true");
    hostEl.setAttribute("data-flux-shadow-host", "true");
    hostEl.setAttribute("data-openidm-shadow-host", "true");
    if (isDrm) {
      hostEl.setAttribute("data-radium-drm", "true");
      hostEl.setAttribute("data-flux-drm", "true");
    }
    positionOverlayHost(video, hostEl);

    // Closed Shadow DOM ensures zero CSS leakage into or from the host webpage
    const shadow = hostEl.attachShadow ? hostEl.attachShadow({ mode: "closed" }) : hostEl;

    const styleEl = doc.createElement("style");
    styleEl.textContent = `
      :host {
        all: initial;
        display: inline-flex;
      }
      .radium-overlay-container {
        display: inline-flex;
        align-items: center;
        gap: 4px;
        background: #25262A;
        border: 1px solid #414248;
        border-radius: 6px;
        padding: 2px 4px;
        box-shadow: 0 2px 8px rgba(0, 0, 0, 0.45);
        font-family: "Segoe UI Variable Text", "Segoe UI", -apple-system, BlinkMacSystemFont, sans-serif;
      }
      .radium-drag-handle {
        color: #A7A8AF;
        cursor: grab;
        user-select: none;
        touch-action: none;
        padding: 2px 3px;
        font: 700 12px "Segoe UI", sans-serif;
        line-height: 1;
      }
      .radium-drag-handle:active { cursor: grabbing; }
      button {
        display: inline-flex;
        align-items: center;
        gap: 6px;
        background: transparent;
        color: #F2F2F3;
        border: none;
        border-radius: 4px;
        padding: 4px 9px;
        font: 600 11px "Segoe UI Variable Text", "Segoe UI", -apple-system, BlinkMacSystemFont, sans-serif;
        letter-spacing: 0.01em;
        cursor: pointer;
        opacity: 0.94;
        transition: background 0.12s ease, color 0.12s ease;
        white-space: nowrap;
      }
      button::before {
        content: "";
        width: 6px;
        height: 6px;
        border-radius: 50%;
        background: #FF4F57;
        box-shadow: none;
        flex-shrink: 0;
      }
      button:hover:not(:disabled), button:focus-visible {
        opacity: 1;
        background: #37383D;
        color: #FFFFFF;
        outline: none;
      }
      button:active:not(:disabled) {
        transform: scale(0.98);
      }
      button:disabled {
        opacity: 0.72;
        color: #A4A5AC;
        cursor: not-allowed;
      }
      button:disabled::before {
        background: #64748B;
        box-shadow: none;
      }
      select {
        background: #2D2E32;
        color: #F2F2F3;
        border: 1px solid #414248;
        border-radius: 4px;
        padding: 2px 6px;
        font: 600 11px "Segoe UI Variable Text", "Segoe UI", -apple-system, BlinkMacSystemFont, sans-serif;
        cursor: pointer;
        outline: none;
        transition: border-color 0.12s ease;
        max-width: 170px;
        text-overflow: ellipsis;
      }
      select:hover, select:focus {
        border-color: #FF4F57;
      }
      select option {
        background: #25262A;
        color: #F2F2F3;
      }
      @media (prefers-reduced-motion: reduce) {
        button, select { transition: none; }
      }
    `;
    if (shadow && typeof shadow.appendChild === "function") {
      shadow.appendChild(styleEl);
    }

    const container = doc.createElement("div");
    container.className = "radium-overlay-container";
    const dragHandle = doc.createElement("span");
    dragHandle.className = "radium-drag-handle";
    dragHandle.textContent = "⠿";
    dragHandle.title = "Drag to move Radium controls";
    dragHandle.setAttribute("aria-label", "Drag to move Radium controls");
    let isDragging = false;
    dragHandle.addEventListener("click", (ev) => {
      if (ev && typeof ev.stopPropagation === "function") ev.stopPropagation();
      if (ev && typeof ev.preventDefault === "function") ev.preventDefault();
    });
    dragHandle.addEventListener("pointerdown", (ev) => {
      if (!ev || ev.button !== 0) return;
      ev.preventDefault();
      ev.stopPropagation();
      const startX = ev.clientX, startY = ev.clientY;
      const startLeft = parseInt(hostEl.style.left, 10) || 8;
      const startTop = parseInt(hostEl.style.top, 10) || 8;
      const move = (moveEvent) => {
        isDragging = true;
        const viewport = doc.defaultView || window;
        const maxLeft = Math.max(8, viewport.innerWidth - (hostEl.offsetWidth || 240) - 8);
        const maxTop = Math.max(8, viewport.innerHeight - (hostEl.offsetHeight || 40) - 8);
        const left = Math.max(8, Math.min(maxLeft, startLeft + moveEvent.clientX - startX));
        const top = Math.max(8, Math.min(maxTop, startTop + moveEvent.clientY - startY));
        hostEl.__radiumManualPosition = { left, top };
        hostEl.style.left = `${left}px`;
        hostEl.style.top = `${top}px`;
        hostEl.style.display = "inline-flex";
        hostEl.style.visibility = "visible";
        hostEl.style.opacity = "1";
      };
      const end = (upEvent) => {
        if (upEvent && typeof upEvent.stopPropagation === "function") upEvent.stopPropagation();
        setTimeout(() => { isDragging = false; }, 50);
        doc.removeEventListener("pointermove", move, true);
        doc.removeEventListener("pointerup", end, true);
        doc.removeEventListener("pointercancel", end, true);
      };
      doc.addEventListener("pointermove", move, true);
      doc.addEventListener("pointerup", end, true);
      doc.addEventListener("pointercancel", end, true);
    });

    const lastCand = (typeof window !== "undefined" && window.__radiumLastCandidate) ? window.__radiumLastCandidate : null;
    const topQ = (lastCand && Array.isArray(lastCand.qualities) && lastCand.qualities.length > 0)
      ? (lastCand.qualities.find(q => !q.isAudioOnly && String(q.resolution || "").toLowerCase().includes("1080")) ||
         lastCand.qualities.find(q => !q.isAudioOnly) ||
         lastCand.qualities[0])
      : null;
    const qSuffix = (topQ && topQ.resolution) ? ` [${topQ.resolution}]` : "";

    const btn = doc.createElement("button");
    btn.type = "button";
    btn.textContent = isDrm ? "DRM Protected (Unsupported)" : `Download with Radium${qSuffix}`;
    btn.disabled = isDrm;
    const baseBtnStyle =
      "background:transparent;color:#F2F2F3;border:none;border-radius:4px;" +
      "padding:4px 9px;font:600 11px 'Segoe UI Variable Text','Segoe UI',sans-serif;cursor:pointer;white-space:nowrap;";
    if (btn.style) {
      btn.style.cssText = baseBtnStyle;
    }

    const selectEl = doc.createElement("select");
    selectEl.setAttribute("aria-label", "Select video quality");
    selectEl.style.display = "none"; // Shown when qualities arrive

    const invokeClick = (ev) => {
      if (ev && typeof ev.stopPropagation === "function") ev.stopPropagation();
      if (ev && typeof ev.preventDefault === "function") ev.preventDefault();

      const liveDrm = Boolean(video.mediaKeys) || isDrm;
      if (liveDrm) {
        btn.textContent = "DRM Protected (Unsupported)";
        btn.disabled = true;
        return;
      }

      const liveSrc = extractVideoElementSrc(video);
      const liveIsBlob = liveSrc.startsWith("blob:");

      const cand = (typeof window !== "undefined") ? (window.__radiumLastCandidate || window.__fluxLastCandidate) : null;
      let chosenQ = hostEl.__radiumSelectedQuality || null;
      if (!chosenQ && selectEl && selectEl.value && cand && Array.isArray(cand.qualities)) {
        chosenQ = cand.qualities.find(q =>
          String(q.id || "").toLowerCase() === String(selectEl.value).toLowerCase() ||
          String(q.resolution || "").toLowerCase() === String(selectEl.value).toLowerCase()
        ) || null;
      }

      if (onDownloadClick) {
        const pageLoc = (doc && doc.location && doc.location.href) ? doc.location.href : (typeof location !== "undefined" ? location.href : "");
        let safeTopLoc = pageLoc;
        try {
          if (typeof window !== "undefined" && window.top && window.top.location && window.top.location.href) {
            safeTopLoc = window.top.location.href;
          }
        } catch (_) {}
        onDownloadClick({
          videoSrc: (chosenQ && chosenQ.videoUrl) ? chosenQ.videoUrl : (liveIsBlob ? "" : liveSrc),
          isMseBlob: liveIsBlob,
          pageTitle: (doc && doc.title) ? doc.title : (typeof document !== "undefined" ? document.title : "video"),
          frameUrl: pageLoc,
          tabUrl: safeTopLoc || pageLoc,
          isIframe: (typeof window !== "undefined" && window.self !== window.top),
          buttonEl: btn,
          selectEl,
          hostEl,
          baseBtnStyle,
          qualityId: chosenQ ? (chosenQ.id || chosenQ.resolution) : undefined,
          resolution: chosenQ ? chosenQ.resolution : undefined,
          videoFilter: chosenQ ? (chosenQ.videoFilter || chosenQ.resolution) : undefined,
          selectedQuality: chosenQ || undefined
        });
      }
    };

    btn.addEventListener("click", (ev) => {
      if (isDragging) {
        if (ev && typeof ev.stopPropagation === "function") ev.stopPropagation();
        if (ev && typeof ev.preventDefault === "function") ev.preventDefault();
        return;
      }
      invokeClick(ev);
    });

    selectEl.addEventListener("change", (ev) => {
      if (ev && typeof ev.stopPropagation === "function") ev.stopPropagation();
      const val = selectEl.value;
      const cand = (typeof window !== "undefined") ? (window.__radiumLastCandidate || window.__fluxLastCandidate) : null;
      if (cand && Array.isArray(cand.qualities)) {
        const found = cand.qualities.find(q =>
          String(q.id || "").toLowerCase() === String(val).toLowerCase() ||
          String(q.resolution || "").toLowerCase() === String(val).toLowerCase()
        );
        if (found) {
          hostEl.__radiumSelectedQuality = found;
          const qText = found.resolution || found.label || "";
          const szStr = found.size > 0 ? ` (~${(found.size / 1048576).toFixed(0)} MB)` : "";
          btn.textContent = `Download [${qText}${szStr}]`;
          btn.title = `Download ${qText} with Radium Download Manager`;
        }
      }
    });

    selectEl.addEventListener("click", (ev) => {
      if (ev && typeof ev.stopPropagation === "function") ev.stopPropagation();
    });

    if (container.appendChild) {
      container.appendChild(dragHandle);
      container.appendChild(btn);
      container.appendChild(selectEl);
    }
    shadow.appendChild(container);
    const targetContainer = doc.fullscreenElement || doc.body;
    if (targetContainer && targetContainer.appendChild) {
      targetContainer.appendChild(hostEl);
    }

    video.__radiumHostEl = video.__fluxHostEl = hostEl;
    video.__radiumBtn = video.__fluxBtn = btn;
    video.__radiumSelect = video.__fluxSelect = selectEl;
    hostEl.__radiumVideo = hostEl.__fluxVideo = video;
    hostEl.__radiumBtn = hostEl.__fluxBtn = btn;
    hostEl.__radiumSelect = hostEl.__fluxSelect = selectEl;

    attached.push({
      video,
      hostEl,
      btn,
      selectEl,
      isBlob,
      isDrm,
      src,
      reposition: () => positionOverlayHost(video, hostEl)
    });
  }
  return attached;
}

function observeDynamicVideosAndFullscreen(doc, win, onDownloadClick) {
  let activeOverlays = scanAndAttachVideoOverlays(doc, onDownloadClick);

  const refreshPositions = () => {
    activeOverlays = activeOverlays.filter(item => {
      if (item.video && item.video.isConnected === false) {
        if (item.hostEl && typeof item.hostEl.remove === "function") {
          item.hostEl.remove();
        }
        return false;
      }
      if (item.hostEl && item.hostEl.isConnected === false) {
        const targetContainer = (doc && doc.fullscreenElement) || (doc && doc.body);
        if (targetContainer && targetContainer.appendChild) {
          targetContainer.appendChild(item.hostEl);
        }
      }
      item.reposition();
      return true;
    });
  };

  const rescanAndRefresh = () => {
    const singletonAttached = scanAndAttachVideoOverlays(doc, onDownloadClick);
    if (singletonAttached.length > 0) {
      activeOverlays = singletonAttached;
    } else {
      activeOverlays = [];
    }
    refreshPositions();
  };

  if (win && typeof win.addEventListener === "function") {
    win.addEventListener("scroll", refreshPositions, { passive: true });
    win.addEventListener("resize", refreshPositions, { passive: true });
  }
  if (doc && typeof doc.addEventListener === "function") {
    doc.addEventListener("fullscreenchange", () => {
      refreshPositions();
      rescanAndRefresh();
    });
    doc.addEventListener("webkitfullscreenchange", () => {
      refreshPositions();
      rescanAndRefresh();
    });
    doc.addEventListener("loadedmetadata", rescanAndRefresh, true);
    doc.addEventListener("loadeddata", rescanAndRefresh, true);
    doc.addEventListener("canplay", rescanAndRefresh, true);
    doc.addEventListener("play", rescanAndRefresh, true);
    doc.addEventListener("playing", rescanAndRefresh, true);
    doc.addEventListener("timeupdate", refreshPositions, { passive: true, capture: true });
    doc.addEventListener("yt-navigate-finish", rescanAndRefresh);
    doc.addEventListener("yt-page-data-updated", rescanAndRefresh);
  }

  // Periodic gentle sweep ensuring dynamic SPA player transitions never leave the overlay detached
  const pollTimer = (typeof setInterval === "function") ? setInterval(() => {
    if (g_contentScriptCleanedUp) {
      clearInterval(pollTimer);
      return;
    }
    rescanAndRefresh();
  }, 1500) : null;

  if (typeof MutationObserver !== "undefined" && doc && doc.documentElement) {
    const observer = new MutationObserver((mutations) => {
      if (Array.isArray(mutations)) {
        const onlyOverlayChange = mutations.every(m =>
          m.target && m.target.getAttribute && (m.target.getAttribute("data-radium-shadow-host") === "true" || m.target.getAttribute("data-flux-shadow-host") === "true")
        );
        if (onlyOverlayChange) return;
      }
      rescanAndRefresh();
    });
    observer.observe(doc.documentElement, { childList: true, subtree: true });
    g_videoObserver = observer;
    return { activeOverlays, observer, refreshPositions, pollTimer };
  }
  return { activeOverlays, observer: null, refreshPositions, pollTimer };
}


function findAnchorFromEvent(ev) {
  if (!ev) return null;
  if (typeof ev.composedPath === "function") {
    try {
      const path = ev.composedPath();
      for (const el of path) {
        if (el && el.nodeType === 1 && typeof el.matches === "function") {
          if (el.matches("a[href], [data-download-url], [data-href]")) {
            return el;
          }
        }
      }
    } catch (_) {}
  }
  let target = ev.target;
  if (target && target.nodeType === 3) {
    target = target.parentElement;
  }
  if (target && typeof target.closest === "function") {
    return target.closest("a[href], [data-download-url], [data-href]") || target.closest("a");
  }
  return null;
}

function triggerBrowserFallback(anchor, href) {
  try {
    if (anchor && anchor.dataset) {
      anchor.dataset.radiumBypass = "true";
      anchor.dataset.fluxBypass = "true";
    }
    safeSendRuntimeMessage({ action: "bypassNextDownload", url: href });
    if (anchor && typeof anchor.click === "function") {
      anchor.click();
    } else if (typeof location !== "undefined" && location.assign) {
      location.assign(href);
    }
  } catch (_) {}
}

// Trusted User Gesture Tracking for Blob Downloads.
let g_lastUserGestureTime = 0;

function recordTrustedUserGesture(ev) {
  if (ev && ev.isTrusted) {
    g_lastUserGestureTime = Date.now();
  }
}

function handleLinkClick(ev) {
  if (!isExtensionContextValid() || g_contentScriptCleanedUp) {
    cleanupContentScript();
    return;
  }
  try {
    const correlationId = "click-" + Date.now().toString(36) + "-" + Math.random().toString(36).slice(2, 8);

    const anchor = findAnchorFromEvent(ev);
    if (!anchor) return;

    if (anchor.dataset && (anchor.dataset.radiumBypass === "true" || anchor.dataset.radiumBypass === true || anchor.dataset.fluxBypass === "true" || anchor.dataset.fluxBypass === true)) {
      delete anchor.dataset.radiumBypass;
      delete anchor.dataset.fluxBypass;
      return;
    }

    let href = anchor.href || (typeof anchor.getAttribute === "function" ? (anchor.getAttribute("data-download-url") || anchor.getAttribute("data-href")) : "") || "";
    if (!href) return;

    let isFrame = false;
    try { isFrame = (window !== window.top); } catch (_) { isFrame = true; }
    let referrer = "";
    try { referrer = location.href; } catch (_) {}
    let pageTitle = "";
    try { pageTitle = document.title; } catch (_) {}

    const isBlobOrData = href.startsWith("blob:") || href.startsWith("data:");
    let mechanism = isBlobOrData ? "BROWSER_BLOB_OR_DATA" : (isFrame ? "FRAME_ORIGINATED" : "ORDINARY_LINK");
    if (!ev.isTrusted) {
      mechanism = "JS_TRIGGERED_DYNAMIC_ANCHOR";
    }

    console.log(`[Radium Trace] [CLICK_SEEN] [${correlationId}]`, {
      url: isBlobOrData ? (href.slice(0, 48) + "...") : href,
      target: anchor.tagName,
      button: ev.button,
      altKey: ev.altKey,
      mechanism,
      isFrame
    });

    if (ev.altKey) {
      // IDM Alt-key bypass behavior: user explicitly requested browser-native download
      console.log(`[Radium Trace] [CLICK_IGNORED] [${correlationId}]`, { url: isBlobOrData ? (href.slice(0, 48) + "...") : href, reason: "ALT_KEY_BYPASS", mechanism });
      safeSendRuntimeMessage({ action: "bypassNextDownload", url: href, correlationId });
      return;
    }

    // 1. Browser-generated in-memory Blob and Data URLs:
    // Native browser download pipeline handles client-side files reliably without disruption.
    const isWebBlob = href.startsWith("blob:http://") || href.startsWith("blob:https://");
    if (isBlobOrData && !isWebBlob) {
      console.log(`[Radium Trace] [BROWSER_BLOB_OR_DATA] [${correlationId}]`, {
        url: href.slice(0, 48) + "...",
        filename: anchor.download || "",
        mechanism
      });
      return;
    }

    if (ev.button !== undefined && ev.button !== 0) {
      console.log(`[Radium Trace] [CLICK_IGNORED] [${correlationId}]`, { url: href, reason: "UNSUPPORTED_BUTTON", mechanism });
      return;
    }

    if (href.startsWith("javascript:") || href.startsWith("#") || href.startsWith("mailto:")) {
      console.log(`[Radium Trace] [CLICK_IGNORED] [${correlationId}]`, { url: href, reason: "NON_HTTP_PROTOCOL", mechanism });
      return;
    }

    let pathname = anchor.pathname || "";
    if (!pathname && typeof URL !== "undefined") {
      try { pathname = new URL(href, (typeof location !== "undefined" ? location.href : undefined)).pathname; } catch (_) {}
    }
    const lastDot = pathname.lastIndexOf(".");
    const ext = lastDot !== -1 ? pathname.slice(lastDot + 1).toLowerCase() : "";
    const hasDownloadAttr = typeof anchor.hasAttribute === "function" && anchor.hasAttribute("download");

    const interceptExts = new Set([
      "zip", "rar", "7z", "tar", "gz", "bz2", "xz", "tgz", "iso", "exe", "msi", "dmg", "pdf",
      "apk", "bin", "deb", "rpm", "docx", "xlsx", "pptx", "csv", "epub",
      "mp4", "mkv", "webm", "mov", "avi", "flv", "mp3", "m4a", "flac", "wav",
      "py", "json", "txt", "md", "sql", "yaml", "xml", "html", "js", "ts", "cpp", "c", "rs", "go", "java"
    ]);

    const isAiFileUrl = href.includes("oaiusercontent.com") || href.includes("rscd=") ||
                        href.includes("response-content-disposition=") || href.includes("/backend-api/files/");
    let isEligible = isWebBlob || hasDownloadAttr || isAiFileUrl || interceptExts.has(ext) ||
      pathname.includes("/redirect") || pathname.includes("/download") || pathname.includes("/dl/");

    if (!isEligible && typeof URL !== "undefined") {
      try {
        const parsed = new URL(href, (typeof location !== "undefined" ? location.href : undefined));
        for (const [key, val] of parsed.searchParams.entries()) {
          const kLower = key.toLowerCase();
          if (kLower.includes("download") || kLower === "dl" || kLower === "file" || kLower === "filename") {
            const vDot = val.lastIndexOf(".");
            const vExt = vDot !== -1 ? val.slice(vDot + 1).toLowerCase() : "";
            if (interceptExts.has(vExt) || val === "1" || val === "true" || kLower.includes("download")) {
              isEligible = true;
              break;
            }
          }
          const vDot = val.lastIndexOf(".");
          if (vDot !== -1 && interceptExts.has(val.slice(vDot + 1).toLowerCase())) {
            isEligible = true;
            break;
          }
        }
      } catch (_) {}
    }

    if (!isEligible) {
      console.log(`[Radium Trace] [CLICK_IGNORED] [${correlationId}]`, {
        url: href,
        reason: "NOT_DOWNLOAD_ELIGIBLE",
        mechanism: "UNKNOWN_FILENAME_DYNAMIC",
        note: "Deferred to browser network response for Content-Disposition determination"
      });
      return;
    }

    // Prevent duplicate click recordings from rapid multi-clicks (brief debounce)
    if (anchor.__radiumClickInFlight) {
      return;
    }
    anchor.__radiumClickInFlight = true;
    setTimeout(() => { delete anchor.__radiumClickInFlight; }, 800);

    // Record link click metadata for correlation in chrome.downloads.onDeterminingFilename.
    // User gesture is strictly preserved: we NEVER call ev.preventDefault() or ev.stopPropagation()
    // on natural anchor clicks, ensuring browser cookies, session state, and redirects proceed uninterrupted.
    const targetFilename = anchor.download ||
      (typeof extractFilenameFromUrlQuery === "function" ? extractFilenameFromUrlQuery(href) : "") || "";
    safeSendRuntimeMessage({
      action: "linkClicked",
      url: href,
      filename: targetFilename,
      hasDownloadAttr: Boolean(anchor.download || isAiFileUrl || hasDownloadAttr),
      referrer,
      pageTitle,
      bypass: false,
      mechanism,
      isFrame,
      correlationId
    });

    console.log(`[Radium Trace] [CLICK_OBSERVED] [${correlationId}]`, {
      url: href,
      filename: anchor.download || "",
      hasDownloadAttr,
      mechanism,
      note: "Click observed and correlated with chrome.downloads.onDeterminingFilename; user gesture preserved"
    });
    console.log(`[Radium Trace] [CLICK_RECORDED] [${correlationId}]`, {
      url: href,
      filename: anchor.download || "",
      hasDownloadAttr,
      mechanism,
      note: "Click intent correlated with chrome.downloads.onDeterminingFilename; user gesture preserved"
    });
  } catch (err) {
    console.error("[Radium] Error in handleLinkClick:", err);
  }
}

function handleMediaPlaybackEvent(ev) {
  if (!isExtensionContextValid() || g_contentScriptCleanedUp) {
    cleanupContentScript();
    return;
  }
  try {
    const target = ev.target;
    if (!target) return;
    const tagName = (target.tagName || "").toLowerCase();
    if (tagName !== "video" && tagName !== "audio") return;

    const src = extractVideoElementSrc(target);
    if (src && !src.startsWith("blob:") && !src.startsWith("data:")) {
      const lower = src.toLowerCase();
      const streamType = lower.includes(".m3u8") ? "HLS" : (lower.includes(".mpd") ? "DASH" : "HTTP_FILE");
      safeSendRuntimeMessage({
        action: "addMedia",
        url: src,
        streamType,
        pageTitle: (typeof document !== "undefined" ? document.title : "video"),
        tabUrl: (typeof location !== "undefined" ? location.href : "")
      });
    }
  } catch (_) {}
}

function extractEligiblePageLinks(doc) {
  if (!doc || !doc.querySelectorAll) return [];
  const anchors = Array.from(doc.querySelectorAll("a[href]"));
  const seen = new Set();
  const results = [];

  // Single source of truth loaded via js/categories.generated.js
  const extCategories = (typeof RADIUM_FILE_CATEGORIES !== "undefined") ? RADIUM_FILE_CATEGORIES : {};

  for (const a of anchors) {
    const rawHref = a.getAttribute("href") || "";
    if (!rawHref || rawHref.startsWith("#") || rawHref.startsWith("javascript:") || rawHref.startsWith("mailto:") || rawHref.startsWith("data:")) {
      continue;
    }
    let fullUrl = a.href;
    try { fullUrl = new URL(rawHref, doc.location ? doc.location.href : undefined).href; } catch (_) {}
    if (!fullUrl.startsWith("http://") && !fullUrl.startsWith("https://")) continue;
    if (seen.has(fullUrl)) continue;
    seen.add(fullUrl);

    let pathname = "";
    try { pathname = new URL(fullUrl).pathname; } catch (_) {}
    const rawFilename = (a.download || pathname.split("/").pop() || "download").split("?")[0];
    const lastDot = rawFilename.lastIndexOf(".");
    const ext = lastDot !== -1 ? rawFilename.slice(lastDot + 1).toLowerCase() : "";
    const category = extCategories[ext] || "Other";
    const linkText = (a.textContent || a.title || rawFilename).trim().replace(/\s+/g, " ");

    results.push({
      url: fullUrl,
      filename: rawFilename || "file",
      ext: ext || "file",
      category,
      text: linkText.slice(0, 80),
      isDirectFile: Boolean(extCategories[ext])
    });
  }
  return results;
}

function injectCatchScriptIfNeeded() {
  // In Firefox MV3 (moz-extension://), content scripts cannot declare "world": "MAIN".
  // In Chromium/Brave (chrome-extension://), manifest.json already injects catch.js in MAIN world,
  // so injecting a <script src="chrome-extension://..."> tag is redundant and triggers frame/CSP warnings.
  try {
    if (typeof chrome !== "undefined" && chrome.runtime && chrome.runtime.getURL) {
      const scriptUrl = chrome.runtime.getURL("catch-script/catch.js");
      if (!scriptUrl || !scriptUrl.startsWith("moz-extension://")) return;
      if (document.getElementById("radium-injected-catch-script")) return;
      const script = document.createElement("script");
      script.id = "radium-injected-catch-script";
      script.src = scriptUrl;
      script.async = false;
      const target = document.head || document.documentElement;
      if (target) {
        target.appendChild(script);
        script.remove();
      }
    }
  } catch (err) {
    // Suppress potential CSP restrictions gracefully
  }
}

function initContentScript() {
  if (typeof document === "undefined" || typeof window === "undefined") return;
  injectCatchScriptIfNeeded();

  // 1. Track trusted user gestures and intercept link clicks with capture phase at window level
  window.addEventListener("pointerdown", recordTrustedUserGesture, { capture: true, passive: true });
  window.addEventListener("mousedown", recordTrustedUserGesture, { capture: true, passive: true });
  window.addEventListener("click", recordTrustedUserGesture, { capture: true, passive: true });
  window.addEventListener("keydown", recordTrustedUserGesture, { capture: true, passive: true });
  window.addEventListener("click", handleLinkClick, { capture: true, passive: false });

  // 2. Listen to media playback events with capture phase
  document.addEventListener("play", handleMediaPlaybackEvent, true);
  document.addEventListener("playing", handleMediaPlaybackEvent, true);
  document.addEventListener("canplay", handleMediaPlaybackEvent, true);
  document.addEventListener("loadedmetadata", handleMediaPlaybackEvent, true);

  // 3. Relay page MAIN world messages from catch-script/catch.js to background
  g_messageListener = (ev) => {
    try {
      if (!isExtensionContextValid() || g_contentScriptCleanedUp) {
        cleanupContentScript();
        return;
      }
      if (!ev || !ev.data) return;
      if (ev.source !== window && ev.origin !== window.location.origin) return;

      if (ev.data.action === "catCatchAddMedia") {
        if (typeof window !== "undefined") {
          window.__radiumLastCandidate = ev.data;
        }
        // Dynamically update overlay badge label and quality selector with detected qualities and estimated size
        try {
          if (typeof document !== "undefined" && document.querySelectorAll) {
            const hosts = document.querySelectorAll("[data-radium-shadow-host], [data-flux-shadow-host]");
            const qualities = Array.isArray(ev.data.qualities) ? ev.data.qualities : [];
            const topQ = (qualities.length > 0)
              ? (qualities.find(q => !q.isAudioOnly && String(q.resolution || "").toLowerCase().includes("1080")) ||
                 qualities.find(q => !q.isAudioOnly) ||
                 qualities[0])
              : null;

            if (topQ) {
              ev.data.qualityId = topQ.id || topQ.resolution || "1080p";
              ev.data.resolution = topQ.resolution || "1080p";
              const qText = topQ.resolution || topQ.label || "";
              const szStr = topQ.size > 0 ? ` (~${(topQ.size / 1048576).toFixed(0)} MB)` : "";

              hosts.forEach(h => {
                if (!h.__radiumSelectedQuality) {
                  h.__radiumSelectedQuality = topQ;
                }
                const chosen = h.__radiumSelectedQuality || topQ;
                const chosenText = chosen.resolution || chosen.label || "";
                const chosenSz = chosen.size > 0 ? ` (~${(chosen.size / 1048576).toFixed(0)} MB)` : "";

                if (h.__radiumBtn &&
                    !h.getAttribute("data-radium-status") && !h.getAttribute("data-flux-status") &&
                    !h.getAttribute("data-radium-drm") && !h.getAttribute("data-flux-drm")) {
                  h.__radiumBtn.textContent = `Download [${chosenText}${chosenSz}]`;
                  h.__radiumBtn.title = `Download ${chosenText} with Radium Download Manager`;
                }

                // Populate or update the compact quality selection dropdown
                const sel = h.__radiumSelect;
                if (sel) {
                  if (qualities.length > 1) {
                    sel.innerHTML = "";
                    for (const q of qualities) {
                      const opt = document.createElement("option");
                      const qId = q.id || q.resolution || "";
                      opt.value = qId;
                      const qLbl = q.label || q.resolution || qId;
                      const qSizeStr = q.size > 0 ? ` (~${(q.size / 1048576).toFixed(0)} MB)` : "";
                      opt.textContent = `${qLbl}${qSizeStr}`;
                      if (chosen && (String(chosen.id || "").toLowerCase() === String(qId).toLowerCase() ||
                                     String(chosen.resolution || "").toLowerCase() === String(qId).toLowerCase())) {
                        opt.selected = true;
                      }
                      sel.appendChild(opt);
                    }
                    sel.style.display = "inline-block";
                  } else {
                    sel.style.display = "none";
                  }
                }
              });
            }
          }
        } catch (_) {}
        safeSendRuntimeMessage({
          action: "addMedia",
          url: ev.data.url,
          streamType: ev.data.streamType || "HLS",
          sourceType: ev.data.sourceType || "",
          ext: ev.data.ext || "mp4",
          mime: ev.data.mime || "",
          size: ev.data.size || 0,
          itag: ev.data.itag || "",
          isAudioOnly: Boolean(ev.data.isAudioOnly),
          isGoogleVideo: Boolean(ev.data.isGoogleVideo),
          isAdaptiveTrack: Boolean(ev.data.isAdaptiveTrack),
          requiresAudioMux: Boolean(ev.data.requiresAudioMux),
          requiresCipherExtraction: Boolean(ev.data.requiresCipherExtraction),
          videoId: ev.data.videoId || "",
          resolution: ev.data.resolution || "",
          videoUrl: ev.data.videoUrl || "",
          pairedAudioUrl: ev.data.pairedAudioUrl || "",
          qualities: Array.isArray(ev.data.qualities) ? ev.data.qualities : [],
          extractorJson: ev.data.extractorJson || "",
          candidateId: ev.data.candidateId || "",
          cachedManifestBody: ev.data.cachedManifestBody || "",
          tabUrl: location.href,
          pageTitle: ev.data.pageTitle || document.title || "video"
        }, (resp) => {
          if (resp && resp.candidate && resp.candidate.candidateId && typeof window !== "undefined" && window.__radiumLastCandidate) {
            window.__radiumLastCandidate.candidateId = resp.candidate.candidateId;
          }
        });
      } else if (ev.data.action === "catCatchAddKey") {
        safeSendRuntimeMessage({
          action: "addKey",
          key: ev.data.key,
          url: ev.data.url
        });
      } else if (ev.data.action === "catCatchPlayerError") {
        safeSendRuntimeMessage({
          action: "playerError",
          reason: ev.data.reason,
          url: ev.data.url || location.href
        });
      } else if (ev.data.action === "radiumDirectBlobDownload") {
        // Forward all same-window blob:/data: downloads without SEC-001 token/gesture rejection
        // so programmatic SPA exports (Gemini, ChatGPT, Claude, etc.) are never silently dropped.
        const isBlobOrDataUrl = typeof ev.data.url === "string" &&
          (ev.data.url.startsWith("blob:") || ev.data.url.startsWith("data:"));
        const dataBase64 = typeof ev.data.dataBase64 === "string" ? ev.data.dataBase64 : "";
        if (!isBlobOrDataUrl && !dataBase64) {
          return;
        }
        const dispatchBlobToBackground = (resolvedB64, resolvedMime, resolvedSize) => {
          safeSendRuntimeMessage({
            action: "interceptBlobDownload",
            url: ev.data.url,
            filename: ev.data.filename,
            mimeType: resolvedMime || ev.data.mimeType,
            size: resolvedSize || ev.data.size,
            dataBase64: resolvedB64,
            correlationId: ev.data.correlationId,
            pageTitle: ev.data.pageTitle || (typeof document !== "undefined" ? document.title : ""),
            pageUrl: ev.data.pageUrl || (typeof location !== "undefined" ? location.href : "")
          }, (resp) => {
            if (!resp || !resp.success) {
              safeSendRuntimeMessage({ action: "bypassNextDownload", url: ev.data.url, correlationId: ev.data.correlationId });
              window.postMessage({
                action: "radiumBlobFallback",
                url: ev.data.url,
                filename: ev.data.filename
              }, "*");
            }
          });
        };
        // If MAIN world could not read the blob bytes (e.g. strict page CSP or Worker-created blob),
        // read the blob here in the ISOLATED content script (same origin as the page, exempt from page CSP),
        // because the chrome-extension:// Service Worker cannot fetch cross-origin blob:https:// URLs!
        if (!dataBase64 && typeof ev.data.url === "string" && ev.data.url.startsWith("blob:") &&
            typeof fetch === "function" && typeof FileReader === "function") {
          fetch(ev.data.url).then(r => r.blob()).then(blob => {
            const reader = new FileReader();
            reader.onload = () => {
              const res = String(reader.result || "");
              const commaIdx = res.indexOf(",");
              const b64 = commaIdx >= 0 ? res.slice(commaIdx + 1) : res;
              dispatchBlobToBackground(b64, blob.type || ev.data.mimeType, blob.size || ev.data.size);
            };
            reader.onerror = () => dispatchBlobToBackground("", ev.data.mimeType, ev.data.size);
            reader.readAsDataURL(blob);
          }).catch(() => dispatchBlobToBackground("", ev.data.mimeType, ev.data.size));
        } else {
          dispatchBlobToBackground(dataBase64, ev.data.mimeType, ev.data.size);
        }
      } else if (ev.data.action === "fluxAnchorClicked" || ev.data.action === "radiumAnchorClicked") {
        safeSendRuntimeMessage({
          action: "linkClicked",
          url: ev.data.url,
          filename: ev.data.download || "",
          hasDownloadAttr: Boolean(ev.data.download),
          referrer: ev.data.pageUrl || location.href,
          pageTitle: ev.data.pageTitle || document.title,
          clickTime: ev.data.clickTime || Date.now(),
          bypass: false,
          mechanism: "JS_TRIGGERED_DYNAMIC_ANCHOR",
          isFrame: (window !== window.top),
          correlationId: "anchor-" + Date.now().toString(36) + "-" + Math.random().toString(36).slice(2, 8)
        });
      }
    } catch (_) {}
  };
  window.addEventListener("message", g_messageListener, { capture: true });

  // 3b. Handle background probePageMedia & extractPageLinks RPC
  if (typeof chrome !== "undefined" && chrome.runtime && chrome.runtime.onMessage) {
    chrome.runtime.onMessage.addListener((request, sender, sendResponse) => {
      if (!isExtensionContextValid() || g_contentScriptCleanedUp) {
        cleanupContentScript();
        return false;
      }
      if (request && (request.action === "radiumFetchBlobData" || request.action === "fluxFetchBlobData")) {
        const blobUrl = request.blobUrl || "";
        const requestId = request.requestId || ("blob-req-" + Date.now().toString(36));

        const queryCatchScript = () => {
          let resolved = false;
          const handler = (ev) => {
            if (!ev || !ev.data || (ev.data.action !== "radiumBlobDataResponse" && ev.data.action !== "fluxBlobDataResponse") || ev.data.requestId !== requestId) return;
            if (resolved) return;
            resolved = true;
            window.removeEventListener("message", handler, true);
            sendResponse(ev.data);
          };
          window.addEventListener("message", handler, true);
          window.postMessage({ action: "radiumGetBlobData", blobUrl, requestId }, "*");
          window.postMessage({ action: "fluxGetBlobData", blobUrl, requestId }, "*");
          setTimeout(() => {
            if (!resolved) {
              resolved = true;
              window.removeEventListener("message", handler, true);
              sendResponse({ success: false, error: "TIMEOUT" });
            }
          }, 8000);
        };

        if (typeof fetch === "function") {
          fetch(blobUrl).then(r => r.blob()).then(blob => {
            const reader = new FileReader();
            reader.onload = () => {
              const res = String(reader.result || "");
              const commaIdx = res.indexOf(",");
              const b64 = commaIdx >= 0 ? res.slice(commaIdx + 1) : res;
              sendResponse({
                success: true,
                dataBase64: b64,
                mimeType: blob.type || "application/octet-stream",
                size: blob.size || 0
              });
            };
            reader.onerror = () => { queryCatchScript(); };
            reader.readAsDataURL(blob);
          }).catch(() => { queryCatchScript(); });
        } else {
          queryCatchScript();
        }
        return true;
      }

      if (request && (request.action === "probePageMedia" || request.action === "radiumProbePageMedia")) {
        try {
          window.postMessage({ action: "radiumScanPageMedia" }, "*");
          window.postMessage({ action: "fluxScanPageMedia" }, "*");
          sendResponse({ success: true });
        } catch (e) {
          sendResponse({ success: false, error: e.message });
        }
        return true;
      }
      if (request && request.action === "extractPageLinks") {
        try {
          const links = extractEligiblePageLinks(document);
          sendResponse({ success: true, links });
        } catch (e) {
          sendResponse({ success: false, error: e.message, links: [] });
        }
        return true;
      }
    });
  }

  // 4. Note: catch-script/catch.js is injected directly into page MAIN world at document_start by manifest.json.
  // Redundant DOM script tag injection has been removed to preserve CSP compliance across all frames.

  // 5. Observe videos and attach overlay with active visual status & error reporting
  const onDownloadClick = (mediaMeta) => {
    const btn = mediaMeta.buttonEl;
    const hostEl = mediaMeta.hostEl;
    const baseStyle = mediaMeta.baseBtnStyle || "";

    if (btn) {
      btn.textContent = "Starting in Radium...";
      btn.disabled = true;
    }
    if (hostEl && hostEl.setAttribute) {
      hostEl.setAttribute("data-radium-status", "starting");
      hostEl.setAttribute("data-flux-status", "starting");
    }

    const lastCand = (typeof window !== "undefined" && window.__radiumLastCandidate) ? window.__radiumLastCandidate : null;
    let chosenQ = mediaMeta.selectedQuality || (hostEl && hostEl.__radiumSelectedQuality) || null;
    if (!chosenQ && lastCand && Array.isArray(lastCand.qualities)) {
      chosenQ = lastCand.qualities.find(q => !q.isAudioOnly && String(q.resolution || "").toLowerCase().includes("1080")) ||
                lastCand.qualities.find(q => !q.isAudioOnly) ||
                lastCand.qualities[0];
    }
    // Never pick subtitle files (.srt, .vtt) as the primary video source
    let candidateId = mediaMeta.candidateId || (lastCand && lastCand.candidateId) || "";
    let qualityId = mediaMeta.qualityId || (chosenQ && (chosenQ.id || chosenQ.resolution)) || (lastCand && lastCand.qualityId) || "";
    let resolution = mediaMeta.resolution || (chosenQ && chosenQ.resolution) || (lastCand && lastCand.resolution) || "";
    let videoFilter = mediaMeta.videoFilter || (chosenQ && (chosenQ.videoFilter || chosenQ.resolution)) || "best";
    let videoSrc = mediaMeta.videoSrc || (chosenQ && chosenQ.videoUrl) || (lastCand ? (lastCand.url || lastCand.videoUrl) : "") || "";
    if (videoSrc && (videoSrc.toLowerCase().includes(".srt") || videoSrc.toLowerCase().includes(".vtt"))) {
      videoSrc = "";
      candidateId = "";
    }
    const curLoc = (typeof location !== "undefined" && location.href) ? location.href : "";
    let effectivePageUrl = mediaMeta.tabUrl || mediaMeta.frameUrl || curLoc;
    try {
      if (typeof window !== "undefined" && window.top && window.top.location && window.top.location.href) {
        effectivePageUrl = window.top.location.href;
      }
    } catch (_) {}
    const isSupportedSite = /bilibili\.com|b23\.tv|youtube\.com|youtu\.be|facebook\.com|fb\.watch|fbcdn\.net|pornhub\.com|phncdn\.com/i.test(effectivePageUrl) ||
                            /bilibili\.com|b23\.tv|facebook\.com|pornhub\.com/i.test(curLoc);
    if (isSupportedSite && (!videoSrc || videoSrc.startsWith("blob:"))) {
      videoSrc = effectivePageUrl;
    }

    safeSendRuntimeMessage({
      action: "downloadMedia",
      candidateId,
      qualityId,
      resolution,
      videoFilter,
      selectedQuality: chosenQ,
      url: videoSrc,
      isMseBlob: Boolean(mediaMeta.isMseBlob),
      // Browser media downloads must prompt the user via the New Download dialog before transferring
      autoStart: false,
      clickTime: Date.now(),
      pageTitle: mediaMeta.pageTitle || document.title || "video",
      tabUrl: mediaMeta.tabUrl || location.href,
      frameUrl: mediaMeta.frameUrl || location.href
    }, (resp, runtimeErr) => {
      const lastErr = runtimeErr || ((typeof chrome !== "undefined" && chrome.runtime && chrome.runtime.lastError)
        ? chrome.runtime.lastError.message
        : null);

      if (lastErr || !resp || !resp.success) {
        const errText = (resp && resp.error) || lastErr || "Please reload page to re-sync extension";
        if (btn) {
          btn.textContent = "Error: " + (errText.length > 28 ? errText.slice(0, 28) + "..." : errText);
          btn.title = errText;
          btn.disabled = false;
          if (btn.style) btn.style.cssText = baseStyle + "border-color:#EF4444;color:#FCA5A5;";
        }
        if (hostEl && hostEl.setAttribute) {
          hostEl.setAttribute("data-radium-status", "error");
          hostEl.setAttribute("data-flux-status", "error");
          hostEl.setAttribute("data-radium-error", errText);
          hostEl.setAttribute("data-flux-error", errText);
        }
      } else {
        const isFallback = Boolean(resp.result && resp.result.fallbackToBrowser);
        const taskId = (resp.result && resp.result.taskId) ? resp.result.taskId : "";
        if (btn) {
          btn.textContent = isFallback ? "Browser Download" : "Starting in Radium \u2713";
          btn.title = taskId ? ("Task ID: " + taskId) : "Queued in Radium";
          btn.disabled = false;
          if (btn.style) btn.style.cssText = baseStyle + "border-color:#22C55E;color:#86EFAC;";
        }
        if (hostEl && hostEl.setAttribute) {
          hostEl.setAttribute("data-radium-status", isFallback ? "browser_fallback" : "queued");
          hostEl.setAttribute("data-flux-status", isFallback ? "browser_fallback" : "queued");
          if (taskId) {
            hostEl.setAttribute("data-radium-task-id", taskId);
            hostEl.setAttribute("data-flux-task-id", taskId);
          }
        }
      }

      if (btn && typeof setTimeout === "function") {
        setTimeout(() => {
          btn.textContent = "Download with Radium";
          if (btn.style && baseStyle) btn.style.cssText = baseStyle;
        }, 3500);
      }
    });
  };

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", () => {
      observeDynamicVideosAndFullscreen(document, window, onDownloadClick);
    }, { once: true });
  } else {
    observeDynamicVideosAndFullscreen(document, window, onDownloadClick);
  }
}

// Auto-run in browser execution
if (typeof window !== "undefined" && typeof document !== "undefined") {
  initContentScript();
}

if (typeof module !== "undefined" && module.exports) {
  module.exports = {
    positionOverlayHost,
    scanAndAttachVideoOverlays,
    observeDynamicVideosAndFullscreen,
    findAnchorFromEvent,
    handleLinkClick,
    handleMediaPlaybackEvent,
    extractEligiblePageLinks,
    initContentScript
  };
}
