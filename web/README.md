# Local MQTT console (web bridge)

A small local web UI to talk to the ESP32 A7670 gateway over MQTT, with a Deno
bridge so that **credentials never reach the browser** and the broker needs no
WebSocket listener.

```
Browser ──WS(JSON)──> Deno bridge (127.0.0.1) ──TCP MQTT──> broker:1883
        (no creds)      (holds creds, command whitelist)
```

The bridge subscribes to `esp32/events/#`, forwards events to the browser, and
publishes only a fixed whitelist of commands (`reboot`, `call`, `accept`,
`hangup`, `sms`) — it builds the topic/payload itself, so the browser cannot
publish arbitrary MQTT messages.

## Setup

Requires [Deno](https://deno.com/) 2.x.

```bash
cd web
cp .env.example .env     # then fill in the credentials
deno task start
```

Open <http://127.0.0.1:8787>.

## Credentials (`.env`, git-ignored)

| Variable | Purpose |
|----------|---------|
| `MQTT_URL` | Broker TCP URL (e.g. `mqtt://dev.rotmanov.de:1883`) |
| `MQTT_USERNAME` / `MQTT_PASSWORD` | Receives events (a read-only user is enough) |
| `MQTT_CMD_USERNAME` / `MQTT_CMD_PASSWORD` | **Required to send commands** — needs publish rights on `esp32/commands/<MAC>`. If omitted, commands go through the events user and won't reach the board if that user is read-only. |
| `PORT` | Local web server port (default 8787) |
| `EVENTS_BASE` / `COMMANDS_BASE` | Topic bases (match the firmware's `constants.h`) |

## Features

The board MAC is auto-discovered from incoming events and selectable in the header.

**Receive:** device/modem status (`/info`), call status (`/callstatus`), incoming
caller number (`/checkresult`), received SMS list (`/sms`), full event log.

**Send:** answer (`accept`) / reject or hang up (`hangup`), start an outgoing call
(`call`), send SMS (`sms`), reboot the board (`reboot`).

### Not available / caveats
- **USSD**: the firmware has `sendUSSD()` but it is not wired to an MQTT command,
  so it cannot be triggered.
- **Reject = hang up**: there is no separate reject; both send `hangup`
  (`AT+CHUP`), which ends an incoming or active call.
- **Outgoing SMS encoding**: sent as GSM-7, so umlauts/emoji in outgoing messages
  may be mangled (incoming SMS are decoded correctly via UCS2).

The bridge binds to `127.0.0.1` only (not reachable from the network).
