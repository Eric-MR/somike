// Drives the *real* Somike window for UI tests and screenshots, through the
// DevTools protocol of its WebView2. Start the app with a debugging port:
//
//   somike.exe --remote-debugging-port 9222
//   node tools/ui-dev/drive.mjs 9222 screenshot out.png
//   node tools/ui-dev/drive.mjs 9222 click "[data-id='spotify.exe']"
//   node tools/ui-dev/drive.mjs 9222 eval "state.live"
//   node tools/ui-dev/drive.mjs 9222 wait "state && state.live.phase === 'live'" 10000
import { writeFileSync } from "node:fs";

const [port = "9222", command, ...args] = process.argv.slice(2);

async function findPage() {
  // Up to 60 s: the first WebView2 start on a fresh machine is slow.
  for (let attempt = 0; attempt < 300; attempt++) {
    try {
      const targets = await (await fetch(`http://127.0.0.1:${port}/json/list`)).json();
      const page = targets.find((t) => t.type === "page" && t.url.startsWith("https://app.somike/"));
      if (page) return page;
    } catch {
      // not up yet
    }
    await new Promise((r) => setTimeout(r, 200));
  }
  throw new Error(`no Somike page on port ${port}`);
}

const page = await findPage();
const ws = new WebSocket(page.webSocketDebuggerUrl);
await new Promise((resolve, reject) => {
  ws.onopen = resolve;
  ws.onerror = reject;
});
let nextId = 0;
const pending = new Map();
ws.onmessage = (event) => {
  const message = JSON.parse(event.data);
  const done = pending.get(message.id);
  if (done) {
    pending.delete(message.id);
    done(message);
  }
};
const call = (method, params = {}) =>
  new Promise((resolve, reject) => {
    const id = ++nextId;
    pending.set(id, (m) => (m.error ? reject(new Error(m.error.message)) : resolve(m.result)));
    ws.send(JSON.stringify({ id, method, params }));
  });
const evaluate = async (expression) => {
  const r = await call("Runtime.evaluate", { expression, returnByValue: true, awaitPromise: true });
  if (r.exceptionDetails) throw new Error(r.exceptionDetails.exception?.description || "evaluation failed");
  return r.result.value;
};

try {
  switch (command) {
    case "screenshot": {
      const r = await call("Page.captureScreenshot", { format: "png" });
      writeFileSync(args[0], Buffer.from(r.data, "base64"));
      console.log(`saved ${args[0]}`);
      break;
    }
    case "click": {
      const found = await evaluate(`(() => { const el = document.querySelector(${JSON.stringify(args[0])}); if (!el) return false; el.click(); return true; })()`);
      if (!found) throw new Error(`no element ${args[0]}`);
      console.log(`clicked ${args[0]}`);
      break;
    }
    case "eval":
      console.log(JSON.stringify(await evaluate(args.join(" ")), null, 1));
      break;
    case "wait": {
      const timeout = Number(args[1] || 10000);
      const start = Date.now();
      while (!(await evaluate(`!!(${args[0]})`))) {
        if (Date.now() - start > timeout) throw new Error(`timed out waiting for: ${args[0]}`);
        await new Promise((r) => setTimeout(r, 200));
      }
      console.log(`ok after ${Date.now() - start} ms`);
      break;
    }
    default:
      throw new Error("commands: screenshot <file> | click <selector> | eval <js> | wait <js> [ms]");
  }
} catch (error) {
  console.error(error.message);
  process.exitCode = 1;
} finally {
  ws.close();
}
