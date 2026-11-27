// Somike UI. Renders the state pushed by the native app (WebView2) and sends
// user actions back. Without WebView2 (plain browser) a mock backend is used,
// so the design can be previewed and iterated on: add ?scenario=setup|live|
// waiting|enhancements|empty to the URL.
"use strict";

const $ = (id) => document.getElementById(id);
const bridge = window.chrome && window.chrome.webview;

let state = null;
const icons = {};
let draggingGain = false;
let lastSourcesKey = "";
let lastMicsKey = "";

// ------------------------------------------------------------------ transport
function send(cmd, extra = {}) {
  const message = { cmd, ...extra };
  if (bridge) bridge.postMessage(message);
  else mock.handle(message);
}

function receive(message) {
  if (!message || typeof message !== "object") return;
  if (message.type === "state") {
    state = message.state;
    render();
  } else if (message.type === "icons") {
    Object.assign(icons, message.icons);
    lastSourcesKey = "";  // re-render tiles with the new icons
    if (state) render();
  } else if (message.type === "toast") {
    toast(message.text, message.tone);
  }
}

// ------------------------------------------------------------------ helpers
const escapeHtml = (text) =>
  String(text).replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[c]);

function shortMicName(name) {
  // "Mikrofon (Corsair HS80 RGB USB Gaming Headset)" -> "Corsair HS80 RGB USB Gaming Headset"
  const match = /\(([^)]+)\)\s*$/.exec(name || "");
  return match ? match[1] : name || "your microphone";
}

function avatarColor(text) {
  let hash = 0;
  for (const ch of text) hash = (hash * 31 + ch.charCodeAt(0)) >>> 0;
  const hue = hash % 360;
  return `linear-gradient(135deg, hsl(${hue} 70% 52%), hsl(${(hue + 40) % 360} 75% 42%))`;
}

let toastTimer = 0;
function toast(text, tone = "info") {
  const el = $("toast");
  el.textContent = text;
  el.dataset.tone = tone;
  el.hidden = false;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => (el.hidden = true), tone === "err" ? 6000 : 3500);
}

const ICON_SPEAKER =
  '<svg viewBox="0 0 24 24"><path d="M4 9.5h3.5L12 5.5v13l-4.5-4H4z"/><path d="M15.5 9a4 4 0 0 1 0 6M18 6.5a7.5 7.5 0 0 1 0 11"/></svg>';
const ICON_CHECK =
  '<svg viewBox="0 0 12 12"><path d="M2.5 6.2l2.3 2.3 4.7-4.9" fill="none" stroke="#fff" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"/></svg>';
const EQ = '<span class="eq" aria-hidden="true"><i></i><i></i><i></i></span>';

