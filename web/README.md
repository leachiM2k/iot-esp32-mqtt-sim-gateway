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
`hangup`, `sms`, `gps`, `gpsoff`) — it builds the topic/payload itself, so the
browser cannot publish arbitrary MQTT messages.

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
| `MQTT_USERNAME` / `MQTT_PASSWORD` | MQTT user for the bridge — receives events and sends commands, so it needs publish rights on `esp32/commands/<MAC>` |
| `PORT` | Local web server port (default 8787) |
| `EVENTS_BASE` / `COMMANDS_BASE` | Topic bases (match the firmware's `constants.h`) |

## Features

Boards are auto-discovered: the firmware publishes its `/info` and `/callstatus`
as **retained** messages, which the broker replays to every new subscriber. So
the board picker in the header repopulates on its own after a bridge restart,
without the board having to republish. (A 1-day expiry on those retained values
needs MQTT 5 and is pending the firmware framework update; for now they live
until overwritten on the next boot / status change.)

**Receive:** device/modem status (`/info`), call status (`/callstatus`), incoming
caller number (`/checkresult`), received SMS list (`/sms`), full event log.

**Send:** answer (`accept`) / reject or hang up (`hangup`), start an outgoing call
(`call`), send SMS (`sms`), query the GPS position (`gps`, shown on an
OpenStreetMap map), power GNSS down (`gpsoff`), reboot the board (`reboot`).

GPS is handled asynchronously on the board: the `gps` command just requests a
fix, the board acquires it in the background and publishes `/gps` when ready
(a cold start can take up to ~90 s) — it does not block the device. GNSS stays
on after a request for fast follow-up fixes; use `gpsoff` to save power.

### Not available / caveats
- **USSD**: the firmware has `sendUSSD()` but it is not wired to an MQTT command,
  so it cannot be triggered.
- **Reject = hang up**: there is no separate reject; both send `hangup`
  (`AT+CHUP`), which ends an incoming or active call.
- **Outgoing SMS encoding**: sent as GSM-7, so umlauts/emoji in outgoing messages
  may be mangled (incoming SMS are decoded correctly via UCS2).

The bridge binds to `127.0.0.1` only (not reachable from the network).
