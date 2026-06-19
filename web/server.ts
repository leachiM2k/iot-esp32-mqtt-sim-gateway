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
const ALLOWED_ACTIONS = new Set(["reboot", "call", "accept", "hangup", "sms", "gps"]);
const MAC_RE = /^[0-9A-Fa-f]{12}$/;
const NUMBER_RE = /^\+?[0-9 ]{3,20}$/;

// --- State ------------------------------------------------------------------
const browsers = new Set<WebSocket>();
const macs = new Set<string>();
let brokerConnected = false;

function broadcast(obj: unknown) {
  const s = JSON.stringify(obj);
  for (const ws of browsers) {
    if (ws.readyState === WebSocket.OPEN) ws.send(s);
  }
}

// Record a board MAC and notify browsers. Boards are discovered from their
// retained status messages, which the broker replays on every (re)subscribe —
// so the picker repopulates by itself after a bridge restart, no persistence.
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
    /* plain text (e.g. the /info topic) */
  }
  console.log(`[evt] ${topic} ${raw.slice(0, 160)}`);
  broadcast({ type: "event", topic, mac, sub, payload, raw, ts: Date.now() });
});

// --- Command handling -------------------------------------------------------
function publishCommand(mac: string, cmd: Record<string, unknown>): boolean {
  const topic = `${COMMANDS_BASE}/${mac}`;
  client.publish(topic, JSON.stringify(cmd), { qos: 1 }, (err) => {
    if (err) console.error("[mqtt] publish error:", err?.message);
  });
  console.log(`[cmd] -> ${topic} ${JSON.stringify(cmd)}`);
  return true;
}

interface CommandMsg {
  type: "command";
  action?: string;
  mac?: string;
  number?: string;
  message?: string;
}

function handleCommand(msg: CommandMsg): { ok: boolean; action?: string; error?: string } {
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
  }

  publishCommand(mac, cmd);
  return { ok: true, action };
}

// --- Static file serving + WebSocket endpoint ------------------------------
const PUBLIC_DIR = new URL("./public/", import.meta.url);
const CONTENT_TYPES: Record<string, string> = {
  ".html": "text/html; charset=utf-8",
  ".js": "text/javascript; charset=utf-8",
  ".css": "text/css; charset=utf-8",
};

async function serveStatic(pathname: string): Promise<Response> {
  const p = pathname === "/" ? "/index.html" : pathname;
  if (p.includes("..")) return new Response("forbidden", { status: 403 });
  try {
    const fileUrl = new URL("." + p, PUBLIC_DIR);
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
    const { socket, response } = Deno.upgradeWebSocket(req);
    socket.onopen = () => {
      browsers.add(socket);
      socket.send(JSON.stringify({ type: "broker", connected: brokerConnected }));
      socket.send(JSON.stringify({ type: "macs", macs: [...macs] }));
    };
    socket.onmessage = (ev) => {
      let msg: CommandMsg;
      try {
        msg = JSON.parse(ev.data as string);
      } catch {
        return;
      }
      if (msg.type === "command") {
        socket.send(JSON.stringify({ type: "ack", ...handleCommand(msg) }));
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