// ------------------------------------------------------------------ rendering
function render() {
  const s = state;
  const app = $("app");
  app.dataset.view = s.view;
  app.dataset.live = String(s.live.on && s.live.phase === "live");
  $("setup").hidden = s.view !== "setup";
  $("main").hidden = s.view !== "main";

  const mic = s.mics.find((m) => m.id === s.micId);
  const micName = shortMicName(mic && mic.name);
  const source = s.sources.find((x) => x.id === s.sourceId);
  const sourceName = source ? source.name : "";

  renderMics(s);
  renderPill(s);
  renderFooter(s, micName);

  if (s.view === "setup") {
    const busy = s.setup.busy;
    const btn = $("setup-btn");
    btn.classList.toggle("busy", busy);
    btn.disabled = busy || !mic;
    btn.querySelector(".btn-label").textContent = busy ? "Setting up…" : "Set up";
    const msg = $("setup-msg");
    msg.textContent = s.setup.message || (mic ? "" : "No microphone found. Plug one in.");
    msg.dataset.tone = s.setup.error ? "err" : "info";
    return;
  }

  // Banner: things that prevent mixing.
  const banner = $("banner");
  if (s.injector.enhancementsOff || s.injector.needsUpdate) {
    banner.hidden = false;
    const repair = s.injector.enhancementsOff;
    $("banner-text").textContent = repair
      ? "Windows turned off audio enhancements for this mic, so nothing can be mixed in."
      : "Update the mic connection so Discord and games hear Somike too.";
    $("banner-btn").textContent = s.setup.busy
      ? (repair ? "Repairing…" : "Updating…")
      : (repair ? "Repair" : "Update");
    $("banner-btn").disabled = s.setup.busy;
    $("banner-btn").dataset.cmd = "repair";
  } else {
    banner.hidden = true;
  }

  renderSources(s);

  // Live button.
  const btn = $("live-btn");
  const phase = s.live.on ? s.live.phase : "off";
  btn.setAttribute("aria-pressed", String(s.live.on));
  btn.dataset.waiting = String(phase === "waiting");
  btn.disabled = !source || !s.injector.installed;
  $("live-text").textContent = { off: "Go live", live: "Live", waiting: "Waiting", error: "Retrying" }[phase];

  const name = `<b>${escapeHtml(sourceName)}</b>`;
  let sub;
  if (!source) sub = "Pick what to play, then go live";
  else if (phase === "off") sub = `Plays ${name} into your mic`;
  else if (phase === "waiting") sub = `Waiting for ${name}. It connects as soon as it starts`;
  else if (phase === "error") sub = escapeHtml(s.live.message || "Something went wrong, retrying");
  else if (!s.injector.micInUse) sub = `Live. Mixing starts when an app uses your mic`;
  else if (!source.system && !source.active) sub = `Live. Play something in ${name}`;
  else sub = `${name} is playing in your mic`;
  $("live-sub").innerHTML = sub;

  const level = phase === "live" ? Math.min(1, Math.sqrt(s.level || 0)) : 0;
  $("meter-fill").style.width = `${Math.round(level * 100)}%`;

  // Controls.
  if (!draggingGain) setGainUi(Math.round(s.gain * 100));
  $("mute").checked = s.muteMic;
  $("mic-sub").textContent = s.micMutedInWindows
    ? "Muted in Windows, app audio still goes out"
    : s.injector.installed ? "Connected" : "Not connected";
}

function renderPill(s) {
  const pill = $("pill");
  let tone = "idle";
  let text = "Off";
  if (s.view === "setup") text = "Setup";
  else if (s.live.on) {
    tone = { live: "live", waiting: "warn", error: "err" }[s.live.phase] || "idle";
    text = { live: "Live", waiting: "Waiting", error: "Error" }[s.live.phase] || "Off";
  }
  pill.dataset.tone = tone;
  $("pill-text").textContent = text;
}

function renderFooter(s, micName) {
  const el = $("footer-status");
  let tone = "idle";
  let text = `Ready · ${micName}`;
  if (s.view === "setup") text = "Not connected to a microphone yet";
  else if (s.injector.enhancementsOff) { tone = "warn"; text = "Audio enhancements are off"; }
  else if (s.live.on && s.live.phase === "live") {
    if (s.injector.micInUse) { tone = "ok"; text = `Mixing into ${micName}`; }
    else { tone = "warn"; text = "No app is using your mic right now"; }
  } else if (s.live.on && s.live.phase === "waiting") { tone = "warn"; text = "Waiting for the app to play"; }
  else if (s.live.on && s.live.phase === "error") { tone = "err"; text = s.live.message || "Error"; }
  el.dataset.tone = tone;
  $("footer-text").textContent = text;
}

function renderMics(s) {
  const key = JSON.stringify([s.mics, s.micId]);
  if (key === lastMicsKey) return;
  lastMicsKey = key;
  for (const select of [$("mic"), $("setup-mic")]) {
    select.innerHTML = s.mics
      .map((m) => `<option value="${escapeHtml(m.id)}">${escapeHtml(shortMicName(m.name))}${m.isDefault ? " (default)" : ""}</option>`)
      .join("");
    select.value = s.micId;
  }
}

