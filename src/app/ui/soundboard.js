// Somike soundboard UI (the "Soundboard" tab). Talks to the native
// SoundboardBridge (src/app/soundboard_bridge.h) with "sb.*" messages.
// Without WebView2 a mock backend is used, so the design can be previewed in a
// browser (tools/ui-dev/serve.mjs): add ?tab=sounds, &board=empty and
// &sheet=browse|edit to the URL.
(() => {
  "use strict";

  const $ = (id) => document.getElementById(id);
  const bridge = window.chrome && window.chrome.webview;
  const esc = (text) =>
    String(text).replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[c]);
  const notify = (text, tone = "info") => (typeof window.toast === "function" ? window.toast(text, tone) : console.log(text));
  const params = new URLSearchParams(location.search);

  const EMOJIS = ["", "📯", "👏", "🥁", "😂", "💥", "🔔", "🎺", "🦗", "😱", "🤡", "👻", "🐐", "🚨", "💨", "🎉",
    "🔥", "💀", "🙄", "😎", "🐱", "🐶", "🎵", "⭐", "❤️", "👋", "🤖", "🎮", "🍕", "🏆"];
  const COLORS = ["#8b5cf6", "#ec4899", "#f97316", "#eab308", "#22c55e", "#14b8a6", "#06b6d4", "#3b82f6", "#a855f7", "#ef4444"];
  const CHIPS = ["Air horn", "Applause", "Drum roll", "Rimshot", "Sad trombone", "Laugh", "Crickets", "Explosion", "Ding",
    "Whoosh", "Boing", "Cash register", "Gong", "Fanfare", "Fail", "Scream", "Wow", "Cat", "Dog", "Door knock"];
  const ICON_DOTS = '<svg viewBox="0 0 20 20"><circle cx="5" cy="10" r="1.5" fill="currentColor"/><circle cx="10" cy="10" r="1.5" fill="currentColor"/><circle cx="15" cy="10" r="1.5" fill="currentColor"/></svg>';
  const ICON_PLAY = '<svg viewBox="0 0 20 20"><path d="M6.5 4.5v11l9-5.5z" fill="currentColor"/></svg>';
  const ICON_STOP = '<svg viewBox="0 0 20 20"><rect x="5.5" y="5.5" width="9" height="9" rx="1.5" fill="currentColor"/></svg>';
  const MOD = { alt: 1, ctrl: 2, shift: 4, win: 8 };

  const sb = {
    library: null,
    playing: new Map(),  // id -> { len, t0 }
    loading: new Set(),
    filter: "",
    editing: "",
    capture: null,       // { target: soundId | "" (stop-all key), button }
    searching: null,     // { query, page } while a search runs
    results: null,       // { query, page, pageCount, total, items }
    online: new Map(),   // catalog id -> state
    lastPadsHtml: "",
    raf: 0,
  };

  // ------------------------------------------------------------------ transport
  function send(cmd, extra = {}) {
    const message = { cmd, ...extra };
    if (bridge) bridge.postMessage(message);
    else mock.handle(message);
  }

  function receive(m) {
    if (!m || typeof m !== "object" || typeof m.type !== "string" || !m.type.startsWith("sb.")) return;
    if (m.type === "sb.library") {
      sb.library = m.library;
      renderLibrary();
    } else if (m.type === "sb.playing") {
      const now = performance.now();
      sb.playing = new Map(m.playing.map((p) => [p.id, { len: p.len, t0: now - p.pos }]));
      sb.loading = new Set(m.loading);
      applyPlaying();
    } else if (m.type === "sb.results") {
      showResults(m);
    } else if (m.type === "sb.online") {
      sb.online.set(m.id, m.state);
      updateResultRow(m.id);
    }
  }

  // ------------------------------------------------------------------ helpers
  const safeColor = (c) => (/^#[0-9a-f]{6}$/i.test(c || "") ? c : COLORS[0]);
  const initial = (name) => (Array.from(String(name).trim())[0] || "?").toUpperCase();
  const findSound = (id) => (sb.library ? sb.library.sounds.find((s) => s.id === id) : undefined);
  const padEl = (id) => document.querySelector(`.sb-pad[data-id="${CSS.escape(id)}"]`);

  function formatDuration(ms) {
    if (!ms) return "";
    const s = ms / 1000;
    if (s < 10) return `${s.toFixed(1)} s`;
    if (s < 60) return `${Math.round(s)} s`;
    return `${Math.floor(s / 60)}:${String(Math.round(s % 60)).padStart(2, "0")}`;
  }

  function setSlider(slider, output, percent) {
    slider.value = percent;
    slider.style.setProperty("--fill", `${percent / 2}%`);
    output.textContent = `${percent}%`;
  }

  // A slider that sends its value while dragging (throttled) and does not jump
  // back when a state push arrives mid-drag.
  function bindSlider(slider, output, onValue) {
    let timer = 0;
    slider.addEventListener("input", () => {
      slider.dataset.dragging = "true";
      setSlider(slider, output, Number(slider.value));
      clearTimeout(timer);
      timer = setTimeout(() => onValue(Number(slider.value) / 100), 40);
    });
    slider.addEventListener("change", () => {
      clearTimeout(timer);
      onValue(Number(slider.value) / 100);
      delete slider.dataset.dragging;
    });
  }
  const syncSlider = (slider, output, value) => {
    if (slider.dataset.dragging !== "true") setSlider(slider, output, Math.round(value * 100));
  };

  function renderKeyButton(button, hotkey, failed) {
    if (sb.capture && sb.capture.button === button) return;
    const set = !!(hotkey && hotkey.vk);
    button.dataset.set = String(set);
    button.dataset.failed = String(set && !!failed);
    button.textContent = set ? hotkey.label || "Hotkey" : "Set hotkey";
    button.title = set && failed ? "Another program already uses this key. Pick a different one." : set ? "Click to change" : "";
  }

  // ------------------------------------------------------------------ board
  function padHtml(s) {
    const icon = s.emoji ? esc(s.emoji) : esc(initial(s.name));
    const key = s.hotkey && s.hotkey.vk
      ? `<kbd class="${s.hotkeyFailed ? "bad" : ""}" title="${s.hotkeyFailed ? "Used by another program" : "Hotkey"}">${esc(s.hotkey.label)}</kbd>`
      : "";
    const title = s.broken ? "The file is missing or can't be read" : `Play ${s.name}`;
    return `<div class="sb-pad" role="listitem" data-id="${esc(s.id)}" data-broken="${!!s.broken}" style="--pad:${safeColor(s.color)}">
      <button type="button" class="sb-pad-main" data-action="play" title="${esc(title)}" aria-label="${esc(`Play ${s.name}`)}">
        <span class="sb-pad-icon" aria-hidden="true">${icon}</span>
        <span class="sb-pad-name">${esc(s.name)}</span>
        <span class="sb-pad-meta">${key}<span>${s.broken ? "" : formatDuration(s.durationMs)}</span></span>
        <span class="sb-pad-progress" aria-hidden="true"><i></i></span>
      </button>
      <button type="button" class="icon-btn sb-pad-edit" data-action="edit" aria-label="${esc(`Edit ${s.name}`)}" title="Edit, hotkey, delete">${ICON_DOTS}</button>
    </div>`;
  }

  function renderLibrary() {
    const lib = sb.library;
    if (!lib) return;
    const st = lib.settings;
    syncSlider($("sb-mic-volume"), $("sb-mic-volume-out"), st.micVolume);
    syncSlider($("sb-monitor-volume"), $("sb-monitor-volume-out"), st.monitorVolume);
    $("sb-monitor").checked = st.monitor;
    $("sb-monitor-volume").disabled = !st.monitor;
    renderKeyButton($("sb-stop-key"), st.stopHotkey, st.stopHotkeyFailed);
    const stopKey = $("sb-stopall-key");
    stopKey.hidden = !(st.stopHotkey && st.stopHotkey.vk);
    stopKey.textContent = stopKey.hidden ? "" : st.stopHotkey.label;
    $("sb-notready").hidden = lib.ready;

    const filter = sb.filter.trim().toLowerCase();
    const shown = filter ? lib.sounds.filter((s) => s.name.toLowerCase().includes(filter)) : lib.sounds;
    const html = shown.map(padHtml).join("");
    if (html !== sb.lastPadsHtml) {
      const active = document.activeElement;
      const focusPad = active && active.closest && active.closest(".sb-pad");
      const focus = focusPad ? [focusPad.dataset.id, active.dataset.action] : null;
      sb.lastPadsHtml = html;
      $("sb-pads").innerHTML = html;
      if (focus) {
        const pad = padEl(focus[0]);
        const target = pad && pad.querySelector(`[data-action="${focus[1]}"]`);
        if (target) target.focus();
      }
    }
    $("sb-empty").hidden = lib.sounds.length > 0;
    document.querySelector(".sb-strip").hidden = lib.sounds.length === 0;
    $("sb-nomatch").hidden = !(lib.sounds.length > 0 && shown.length === 0);
    applyPlaying();
    if (sb.editing) fillEdit(false);
  }

  function applyPlaying() {
    for (const pad of $("sb-pads").children) {
      const playing = sb.playing.has(pad.dataset.id);
      pad.dataset.playing = String(playing);
      pad.dataset.loading = String(sb.loading.has(pad.dataset.id));
      if (!playing) pad.querySelector(".sb-pad-progress i").style.width = "0%";
    }
    $("sb-stopall").hidden = sb.playing.size === 0;
    $("tab-sounds").dataset.playing = String(sb.playing.size > 0);
    if (sb.playing.size && !sb.raf) sb.raf = requestAnimationFrame(animateProgress);
  }

  function animateProgress() {
    sb.raf = 0;
    const now = performance.now();
    for (const [id, p] of sb.playing) {
      const pad = padEl(id);
      if (!pad) continue;
      const fraction = p.len > 0 ? Math.min(1, (now - p.t0) / p.len) : 1;
      pad.querySelector(".sb-pad-progress i").style.width = `${(fraction * 100).toFixed(2)}%`;
    }
    if (sb.playing.size) sb.raf = requestAnimationFrame(animateProgress);
  }

  // ------------------------------------------------------------------ tabs
  function setTab(tab) {
    const value = tab === "sounds" ? "sounds" : "mic";
    $("app").dataset.tab = value;
    for (const button of document.querySelectorAll(".tab")) {
      const selected = button.dataset.tab === value;
      button.setAttribute("aria-selected", String(selected));
      button.tabIndex = selected ? 0 : -1;
    }
    try { localStorage.setItem("somike.tab", value); } catch { /* private mode */ }
  }

  // ------------------------------------------------------------------ sheets
  let sheetReturnFocus = null;
  function openSheet(sheet) {
    if (sheet.hidden) sheetReturnFocus = document.activeElement;
    sheet.hidden = false;
  }
  function closeSheet(sheet) {
    if (sheet.hidden) return;
    stopCapture();
    sheet.hidden = true;
    if (sheet.id === "sb-edit") {
      flushName();
      sb.editing = "";
      resetDelete();
    }
    if (sheet.id === "sb-browse-sheet") {
      for (const [id, state] of sb.online) {
        if (state === "previewing" || state === "loading") send("sb.preview", { id });  // toggles it off
      }
    }
    if (sheetReturnFocus && document.contains(sheetReturnFocus)) sheetReturnFocus.focus();
  }
  const openSheets = () => [...document.querySelectorAll(".sheet:not([hidden])")];

  // ------------------------------------------------------------------ edit sheet
  let nameTimer = 0;
  let deleteTimer = 0;
  function openEdit(id) {
    if (!findSound(id)) return;
    sb.editing = id;
    resetDelete();
    fillEdit(true);
    openSheet($("sb-edit"));
    $("sb-edit-name").focus();
    $("sb-edit-name").select();
  }

  function fillEdit(full) {
    const s = findSound(sb.editing);
    if (!s) {
      closeSheet($("sb-edit"));
      return;
    }
    const name = $("sb-edit-name");
    if (full || document.activeElement !== name) name.value = s.name;
    const icon = $("sb-edit-icon");
    icon.textContent = s.emoji || initial(s.name);
    icon.style.setProperty("--pad", safeColor(s.color));

    if (full || !$("sb-emojis").childElementCount) {
      $("sb-emojis").innerHTML = EMOJIS.map((e) => e
        ? `<button type="button" data-emoji="${esc(e)}" aria-label="${esc(e)}">${esc(e)}</button>`
        : `<button type="button" class="no-emoji" data-emoji="" title="First letter of the name">Aa</button>`).join("");
      $("sb-colors").innerHTML = COLORS.map((c) => `<button type="button" data-color="${c}" style="--swatch:${c}" aria-label="Colour ${c}"></button>`).join("");
    }
    for (const b of $("sb-emojis").children) b.setAttribute("aria-pressed", String(b.dataset.emoji === (s.emoji || "")));
    for (const b of $("sb-colors").children) b.setAttribute("aria-pressed", String(b.dataset.color.toLowerCase() === safeColor(s.color).toLowerCase()));

    syncSlider($("sb-edit-volume"), $("sb-edit-volume-out"), s.volume);
    renderKeyButton($("sb-edit-key"), s.hotkey, s.hotkeyFailed);
    $("sb-edit-key-clear").hidden = !(s.hotkey && s.hotkey.vk);
    $("sb-edit-key-hint").textContent = s.hotkeyFailed
      ? "Another program already uses this key"
      : "Plays the sound from any app, even in games";

    const credit = $("sb-edit-credit");
    if (s.credit && (s.credit.title || s.credit.creator)) {
      const license = (s.credit.license || "").toUpperCase().replace("PDM", "Public domain");
      const link = s.credit.url ? ` · <button type="button" class="link" data-link="${esc(s.credit.url)}">source</button>` : "";
      credit.innerHTML = `<b>${esc(s.credit.title)}</b>${s.credit.creator ? ` by ${esc(s.credit.creator)}` : ""} · ${esc(license)}${link}`;
      credit.hidden = false;
    } else {
      credit.hidden = true;
    }
  }

  function flushName() {
    clearTimeout(nameTimer);
    const s = findSound(sb.editing);
    const value = $("sb-edit-name").value.trim();
    if (s && value && value !== s.name) send("sb.rename", { id: s.id, value });
  }

  function resetDelete() {
    clearTimeout(deleteTimer);
    const button = $("sb-edit-delete");
    button.classList.remove("confirm");
    button.textContent = "Delete";
  }

  // ------------------------------------------------------------------ hotkeys
  const MODIFIER_CODES = new Set(["ControlLeft", "ControlRight", "AltLeft", "AltRight", "ShiftLeft", "ShiftRight",
    "MetaLeft", "MetaRight", "ContextMenu", "CapsLock", "NumLock"]);

  function modsLabel(mods) {
    return [mods & MOD.ctrl && "Ctrl", mods & MOD.alt && "Alt", mods & MOD.shift && "Shift", mods & MOD.win && "Win"]
      .filter(Boolean).join("+");
  }

  function keyName(e) {
    const code = e.code || "";
    let m;
    if ((m = /^Key([A-Z])$/.exec(code))) return m[1];
    if ((m = /^Digit(\d)$/.exec(code))) return m[1];
    if ((m = /^Numpad(\d)$/.exec(code))) return `Num ${m[1]}`;
    const named = {
      NumpadAdd: "Num +", NumpadSubtract: "Num -", NumpadMultiply: "Num *", NumpadDivide: "Num /", NumpadDecimal: "Num .",
      ArrowUp: "↑", ArrowDown: "↓", ArrowLeft: "←", ArrowRight: "→", Space: "Space", Enter: "Enter", Tab: "Tab",
      Insert: "Ins", Home: "Home", End: "End", PageUp: "PgUp", PageDown: "PgDn", Pause: "Pause", ScrollLock: "ScrLk",
      Backquote: "`", Minus: "-", Equal: "=", BracketLeft: "[", BracketRight: "]", Backslash: "\\", Semicolon: ";",
      Quote: "'", Comma: ",", Period: ".", Slash: "/", IntlBackslash: "<",
      MediaPlayPause: "Play/Pause", MediaTrackNext: "Next track", MediaTrackPrevious: "Previous track", MediaStop: "Media stop",
    };
    if (named[code]) return named[code];
    if (/^F\d{1,2}$/.test(code)) return code;
    return e.key && e.key.length === 1 ? e.key.toUpperCase() : code || "Key";
  }

  // Mirror of IsAllowedHotkey() in src/app/soundboard_library.cpp.
  function hotkeyProblem(vk, mods) {
    if (!vk || vk > 0xfe) return "That key can't be used";
    if (vk === 0x4d && mods === (MOD.ctrl | MOD.alt)) return "Ctrl+Alt+M is Somike's go-live key";
    if (mods !== 0 && mods !== MOD.shift) return "";
    const fKey = vk >= 0x70 && vk <= 0x87;
    const numpad = vk >= 0x60 && vk <= 0x6f;
    const media = vk >= 0xad && vk <= 0xb7;
    const special = vk === 0x13 || vk === 0x91;
    return fKey || numpad || media || special ? "" : "Add Ctrl, Alt or Win (F-keys and the number pad work alone)";
  }

  function startCapture(target, button) {
    stopCapture();
    sb.capture = { target, button };
    button.classList.add("recording");
    button.dataset.failed = "false";
    button.textContent = "Press keys…";
    send("sb.capture", { value: true });
  }

  function stopCapture() {
    if (!sb.capture) return;
    const { button } = sb.capture;
    sb.capture = null;
    button.classList.remove("recording");
    send("sb.capture", { value: false });
    if (sb.library) {
      renderKeyButton($("sb-stop-key"), sb.library.settings.stopHotkey, sb.library.settings.stopHotkeyFailed);
      if (sb.editing) fillEdit(false);
    }
  }

  function onCaptureKey(e) {
    if (!sb.capture) return;
    e.preventDefault();
    e.stopPropagation();
    const { target, button } = sb.capture;
    if (e.key === "Escape") return stopCapture();
    if (e.key === "Backspace" || e.key === "Delete") {
      send("sb.hotkey", { id: target, vk: 0, mods: 0, label: "" });
      return stopCapture();
    }
    const mods = (e.altKey ? MOD.alt : 0) | (e.ctrlKey ? MOD.ctrl : 0) | (e.shiftKey ? MOD.shift : 0) | (e.metaKey ? MOD.win : 0);
    if (MODIFIER_CODES.has(e.code)) {
      button.textContent = mods ? `${modsLabel(mods)}+…` : "Press keys…";
      return;
    }
    const vk = e.keyCode;
    const problem = hotkeyProblem(vk, mods);
    if (problem) {
      button.textContent = problem.length > 26 ? "Add Ctrl or Alt" : problem;
      button.title = problem;
      notify(problem, "err");
      return;
    }
    const label = [modsLabel(mods), keyName(e)].filter(Boolean).join("+");
    if (sb.library) {
      const sameKeys = (k) => k && k.vk === vk && k.mods === mods;
      const other = sb.library.sounds.find((s) => s.id !== target && sameKeys(s.hotkey));
      if (other) notify(`${label} moved from “${other.name}”`);
      else if (target && sameKeys(sb.library.settings.stopHotkey)) notify(`${label} no longer stops all sounds`);
    }
    send("sb.hotkey", { id: target, vk, mods, label });
    stopCapture();
  }

  // ------------------------------------------------------------------ free sounds
  function openBrowse() {
    openSheet($("sb-browse-sheet"));
    if (!sb.results && !sb.searching) renderResultsIntro();
    $("sb-query").focus();
  }

  function search(query, page = 1) {
    query = query.trim();
    if (!query) return;
    sb.searching = { query, page };
    $("sb-query").value = query;
    for (const chip of $("sb-chips").children) chip.setAttribute("aria-pressed", String(chip.textContent.toLowerCase() === query.toLowerCase()));
    if (page === 1) {
      $("sb-results").innerHTML = '<div class="sb-skeleton"></div>'.repeat(6);
      $("sb-results").scrollTop = 0;
    } else {
      const more = $("sb-results").querySelector(".sb-more .btn");
      if (more) { more.disabled = true; more.textContent = "Loading…"; }
    }
    send("sb.search", { query, page });
  }

  function showResults(m) {
    if (!sb.searching || m.query !== sb.searching.query || m.page !== sb.searching.page) return;  // outdated
    sb.searching = null;
    if (m.error) {
      if (m.page > 1 && sb.results) {
        notify(m.error, "err");
        renderResults();
        return;
      }
      sb.results = null;
      $("sb-results").innerHTML = `<div class="sb-results-note"><b>Couldn't load sounds</b>${esc(m.error)}
        <div><button type="button" class="btn btn-small" data-retry="${esc(m.query)}">Try again</button></div></div>`;
      return;
    }
    for (const item of m.items) sb.online.set(item.id, item.state);
    if (m.page === 1 || !sb.results) sb.results = { query: m.query, page: m.page, pageCount: m.pageCount, total: m.total, items: m.items };
    else Object.assign(sb.results, { page: m.page, pageCount: m.pageCount, items: sb.results.items.concat(m.items) });
    renderResults();
  }

  function renderResultsIntro() {
    $("sb-results").innerHTML = `<div class="sb-results-note"><b>Thousands of free sound effects</b>
      Pick a category above or search in English. Listen first, then add the ones you like.</div>`;
  }

  function resultHtml(item) {
    const state = sb.online.get(item.id) || item.state || "idle";
    const meta = [formatDuration(item.durationMs), item.creator && `by ${item.creator}`,
      item.license === "pdm" ? "Public domain" : "CC0", item.source === "wikimedia_audio" ? "Wikimedia" : item.source === "freesound" ? "Freesound" : ""]
      .filter(Boolean).join(" · ");
    return `<div class="sb-result" data-id="${esc(item.id)}" data-state="${esc(state)}">
      <button type="button" class="sb-listen" data-action="preview" aria-label="${esc(`Listen to ${item.title}`)}" title="Listen (only you hear it)">${state === "previewing" ? ICON_STOP : ICON_PLAY}</button>
      <div class="sb-result-text"><b title="${esc(item.title)}">${esc(item.title)}</b><small>${esc(meta)}</small></div>
      <button type="button" class="btn btn-small sb-result-add" data-action="add">${addLabel(state)}</button>
    </div>`;
  }

  const addLabel = (state) =>
    state === "adding" ? '<span class="spinner" aria-hidden="true"></span>Adding'
      : state === "added" ? "✓ Added"
      : "+ Add";

  function renderResults() {
    const r = sb.results;
    if (!r) return renderResultsIntro();
    if (!r.items.length) {
      $("sb-results").innerHTML = `<div class="sb-results-note"><b>No free sounds for “${esc(r.query)}”</b>Try another word, in English (e.g. “horn” instead of “air horn sound”).</div>`;
      return;
    }
    const more = r.page < r.pageCount ? '<div class="sb-more"><button type="button" class="btn btn-small" data-more>Show more</button></div>' : "";
    $("sb-results").innerHTML = r.items.map(resultHtml).join("") + more;
  }

  function updateResultRow(id) {
    const row = document.querySelector(`.sb-result[data-id="${CSS.escape(id)}"]`);
    if (!row) return;
    const state = sb.online.get(id) || "idle";
    row.dataset.state = state;
    row.querySelector(".sb-listen").innerHTML = state === "previewing" ? ICON_STOP : ICON_PLAY;
    row.querySelector(".sb-result-add").innerHTML = addLabel(state);
  }

  // ------------------------------------------------------------------ drag & drop
  let dragDepth = 0;
  const hasFiles = (e) => e.dataTransfer && [...e.dataTransfer.types].includes("Files");
  const dropAllowed = () => $("app").dataset.view === "main";
  function bindDrop() {
    document.addEventListener("dragenter", (e) => {
      if (!hasFiles(e)) return;
      e.preventDefault();
      dragDepth++;
      if (dropAllowed()) $("sb-drop").hidden = false;
    });
    document.addEventListener("dragover", (e) => {
      if (!hasFiles(e)) return;
      e.preventDefault();
      e.dataTransfer.dropEffect = dropAllowed() ? "copy" : "none";
    });
    document.addEventListener("dragleave", (e) => {
      if (!hasFiles(e)) return;
      dragDepth = Math.max(0, dragDepth - 1);
      if (!dragDepth) $("sb-drop").hidden = true;
    });
    document.addEventListener("drop", (e) => {
      if (!hasFiles(e)) return;
      e.preventDefault();
      dragDepth = 0;
      $("sb-drop").hidden = true;
      if (!dropAllowed()) return;
      const files = [...e.dataTransfer.files];
      if (!files.length) return;
      setTab("sounds");
      if (bridge && bridge.postMessageWithAdditionalObjects) bridge.postMessageWithAdditionalObjects({ cmd: "sb.drop" }, e.dataTransfer.files);
      else if (bridge) notify("Use “Add files” to add sounds (this WebView2 version can't receive dropped files).", "err");
      else mock.drop(files.map((f) => f.name));
    });
  }

  // ------------------------------------------------------------------ events
  function bindEvents() {
    const tabs = $("tabs");
    tabs.addEventListener("click", (e) => {
      const tab = e.target.closest(".tab");
      if (tab) setTab(tab.dataset.tab);
    });
    tabs.addEventListener("keydown", (e) => {
      if (e.key !== "ArrowLeft" && e.key !== "ArrowRight") return;
      const next = $("app").dataset.tab === "sounds" ? "mic" : "sounds";
      setTab(next);
      $(next === "sounds" ? "tab-sounds" : "tab-mic").focus();
      e.preventDefault();
    });

    $("sb-pads").addEventListener("click", (e) => {
      const button = e.target.closest("[data-action]");
      const pad = e.target.closest(".sb-pad");
      if (!button || !pad) return;
      if (button.dataset.action === "edit") openEdit(pad.dataset.id);
      else send("sb.play", { id: pad.dataset.id });
    });
    $("sb-pads").addEventListener("contextmenu", (e) => {
      const pad = e.target.closest(".sb-pad");
      if (!pad) return;
      e.preventDefault();
      openEdit(pad.dataset.id);
    });

    $("sb-filter").addEventListener("input", (e) => {
      sb.filter = e.target.value;
      renderLibrary();
    });
    $("sb-add").addEventListener("click", () => send("sb.import"));
    $("sb-browse").addEventListener("click", openBrowse);
    $("sb-empty").addEventListener("click", (e) => {
      const action = e.target.closest("[data-sb]");
      if (!action) return;
      if (action.dataset.sb === "browse") openBrowse();
      else send("sb.import");
    });
    $("sb-stopall").addEventListener("click", () => send("sb.stopAll"));

    bindSlider($("sb-mic-volume"), $("sb-mic-volume-out"), (v) => send("sb.settings", { micVolume: v }));
    bindSlider($("sb-monitor-volume"), $("sb-monitor-volume-out"), (v) => send("sb.settings", { monitorVolume: v }));
    $("sb-monitor").addEventListener("change", (e) => {
      $("sb-monitor-volume").disabled = !e.target.checked;
      send("sb.settings", { monitor: e.target.checked });
    });
    $("sb-stop-key").addEventListener("click", (e) => startCapture("", e.currentTarget));

    // Edit sheet.
    $("sb-edit-name").addEventListener("input", () => {
      clearTimeout(nameTimer);
      nameTimer = setTimeout(flushName, 500);
    });
    $("sb-edit-name").addEventListener("change", flushName);
    $("sb-edit-name").addEventListener("keydown", (e) => {
      if (e.key === "Enter") {
        flushName();
        closeSheet($("sb-edit"));
      }
    });
    $("sb-emojis").addEventListener("click", (e) => {
      const b = e.target.closest("[data-emoji]");
      const s = findSound(sb.editing);
      if (b && s) send("sb.look", { id: s.id, emoji: b.dataset.emoji, color: s.color });
    });
    $("sb-colors").addEventListener("click", (e) => {
      const b = e.target.closest("[data-color]");
      const s = findSound(sb.editing);
      if (b && s) send("sb.look", { id: s.id, emoji: s.emoji, color: b.dataset.color });
    });
    bindSlider($("sb-edit-volume"), $("sb-edit-volume-out"), (v) => sb.editing && send("sb.volume", { id: sb.editing, value: v }));
    $("sb-edit-key").addEventListener("click", (e) => startCapture(sb.editing, e.currentTarget));
    $("sb-edit-key-clear").addEventListener("click", () => send("sb.hotkey", { id: sb.editing, vk: 0, mods: 0, label: "" }));
    const test = () => sb.editing && send("sb.test", { id: sb.editing });
    $("sb-edit-icon").addEventListener("click", test);
    $("sb-edit-test").addEventListener("click", test);
    $("sb-edit-delete").addEventListener("click", (e) => {
      const button = e.currentTarget;
      const s = findSound(sb.editing);
      if (!s) return;
      if (!button.classList.contains("confirm")) {
        button.classList.add("confirm");
        button.textContent = "Click again to delete";
        clearTimeout(deleteTimer);
        deleteTimer = setTimeout(resetDelete, 3500);
        return;
      }
      send("sb.remove", { id: s.id });
      closeSheet($("sb-edit"));
      notify(`Deleted “${s.name}”`);
    });

    // Browse sheet.
    $("sb-chips").innerHTML = CHIPS.map((c) => `<button type="button" aria-pressed="false">${esc(c)}</button>`).join("");
    $("sb-chips").addEventListener("click", (e) => {
      const chip = e.target.closest("button");
      if (chip) search(chip.textContent);
    });
    // The chip row scrolls sideways without a scrollbar: let a mouse wheel do it.
    $("sb-chips").addEventListener("wheel", (e) => {
      const row = e.currentTarget;
      if (Math.abs(e.deltaY) <= Math.abs(e.deltaX) || row.scrollWidth <= row.clientWidth) return;
      row.scrollLeft += e.deltaY;
      e.preventDefault();
    }, { passive: false });
    $("sb-search-form").addEventListener("submit", (e) => {
      e.preventDefault();
      search($("sb-query").value);
    });
    $("sb-results").addEventListener("click", (e) => {
      const retry = e.target.closest("[data-retry]");
      if (retry) return search(retry.dataset.retry);
      if (e.target.closest("[data-more]") && sb.results) return search(sb.results.query, sb.results.page + 1);
      const button = e.target.closest("[data-action]");
      const row = e.target.closest(".sb-result");
      if (!button || !row) return;
      send(button.dataset.action === "add" ? "sb.add" : "sb.preview", { id: row.dataset.id });
    });

    // Shared: closing sheets, outbound links, capture.
    document.addEventListener("click", (e) => {
      const close = e.target.closest("[data-close]");
      if (close) closeSheet(close.closest(".sheet"));
      const link = e.target.closest("[data-link]");
      if (link) {
        e.preventDefault();
        if (bridge) send("sb.openLink", { url: link.dataset.link });
        else window.open(link.dataset.link, "_blank", "noopener");
      }
    });
    document.addEventListener("mousedown", (e) => {
      if (sb.capture && !e.target.closest(".key-btn")) stopCapture();
    });
    document.addEventListener("keydown", onCaptureKey, true);
    document.addEventListener("keydown", (e) => {
      if (e.key !== "Escape" || sb.capture) return;
      const sheets = openSheets();
      if (sheets.length) {
        e.preventDefault();
        closeSheet(sheets[sheets.length - 1]);
      }
    });
    // Keep Tab inside an open sheet.
    document.addEventListener("keydown", (e) => {
      if (e.key !== "Tab") return;
      const sheet = openSheets().pop();
      if (!sheet) return;
      const items = [...sheet.querySelectorAll("button, input, [tabindex]")].filter((el) => !el.disabled && el.offsetParent !== null && el.tabIndex >= 0);
      if (!items.length) return;
      const first = items[0];
      const last = items[items.length - 1];
      if (e.shiftKey && document.activeElement === first) { e.preventDefault(); last.focus(); }
      else if (!e.shiftKey && document.activeElement === last) { e.preventDefault(); first.focus(); }
      else if (!sheet.contains(document.activeElement)) { e.preventDefault(); first.focus(); }
    });
    bindDrop();
  }

  // ------------------------------------------------------------------ mock backend (browser preview only)
  const mock = {
    lib: null,
    voices: new Map(),  // id -> { start, len }
    timer: 0,
    nextId: 100,
    init() {
      const sample = [
        ["Air horn", "📯", "#ec4899", 2571, { vk: 0x61, mods: 0, label: "Num 1" }],
        ["Applause", "👏", "#22c55e", 6320, { vk: 0x62, mods: 0, label: "Num 2" }],
        ["Drum roll", "🥁", "#f97316", 3079, { vk: 0x31, mods: 3, label: "Ctrl+Alt+1" }],
        ["Sad trombone", "🎺", "#eab308", 5205, { vk: 0x63, mods: 0, label: "Num 3" }],
        ["Crickets", "🦗", "#14b8a6", 15589, null],
        ["Evil laugh", "😂", "#a855f7", 3100, { vk: 0x7c, mods: 0, label: "F13" }],
        ["Explosion", "💥", "#ef4444", 4200, null],
        ["Ding", "🔔", "#06b6d4", 900, { vk: 0x44, mods: 3, label: "Ctrl+Alt+D" }],
        ["Victory fanfare", "", "#3b82f6", 7400, null],
      ];
      this.lib = {
        ready: true,
        settings: { micVolume: 0.8, monitor: true, monitorVolume: 0.5, stopHotkey: { vk: 0x13, mods: 0, label: "Pause" }, stopHotkeyFailed: false },
        sounds: params.get("board") === "empty" ? [] : sample.map(([name, emoji, color, durationMs, hotkey], i) => ({
          id: `mock${i}`, name, emoji, color, volume: 1, durationMs, hotkey: hotkey || { vk: 0, mods: 0, label: "" },
          hotkeyFailed: name === "Ding", broken: false,
          credit: i < 3 ? { title: `${name} 01.wav`, creator: "someone", license: "cc0", url: "https://freesound.org/" } : null,
        })),
      };
      this.push();
    },
    push() { receive({ type: "sb.library", library: JSON.parse(JSON.stringify(this.lib)) }); },
    pushPlaying() {
      const now = performance.now();
      const playing = [];
      for (const [id, v] of this.voices) {
        const pos = now - v.start;
        if (pos >= v.len) this.voices.delete(id);
        else playing.push({ id, pos: Math.round(pos), len: v.len });
      }
      receive({ type: "sb.playing", playing, loading: [] });
      if (!this.voices.size) {
        clearInterval(this.timer);
        this.timer = 0;
      }
    },
    play(id) {
      const s = this.lib.sounds.find((x) => x.id === id);
      if (!s) return;
      if (this.voices.has(id)) this.voices.delete(id);
      else this.voices.set(id, { start: performance.now(), len: s.durationMs });
      if (!this.timer) this.timer = setInterval(() => this.pushPlaying(), 250);
      this.pushPlaying();
    },
    drop(names) {
      for (const name of names) {
        this.lib.sounds.push({ id: `mock${this.nextId++}`, name: name.replace(/\.[^.]+$/, "").replace(/_/g, " "), emoji: "",
          color: COLORS[this.nextId % COLORS.length], volume: 1, durationMs: 2400, hotkey: { vk: 0, mods: 0, label: "" }, hotkeyFailed: false, broken: false, credit: null });
      }
      this.push();
      notify(names.length === 1 ? `Added “${names[0]}”` : `Added ${names.length} sounds`);
    },
    online(id, state) { receive({ type: "sb.online", id, state }); },
    handle(m) {
      const s = this.lib.sounds.find((x) => x.id === m.id);
      switch (m.cmd) {
        case "sb.play": this.play(m.id); return;
        case "sb.stopAll": this.voices.clear(); this.pushPlaying(); return;
        case "sb.test": notify("Plays only on your speakers in the real app"); return;
        case "sb.import": this.drop(["My recording.mp3"]); return;
        case "sb.rename": if (s) s.name = m.value; break;
        case "sb.look": if (s) { s.emoji = m.emoji; s.color = m.color; } break;
        case "sb.volume": if (s) s.volume = m.value; break;
        case "sb.remove": this.lib.sounds = this.lib.sounds.filter((x) => x.id !== m.id); break;
        case "sb.hotkey": {
          const key = { vk: m.vk, mods: m.mods, label: m.label };
          for (const other of this.lib.sounds) if (other.hotkey.vk === m.vk && other.hotkey.mods === m.mods) other.hotkey = { vk: 0, mods: 0, label: "" };
          if (m.id) { if (s) s.hotkey = key; } else this.lib.settings.stopHotkey = key;
          break;
        }
        case "sb.settings": Object.assign(this.lib.settings, Object.fromEntries(Object.entries(m).filter(([k]) => k !== "cmd"))); break;
        case "sb.search":
          setTimeout(() => {
            const items = m.query.toLowerCase() === "nothing" ? [] : Array.from({ length: 12 }, (_, i) => ({
              id: `00000000-0000-0000-0000-${String(m.page * 100 + i).padStart(12, "0")}`,
              title: `${m.query} ${["(short)", "loud", "classic", "01", "reverb", "far away"][i % 6]}`.trim(),
              creator: ["mcpable", "zar.265", "InspectorJ", "qubodup"][i % 4], license: i % 5 ? "cc0" : "pdm",
              source: i % 4 ? "freesound" : "wikimedia_audio", durationMs: 800 + i * 900, state: "idle",
            }));
            receive({ type: "sb.results", query: m.query, page: m.page, pageCount: 3, total: 36, error: "", items });
          }, 450);
          return;
        case "sb.preview": {
          const current = [...sb.online].find(([, st]) => st === "previewing" || st === "loading");
          if (current) this.online(current[0], "idle");
          if (current && current[0] === m.id) return;
          this.online(m.id, "loading");
          setTimeout(() => {
            this.online(m.id, "previewing");
            setTimeout(() => sb.online.get(m.id) === "previewing" && this.online(m.id, "idle"), 2200);
          }, 350);
          return;
        }
        case "sb.add": {
          const item = sb.results && sb.results.items.find((x) => x.id === m.id);
          this.online(m.id, "adding");
          setTimeout(() => {
            this.online(m.id, "added");
            this.drop([item ? item.title : "Sound"]);
          }, 700);
          return;
        }
        case "sb.capture": case "sb.openFolder": case "sb.openLink": return;
      }
      this.push();
    },
  };

  // ------------------------------------------------------------------ start
  bindEvents();
  let initialTab = params.get("tab");
  if (!initialTab) {
    try { initialTab = localStorage.getItem("somike.tab"); } catch { /* private mode */ }
  }
  setTab(initialTab);
  if (bridge) {
    bridge.addEventListener("message", (e) => receive(e.data));
  } else {
    mock.init();
    const sheet = params.get("sheet");
    if (sheet === "browse") {
      openBrowse();
      search("Air horn");
    } else if (sheet === "edit" && mock.lib.sounds.length) {
      openEdit(mock.lib.sounds[0].id);
    }
  }
})();
