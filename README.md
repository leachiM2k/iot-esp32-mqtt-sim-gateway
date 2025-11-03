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

**Tested on**: LilyGo T-Call A7670E

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

#### Send USSD
```json
{
  "action": "ussd",
  "code": "*100#"
}
```

### Event Topic (`esp32/events/<MAC>`)

The device publishes events with ISO8601 timestamps:

#### Incoming Call
```json
{
  "timestamp": "2025-11-03T10:15:30Z",
  "mac": "AC1518B62E50",
  "event": "call",
  "number": "+491234567890"
}
```

#### Call Status Update
```json
{
  "timestamp": "2025-11-03T10:15:35Z",
  "mac": "AC1518B62E50",
  "event": "call_update",
  "status": "ESTABLISHED"
}
```

#### SMS Received
```json
{
  "timestamp": "2025-11-03T10:20:00Z",
  "mac": "AC1518B62E50",
  "event": "sms",
  "message": "SMS content here"
}
```

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