function renderSources(s) {
  const key = JSON.stringify([s.sources, s.sourceId]);
  if (key === lastSourcesKey) return;
  lastSourcesKey = key;
  const container = $("sources");
  container.innerHTML = s.sources.map((src) => tileHtml(src, src.id === s.sourceId)).join("");
  $("sources-hint").hidden = s.sources.some((x) => !x.system);
}

function tileHtml(src, selected) {
  let icon;
  if (src.system) icon = `<span class="tile-icon" style="background:var(--live)">${ICON_SPEAKER}</span>`;
  else if (icons[src.iconKey]) icon = `<span class="tile-icon"><img alt="" src="${icons[src.iconKey]}"></span>`;
  else icon = `<span class="tile-icon" style="background:${avatarColor(src.name)}">${escapeHtml(src.name.charAt(0).toUpperCase())}</span>`;

  let sub;
  if (src.system) sub = "Everything you hear";
  else if (src.active) sub = `${EQ}Playing`;
  else if (src.running) sub = "Quiet";
  else sub = "Not running";

  return `<button type="button" class="tile" role="radio" aria-checked="${selected}"
      data-id="${escapeHtml(src.id)}" data-active="${!!src.active}" data-running="${src.running !== false}" data-system="${!!src.system}"
      title="${escapeHtml(src.system ? "All system audio. Others in a voice chat may hear themselves." : src.name)}">
      ${icon}<span class="tile-name">${escapeHtml(src.name)}</span><span class="tile-sub">${sub}</span>
      <span class="tile-check">${ICON_CHECK}</span></button>`;
}

function setGainUi(percent) {
  const slider = $("gain");
  slider.value = percent;
  slider.style.setProperty("--fill", `${percent / 2}%`);
  $("gain-out").textContent = `${percent}%`;
}

// ------------------------------------------------------------------ events
function bindEvents() {
  $("sources").addEventListener("click", (e) => {
    const tile = e.target.closest(".tile");
    if (tile) send("selectSource", { id: tile.dataset.id });
  });
  $("live-btn").addEventListener("click", () => send("toggleLive"));
  $("setup-btn").addEventListener("click", () => send("setup"));
  $("banner-btn").addEventListener("click", (e) => send(e.currentTarget.dataset.cmd));
  $("refresh").addEventListener("click", (e) => {
    const btn = e.currentTarget;
    btn.classList.remove("spin");
    void btn.offsetWidth;
    btn.classList.add("spin");
    send("refresh");
  });

  let gainTimer = 0;
  const gain = $("gain");
  gain.addEventListener("input", () => {
    draggingGain = true;
    setGainUi(Number(gain.value));
    clearTimeout(gainTimer);
    gainTimer = setTimeout(() => send("setGain", { value: Number(gain.value) / 100 }), 30);
  });
  gain.addEventListener("change", () => {
    send("setGain", { value: Number(gain.value) / 100 });
    draggingGain = false;
  });
  $("mute").addEventListener("change", (e) => send("setMuteMic", { value: e.target.checked }));
  for (const select of [$("mic"), $("setup-mic")]) {
    select.addEventListener("change", (e) => send("selectMic", { id: e.target.value }));
  }

  const menu = $("menu");
  const more = $("more");
  more.addEventListener("click", (e) => {
    e.stopPropagation();
    menu.hidden = !menu.hidden;
    more.setAttribute("aria-expanded", String(!menu.hidden));
  });
  menu.addEventListener("click", (e) => {
    const item = e.target.closest("[data-cmd]");
    if (!item) return;
    menu.hidden = true;
    send(item.dataset.cmd);
  });
  document.addEventListener("click", () => { menu.hidden = true; });
  document.addEventListener("keydown", (e) => { if (e.key === "Escape") menu.hidden = true; });
  // No browser context menu / zoom in the app window.
  document.addEventListener("contextmenu", (e) => { if (bridge) e.preventDefault(); });
}

