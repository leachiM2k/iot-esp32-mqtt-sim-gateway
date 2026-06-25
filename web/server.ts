/**
 * Local MQTT <-> WebSocket bridge for the ESP32 A7670 SMS/call gateway.
 *
 * Why this exists:
 *  - Browsers can't speak raw MQTT/TCP, and the broker has no WebSocket
 *    listener. This server connects to the broker over TCP and exposes a
 *    plain WebSocket (same origin as the served page) to the browser.
 *  - The MQTT credentials live here (server side), never in the browser.
 *  - The browser may only trigger a fixed command whitelist; the server
 *    builds the topic/payload itself, so the browser can't publish arbitrary
 *    MQTT messages.
 *
 * Run:  deno task start         (reads ./.env)
 * Then open http://127.0.0.1:8787
 */
import mqtt from "npm:mqtt@5.10.1";

// --- Config (from environment / .env) --------------------------------------
const PORT = Number(Deno.env.get("PORT") ?? "8787");
const MQTT_URL = Deno.env.get("MQTT_URL") ?? "mqtt://dev.rotmanov.de:1883";
const EVENTS_BASE = Deno.env.get("EVENTS_BASE") ?? "esp32/events";
const COMMANDS_BASE = Deno.env.get("COMMANDS_BASE") ?? "esp32/commands";

// One user for both receiving events and sending commands; it needs publish
// rights on COMMANDS_BASE/<MAC>.
const MQTT_USER = Deno.env.get("MQTT_USERNAME");
const MQTT_PASS = Deno.env.get("MQTT_PASSWORD");

// --- Command whitelist (mirrors the firmware's getAction()) -----------------
const ALLOWED_ACTIONS = new Set(["reboot", "call", "accept", "hangup", "sms", "gps", "gpsoff", "volte"]);
const MAC_RE = /^[0-9A-Fa-f]{12}$/;
const NUMBER_RE = /^\+?[0-9 ]{3,20}$/;

// Origins allowed to open the WebSocket. Browsers always send an Origin header
// on WS upgrades; a malicious site opened in another tab can otherwise open
// ws://127.0.0.1:PORT/ws and fire commands (call/sms/reboot). Reject anything
// not coming from this server. localhost is accepted because the user may open
// the page via either hostname.
const ALLOWED_ORIGINS = new Set([
  `http://127.0.0.1:${PORT}`,
  `http://localhost:${PORT}`,
]);

// --- State ------------------------------------------------------------------
const browsers = new Set<WebSocket>();
const macs = new Set<string>();
let brokerConnected = false;

// Cache of the latest event per <mac>/<sub>. The broker replays retained
// messages to the bridge on subscribe, but only at bridge connect time —
// before any browser is connected. New browsers would miss them. We cache
// every event we see and replay the latest per topic on browser connect so
// the UI shows the retained state immediately.
interface CachedEvent { topic: string; mac: string; sub: string; payload: unknown; raw: string; ts: number; }
const lastEvent = new Map<string, CachedEvent>();

function broadcast(obj: unknown) {
  const s = JSON.stringify(obj);
  for (const ws of browsers) {
    if (ws.readyState === WebSocket.OPEN) ws.send(s);
  }
}

// Record a board MAC and notify browsers. Boards are discovered from their
// (retained) status messages; the bridge caches every event and replays the
// latest per topic to new browsers, so the picker and status repopulate by
// themselves after a bridge restart — no persistence needed.
function rememberMac(mac: string): boolean {
  if (!MAC_RE.test(mac) || macs.has(mac)) return false;
  macs.add(mac);
  broadcast({ type: "macs", macs: [...macs] });
  return true;
}

// --- MQTT connection (events + commands) -----------------------------------
const client = mqtt.connect(MQTT_URL, {
  username: MQTT_USER,
  password: MQTT_PASS,
  reconnectPeriod: 3000,
  clientId: "espbridge-" + Math.random().toString(16).slice(2, 10),
});

client.on("connect", () => {
  brokerConnected = true;
  console.log(`[mqtt] connected to ${MQTT_URL} as ${MQTT_USER ?? "(anonymous)"}`);
  client.subscribe(`${EVENTS_BASE}/#`, { qos: 0 }, (err) => {
    if (err) console.error("[mqtt] subscribe error:", err.message);
    else console.log(`[mqtt] subscribed to ${EVENTS_BASE}/#`);
  });
  broadcast({ type: "broker", connected: true });
});

client.on("reconnect", () => console.log("[mqtt] reconnecting…"));
client.on("close", () => {
  if (brokerConnected) {
    brokerConnected = false;
    broadcast({ type: "broker", connected: false });
  }
});
client.on("error", (e: Error) => {
  console.error("[mqtt] error:", e.message);
  broadcast({ type: "broker", connected: false, error: e.message });
});

client.on("message", (topic: string, payloadBuf: Uint8Array) => {
  const raw = new TextDecoder().decode(payloadBuf);
  // topic = <EVENTS_BASE>/<MAC>/<sub>
  const parts = topic.split("/");
  if (parts.length < 4) return;
  const mac = parts[2];
  const sub = parts.slice(3).join("/");

  rememberMac(mac);

  let payload: unknown = raw;
  try {
    payload = JSON.parse(raw);
  } catch {
    /* plain text (some topics may be non-JSON) */
  }
  console.log(`[evt] ${topic} ${raw.slice(0, 160)}`);
  const evt = { type: "event", topic, mac, sub, payload, raw, ts: Date.now() } as const;
  lastEvent.set(`${mac}/${sub}`, evt);
  broadcast(evt);
});

