// UI test matrix: renders every mock scenario plus edge cases (long names, many
// apps, no microphone, errors, open menu, toast) in headless Microsoft Edge at
// typical window sizes and display scales, checks the layout and the console,
// and saves a screenshot per case.
//
//   node tools/ui-dev/serve.mjs                 (keep running)
//   node tools/ui-dev/matrix.mjs [outDir]       exit code 1 if any case has issues
import { spawn } from "node:child_process";
import { existsSync, mkdirSync, mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";

const base = process.env.SOMIKE_UI_URL || "http://localhost:5799/";
const outDir = process.argv[2] || join(tmpdir(), "somike-ui-matrix");
const port = 9333;

// Window client sizes in DIPs and the display scale they typically come with.
const sizes = [
  { name: "min", width: 400, height: 600, scale: 1 },          // smallest window
  { name: "default", width: 480, height: 800, scale: 1 },      // first start, 100 %
  { name: "laptop125", width: 480, height: 550, scale: 1.25 }, // 1366x768 at 125 %
  { name: "fhd150", width: 480, height: 663, scale: 1.5 },     // 1920x1080 at 150 %
  { name: "uhd200", width: 480, height: 800, scale: 2 },       // 4K at 200 %
  { name: "wide", width: 1280, height: 900, scale: 1 },        // maximized
];

const longName = "Super Long Application Name That Never Ends Deluxe Edition 2026";
const cases = [
  { name: "ready", scenario: "ready" },
  { name: "live", scenario: "live" },
  { name: "waiting", scenario: "waiting" },
  { name: "setup", scenario: "setup" },
  { name: "enhancements", scenario: "enhancements" },
  { name: "update", scenario: "update" },
  { name: "empty", scenario: "empty" },
  {
    name: "long-names", scenario: "live", apply: `
      mock.s.sources = Array.from({ length: 14 }, (_, i) => ({ id: "app" + i + ".exe",
        name: i % 3 === 0 ? ${JSON.stringify(longName)} + " " + i : "App " + i, running: i % 4 !== 0,
        active: i % 2 === 0, iconKey: "none" }));
      mock.s.sources.push({ id: "system", name: "All system audio", system: true, running: true, active: true });
      mock.s.sourceId = "app0.exe";
      mock.s.mics[0].name = "Mikrofon (" + ${JSON.stringify(longName)} + " USB Audio Device)";`,
  },
  { name: "no-mic", scenario: "setup", apply: `mock.s.mics = []; mock.s.micId = "";` },
  {
    name: "setup-error", scenario: "setup", apply: `
      mock.s.setup = { busy: false, error: true, message: "Could not start the installer: The operation was canceled by the user. (0x800704C7) Please try again." };`,
  },
  {
    name: "setup-busy", scenario: "setup", apply: `
      mock.s.setup = { busy: true, error: false, message: "Allow the Windows prompt. Sound pauses for a moment…" };`,
  },
  {
    name: "live-error", scenario: "live", apply: `
      mock.s.live = { on: true, phase: "error", message: "Capture of Spotify failed: the audio device was removed (0x88890004). Retrying…" };`,
  },
  { name: "mic-muted", scenario: "live", apply: `mock.s.micMutedInWindows = true; mock.s.injector.micInUse = false;` },
  { name: "mic-unplugged", scenario: "ready", apply: `mock.s.micId = "unplugged-mic";` },
  { name: "menu-open", scenario: "ready", after: `document.getElementById("more").click();` },
  {
    name: "menu-setup", scenario: "setup", after: `document.getElementById("more").click();
      if ([...document.querySelectorAll("#menu [data-installed]")].some((el) => !el.hidden))
        console.error("repair/remove are offered before Somike is set up");`,
  },
  {
    name: "toast", scenario: "ready",
    after: `toast("Connected to Corsair HS80 RGB USB Gaming Headset. Pick an app and go live. This is a deliberately long message.", "info");`,
  },
];

// Runs in the page: returns a list of layout problems.
const checkLayout = `(() => {
  const issues = [];
  const vw = innerWidth, vh = innerHeight;
  const describe = (el) => el.id ? "#" + el.id : el.tagName.toLowerCase() + (el.className && typeof el.className === "string" ? "." + el.className.split(" ")[0] : "");
  const visible = (el) => !el.closest("[hidden]") && el.getClientRects().length > 0;
  if (document.documentElement.scrollWidth > vw + 1) issues.push("page scrolls horizontally (" + document.documentElement.scrollWidth + " > " + vw + ")");
  for (const el of document.querySelectorAll("#app *")) {
    if (!visible(el)) continue;
    const r = el.getBoundingClientRect();
    if (r.width === 0 || r.height === 0) continue;
    if (r.right > vw + 1 || r.left < -1) issues.push(describe(el) + " sticks out horizontally (" + Math.round(r.left) + ".." + Math.round(r.right) + " of " + vw + ")");
  }
  for (const el of document.querySelectorAll(".tile-name, #footer-text, .row-label small, .row-label span, .select, #live-sub, #banner-text, .tile-sub, #setup-msg, .toast, .brand-name")) {
    if (!visible(el)) continue;
    const cs = getComputedStyle(el);
    if (el.scrollWidth > el.clientWidth + 1 && cs.textOverflow !== "ellipsis" && cs.overflowX !== "visible") issues.push(describe(el) + " text is cut off without an ellipsis");
  }
  for (const sel of ["#toast", "#menu"]) {
    const el = document.querySelector(sel);
    if (el && visible(el)) {
      const r = el.getBoundingClientRect();
      if (r.top < 0 || r.bottom > vh + 1) issues.push(sel + " is outside the window vertically");
    }
  }
  for (const id of ["main", "setup"]) {
    const el = document.getElementById(id);
    if (el && visible(el) && el.scrollHeight > el.clientHeight + 1 && getComputedStyle(el).overflowY === "visible") issues.push("#" + id + " content is cut off (no scrolling)");
  }
  const footer = document.querySelector(".footer").getBoundingClientRect();
  if (footer.bottom > vh + 1) issues.push("footer is below the window edge");
  return issues;
})()`;

function findEdge() {
  const candidates = [
    process.env.SOMIKE_EDGE,
    "C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe",
    "C:\\Program Files\\Microsoft\\Edge\\Application\\msedge.exe",
  ].filter(Boolean);
  const found = candidates.find((p) => existsSync(p));
  if (!found) throw new Error("Microsoft Edge not found; set SOMIKE_EDGE");
  return found;
}

async function connect() {
  for (let attempt = 0; attempt < 100; attempt++) {
    try {
      const targets = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
      const page = targets.find((t) => t.type === "page");
      if (page) return page.webSocketDebuggerUrl;
    } catch {
      // not up yet
    }
    await new Promise((r) => setTimeout(r, 100));
  }
  throw new Error("headless Edge did not start");
}

const profile = mkdtempSync(join(tmpdir(), "somike-edge-"));
const edge = spawn(findEdge(), [
  "--headless=new", `--remote-debugging-port=${port}`, `--user-data-dir=${profile}`,
  "--no-first-run", "--no-default-browser-check", "--hide-scrollbars", "about:blank",
], { stdio: "ignore" });

const ws = new WebSocket(await connect());
await new Promise((resolve, reject) => { ws.onopen = resolve; ws.onerror = reject; });
let nextId = 0;
const pending = new Map();
const listeners = [];
ws.onmessage = (event) => {
  const message = JSON.parse(event.data);
  if (message.id && pending.has(message.id)) {
    const done = pending.get(message.id);
    pending.delete(message.id);
    done(message);
  } else if (message.method) {
    for (const listener of [...listeners]) listener(message);
  }
};
const call = (method, params = {}) => new Promise((resolve, reject) => {
  const id = ++nextId;
  pending.set(id, (m) => (m.error ? reject(new Error(`${method}: ${m.error.message}`)) : resolve(m.result)));
  ws.send(JSON.stringify({ id, method, params }));
});
const evaluate = async (expression) => {
  const r = await call("Runtime.evaluate", { expression, returnByValue: true, awaitPromise: true });
  if (r.exceptionDetails) throw new Error(r.exceptionDetails.exception?.description || r.exceptionDetails.text);
  return r.result.value;
};

let consoleMessages = [];
listeners.push((m) => {
  if (m.method === "Runtime.consoleAPICalled" && ["error", "warning", "assert"].includes(m.params.type)) {
    consoleMessages.push(`${m.params.type}: ${m.params.args.map((a) => a.value ?? a.description).join(" ")}`);
  } else if (m.method === "Runtime.exceptionThrown") {
    consoleMessages.push(`exception: ${m.params.exceptionDetails.exception?.description || m.params.exceptionDetails.text}`);
  } else if (m.method === "Log.entryAdded" && ["error", "warning"].includes(m.params.entry.level)) {
    consoleMessages.push(`${m.params.entry.level}: ${m.params.entry.text}`);
  }
});

await call("Page.enable");
await call("Runtime.enable");
await call("Log.enable");
mkdirSync(outDir, { recursive: true });

const report = [];
let failures = 0;
try {
  for (const size of sizes) {
    await call("Emulation.setDeviceMetricsOverride", {
      width: size.width, height: size.height, deviceScaleFactor: size.scale, mobile: false,
    });
    for (const c of cases) {
      consoleMessages = [];
      const loaded = new Promise((resolve) => listeners.push(function onLoad(m) {
        if (m.method === "Page.loadEventFired") { listeners.splice(listeners.indexOf(onLoad), 1); resolve(); }
      }));
      await call("Page.navigate", { url: `${base}?scenario=${c.scenario}&still` });
      await loaded;
      if (c.apply) await evaluate(`${c.apply}; mock.push(); true`);
      if (c.after) await evaluate(`${c.after}; true`);
      await new Promise((r) => setTimeout(r, 150));
      const issues = await evaluate(checkLayout);
      const shot = await call("Page.captureScreenshot", { format: "png" });
      const file = `${c.name}-${size.name}.png`;
      writeFileSync(join(outDir, file), Buffer.from(shot.data, "base64"));
      const entry = { case: c.name, size: size.name, viewport: `${size.width}x${size.height}@${size.scale}`, issues, console: consoleMessages, screenshot: file };
      report.push(entry);
      if (issues.length || consoleMessages.length) {
        failures++;
        console.log(`FAIL ${c.name} @ ${entry.viewport}`);
        for (const line of [...issues, ...consoleMessages]) console.log(`     ${line}`);
      }
    }
  }
} finally {
  writeFileSync(join(outDir, "report.json"), JSON.stringify(report, null, 2));
  ws.close();
  edge.kill();
  await new Promise((r) => setTimeout(r, 500));
  try { rmSync(profile, { recursive: true, force: true }); } catch { /* Edge may still hold files */ }
}
console.log(`${report.length} cases, ${failures} with issues. Screenshots and report: ${outDir}`);
process.exitCode = failures ? 1 : 0;