// ------------------------------------------------------------------ mock backend (browser preview only)
const mock = {
  s: null,
  timer: 0,
  init() {
    const scenario = new URLSearchParams(location.search).get("scenario") || "ready";
    this.s = {
      view: scenario === "setup" ? "setup" : "main",
      setup: { busy: false, message: "", error: false },
      mics: [
        { id: "mic-1", name: "Mikrofon (Corsair HS80 RGB USB Gaming Headset)", isDefault: true },
        { id: "mic-2", name: "Microphone (Realtek(R) Audio)", isDefault: false },
      ],
      micId: "mic-1",
      micMutedInWindows: false,
      injector: {
        installed: scenario !== "setup",
        enhancementsOff: scenario === "enhancements",
        needsUpdate: scenario === "update",
        micInUse: true,
      },
      sources: scenario === "empty" ? [] : [
        { id: "spotify.exe", name: "Spotify", running: true, active: true, iconKey: "spotify.exe" },
        { id: "chrome.exe", name: "Google Chrome", running: true, active: false, iconKey: "chrome.exe" },
        { id: "discord.exe", name: "Discord", running: true, active: false, iconKey: "discord.exe" },
        { id: "vlc.exe", name: "VLC media player", running: false, active: false, iconKey: "vlc.exe" },
      ],
      sourceId: scenario === "empty" ? "" : "spotify.exe",
      live: { on: false, phase: "off", message: "" },
      level: 0,
      gain: 1,
      muteMic: false,
    };
    this.s.sources.push({ id: "system", name: "All system audio", system: true, running: true, active: true });
    if (scenario === "empty") this.s.sourceId = "";
    if (scenario === "live") this.s.live = { on: true, phase: "live", message: "" };
    if (scenario === "waiting") { this.s.sourceId = "vlc.exe"; this.s.live = { on: true, phase: "waiting", message: "" }; }
    // ?still: no ticker/animations, for screenshots.
    if (new URLSearchParams(location.search).has("still")) {
      document.documentElement.classList.add("still");
      this.s.level = 0.45;
    } else {
      setInterval(() => this.tick(), 120);
    }
    this.push();
  },
  push() { receive({ type: "state", state: JSON.parse(JSON.stringify(this.s)) }); },
  tick() {
    const s = this.s;
    const src = s.sources.find((x) => x.id === s.sourceId);
    const target = s.live.on && s.live.phase === "live" && src && src.active ? 0.25 + Math.random() * 0.5 : 0;
    s.level = s.level * 0.5 + target * 0.5;
    this.push();
  },
  handle(m) {
    const s = this.s;
    switch (m.cmd) {
      case "selectSource": {
        s.sourceId = m.id;
        const src = s.sources.find((x) => x.id === m.id);
        if (s.live.on) s.live.phase = src && src.running !== false ? "live" : "waiting";
        break;
      }
      case "toggleLive": {
        const src = s.sources.find((x) => x.id === s.sourceId);
        s.live.on = !s.live.on;
        s.live.phase = !s.live.on ? "off" : src && src.running !== false ? "live" : "waiting";
        break;
      }
      case "setGain": s.gain = m.value; break;
      case "setMuteMic": s.muteMic = m.value; break;
      case "selectMic": s.micId = m.id; break;
      case "setup": case "repair":
        s.setup = { busy: true, message: "Waiting for Windows permission…", error: false };
        setTimeout(() => {
          s.setup = { busy: false, message: "", error: false };
          s.injector.installed = true; s.injector.enhancementsOff = false; s.injector.needsUpdate = false; s.view = "main";
          this.push(); toast("Connected. Somike is ready.");
        }, 1800);
        break;
      case "uninstall":
        s.injector.installed = false; s.view = "setup"; s.live = { on: false, phase: "off", message: "" };
        break;
      case "refresh": toast("App list refreshed"); break;
      case "openLogs": toast("Opens the log folder in the real app"); break;
    }
    this.push();
  },
};

// ------------------------------------------------------------------ start
bindEvents();
if (bridge) {
  bridge.addEventListener("message", (e) => receive(e.data));
  send("ready");
} else {
  mock.init();
}