// --- Command handling -------------------------------------------------------
// Returns a Promise that resolves once the broker acknowledged the publish
// (qos:1 PUBACK), so the browser ack reflects the actual outcome rather than
// "we called publish()".
function publishCommand(mac: string, cmd: Record<string, unknown>): Promise<{ ok: boolean; error?: string }> {
  const topic = `${COMMANDS_BASE}/${mac}`;
  const payload = JSON.stringify(cmd);
  return new Promise((resolve) => {
    client.publish(topic, payload, { qos: 1 }, (err) => {
      if (err) {
        console.error("[mqtt] publish error:", err?.message);
        resolve({ ok: false, error: err?.message ?? "publish failed" });
      } else {
        resolve({ ok: true });
      }
    });
    console.log(`[cmd] -> ${topic} ${payload}`);
  });
}

interface CommandMsg {
  type: "command";
  action?: string;
  mac?: string;
  number?: string;
  message?: string;
  enable?: boolean;
}

async function handleCommand(msg: CommandMsg): Promise<{ ok: boolean; action?: string; error?: string }> {
  const action = String(msg.action ?? "");
  if (!ALLOWED_ACTIONS.has(action)) return { ok: false, action, error: `action '${action}' not allowed` };

  const mac = String(msg.mac ?? "");
  if (!MAC_RE.test(mac)) return { ok: false, action, error: "no valid board selected (MAC)" };

  const cmd: Record<string, unknown> = { action };

  if (action === "call") {
    const number = String(msg.number ?? "").trim();
    if (!NUMBER_RE.test(number)) return { ok: false, action, error: "invalid phone number" };
    cmd.number = number;
  } else if (action === "sms") {
    const number = String(msg.number ?? "").trim();
    const message = String(msg.message ?? "");
    if (!NUMBER_RE.test(number)) return { ok: false, action, error: "invalid phone number" };
    if (message.length === 0) return { ok: false, action, error: "empty message" };
    if (message.length > 600) return { ok: false, action, error: "message too long" };
    cmd.number = number;
    cmd.message = message;
  } else if (action === "volte") {
    if (typeof msg.enable !== "boolean") return { ok: false, action, error: "volte requires boolean 'enable'" };
    cmd.enable = msg.enable;
  }

  const pub = await publishCommand(mac, cmd);
  return pub.ok ? { ok: true, action } : { ok: false, action, error: pub.error };
}

// --- Static file serving + WebSocket endpoint ------------------------------
const PUBLIC_DIR = new URL("./public/", import.meta.url);
const CONTENT_TYPES: Record<string, string> = {
  ".html": "text/html; charset=utf-8",
  ".js": "text/javascript; charset=utf-8",
  ".css": "text/css; charset=utf-8",
  ".woff2": "font/woff2",
  ".dac": "application/octet-stream",
};

async function serveStatic(pathname: string): Promise<Response> {
  const p = pathname === "/" ? "/index.html" : pathname;
  // Resolve against the public dir and refuse anything that escapes it.
  // `new URL` collapses ".." segments, so we then check the result still lives
  // under PUBLIC_DIR.
  const fileUrl = new URL("." + p, PUBLIC_DIR);
  if (!fileUrl.href.startsWith(PUBLIC_DIR.href)) return new Response("forbidden", { status: 403 });
  try {
    const data = await Deno.readFile(fileUrl);
    const ext = p.slice(p.lastIndexOf("."));
    return new Response(data, {
      headers: { "content-type": CONTENT_TYPES[ext] ?? "application/octet-stream" },
    });
  } catch {
    return new Response("not found", { status: 404 });
  }
}

Deno.serve({ port: PORT, hostname: "127.0.0.1" }, (req) => {
  const url = new URL(req.url);

  if (url.pathname === "/ws") {
    if (req.headers.get("upgrade") !== "websocket") {
      return new Response("expected websocket", { status: 426 });
    }
    const origin = req.headers.get("origin");
    if (!origin || !ALLOWED_ORIGINS.has(origin)) {
      return new Response("forbidden origin", { status: 403 });
    }
    const { socket, response } = Deno.upgradeWebSocket(req);
    socket.onopen = () => {
      browsers.add(socket);
      socket.send(JSON.stringify({ type: "broker", connected: brokerConnected }));
      socket.send(JSON.stringify({ type: "macs", macs: [...macs] }));
      // Replay the latest retained/cached event per topic so new browsers
      // see the current board state immediately (info, callstatus, volte, …).
      for (const evt of lastEvent.values()) socket.send(JSON.stringify(evt));
    };
    socket.onmessage = (ev) => {
      let msg: CommandMsg;
      try {
        msg = JSON.parse(ev.data as string);
      } catch {
        return;
      }
      if (msg.type === "command") {
        handleCommand(msg).then((result) => socket.send(JSON.stringify({ type: "ack", ...result })));
      }
    };
    socket.onclose = () => browsers.delete(socket);
    socket.onerror = () => browsers.delete(socket);
    return response;
  }

  return serveStatic(url.pathname);
});

console.log(`[http] UI on http://127.0.0.1:${PORT}  (broker: ${MQTT_URL})`);
if (!MQTT_USER) console.warn("[warn] MQTT_USERNAME not set — is .env loaded? Run via 'deno task start'.");
