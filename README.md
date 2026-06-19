# IoT ESP32 MQTT SMS Gateway

A feature-rich ESP32-based IoT gateway that combines WiFi, MQTT, GSM connectivity, and SMS/voice call capabilities. Built for the **LilyGo T-Call A7670E** board with the A7670 cellular modem.

## Features

- **WiFi Connectivity**: Easy setup via WiFiManager captive portal
- **MQTT Communication**: Bidirectional MQTT messaging with auto-reconnect
- **GSM/LTE Modem**: Full A7670 modem support via TinyGSM
- **SMS Gateway**: Send and receive SMS messages via MQTT commands
- **Voice Calls**: Make, receive, and manage voice calls remotely
- **USSD Support**: Query network information and balance
- **Event Publishing**: Real-time event notifications (calls, SMS) to MQTT
- **NTP Time Sync**: Automatic time synchronization
- **mDNS Support**: Device discovery on local network

## Hardware

**Board**: LilyGo T-Call A7670E, **hardware revision V1.1** (ESP32-WROVER-E, 4 MB
Flash / 8 MB PSRAM). The board build macro is `LILYGO_T_CALL_A7670_V1_1`
(`platformio.ini`); the V1.1 pinout matches LilyGo's
[official definitions](https://github.com/Xinyuan-LilyGO/LilyGo-Modem-Series).

### Modem variant

The board ships with one of several A7670 modem variants. This project targets the
**A7670E** (Europe bands: LTE-FDD B1/B3/B5/B7/B8/B20, GSM 900/1800) with voice +
SMS support. The unit in use is confirmed (via boot log) as **A7670E-FASE**
(firmware revision `A7670M7_V1.11.1`) — the variant with GPS, voice and SMS. GPS is
not used by the firmware.

To find your exact variant, watch the serial console at boot for the `Model Name:`
line (from `getModemName()`) or send `AT+SIMCOMATI`. The model is also published to
MQTT on boot — see the `/info` event below.

### Pin Configuration

- Modem UART: TX=27, RX=26
- Modem Control: PWR=4, DTR=32, RESET=5, RING=33
- LED: GPIO 13

## Requirements

### Software
- PlatformIO (with Espressif32 platform v6.12.0)
- Python package `intelhex` (for esptool):
  ```bash
  ~/.platformio/penv/bin/pip install --upgrade intelhex
  ```

### Libraries
- TinyGSM (included in `lib/`)
- StreamDebugger v1.0.1
- WiFiManager v2.0.17
- ArduinoJson v7.3.0

## Configuration

Edit `src/constants.h` to configure:

```cpp
#define NETWORK_APN     "your-apn"           // GSM APN
#define MQTT_SERVER     "mqtt://your-server" // MQTT broker URL
#define MQTT_USER       "username"
#define MQTT_PASSWORD   "password"
#define MQTT_COMMAND_TOPIC "esp32/commands"
#define MQTT_EVENT_TOPIC   "esp32/events"
```

## Building & Flashing

```bash
# Build
pio run

# Upload
pio run --target upload

# Monitor
pio device monitor
```

## MQTT Interface

### Command Topic (`esp32/commands/<MAC>`)

Send JSON commands to control the device:

#### Send SMS
```json
{
  "action": "sms",
  "number": "+491234567890",
  "message": "Hello from IoT device!"
}
```

#### Make Call
```json
{
  "action": "call",
  "number": "+491234567890"
}
```

#### Hang Up
```json
{
  "action": "hangup"
}
```

#### Accept Call
```json
{
  "action": "accept"
}
```

#### Reboot
```json
{
  "action": "reboot"
}
```

#### Query GPS position
```json
{
  "action": "gps"
}
```
Requests a position fix. Handled **asynchronously** — the command returns
immediately and the board acquires the fix in the background (on the main loop,
so it never blocks the device or the modem UART), then publishes it on the
`/gps` event topic. A cold start may need a moment / a clear view of the sky.
GNSS is kept on after a request so later fixes come faster.

#### Power down GPS
```json
{
  "action": "gpsoff"
}
```
Powers GNSS off to save energy (`AT+CGNSSPWR=0`).

> **Note:** A `sendUSSD()` method exists in the firmware but is currently **not**
> wired to any MQTT command, so there is no `ussd` action yet.

### Event Topics

The device publishes to several sub-topics under `esp32/events/<MAC>`. All event
payloads carry an ISO8601 timestamp in the `time` field and the device `mac`.

#### Incoming Call — `esp32/events/<MAC>/checkresult`
```json
{
  "mac": "AC1518B62E50",
  "time": "2025-11-03T10:15:30Z",
  "event": 1,
  "data": "+491234567890"
}
```

#### Call Status Update — `esp32/events/<MAC>/callstatus`
Published on call-related events and after `call`/`accept`/`hangup` commands.
```json
{
  "mac": "AC1518B62E50",
  "time": "2025-11-03T10:15:35Z",
  "status": "ESTABLISHED"
}
```
`status` is one of `NO_CALL`, `CALLING`, `RINGING`, `ESTABLISHED`, `UNKNOWN`.

#### SMS Received — `esp32/events/<MAC>/sms`
```json
{
  "mac": "AC1518B62E50",
  "time": "2025-11-03T10:20:00Z",
  "event": 3,
  "sender": "+491234567890",
  "message": "SMS content here",
  "data": "+491234567890:SMS content here"
}
```

#### Device Started — `esp32/events/<MAC>/info`
Plain-text status messages (not JSON) published during boot. **Retained**, so a
new subscriber immediately learns the board exists and its last status:
- `Device started at 2025-11-03T10:15:00Z, initializing modem`
- on success: `Modem ready: A7670E-FASE` (includes the detected modem variant)
- on failure: `Modem initialization failed` (device continues in degraded mode)

#### GPS Position — `esp32/events/<MAC>/gps`
Reply to a `gps` command. Coordinates are in decimal degrees. A successful fix
is published **retained** (last known position); a no-fix reply is transient.
```json
{
  "mac": "AC1518B62E50",
  "time": "2026-06-19T22:00:00Z",
  "fix": true,
  "lat": 52.516275,
  "lon": 13.377704,
  "alt": 38.5,
  "satellites": 9
}
```
When there is no fix yet: `{ "mac": "...", "time": "...", "fix": false }`.

> **Note:** The `event` field is the numeric enum value
> (`1` = call, `2` = call update, `3` = SMS).
>
> `/info` and `/callstatus` are published as retained messages; `/sms`,
> `/checkresult` and a no-fix `/gps` are transient.

## First Run

1. **WiFi Setup**: Device creates AP `AutoConnectAP` on first boot
2. Connect to AP and configure WiFi credentials
3. Device connects to WiFi and MQTT broker
4. Subscribe to command topic to control device

## Known Issues

- **MQTT Disconnect after SMS**: Fixed by filtering URC messages (only read lines starting with `+`)
- **esptool.py error**: Requires `intelhex` Python package with Espressif32 v6.12.0+

## Troubleshooting

### MQTT Connection Issues
- Check broker URL, credentials, and network connectivity
- Monitor serial output for error messages (115200 baud)

### Modem Not Responding
- Verify SIM card is inserted and unlocked
- Check APN configuration for your carrier
- Ensure antenna is connected

### Build Errors
```bash
# Clean and rebuild
pio run -t clean
pio run
```

## License

This project uses TinyGSM library (Apache License 2.0). See individual library licenses in `lib/` directory.

## Contributing

Issues and pull requests are welcome!

## Author

Created for IoT gateway applications with ESP32 and cellular connectivity.
