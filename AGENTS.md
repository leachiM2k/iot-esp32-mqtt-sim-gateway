# AGENTS.md

Schneller Einstieg für Coding-Agenten in dieses Projekt. Ergänzt die `README.md`
(die sich an Endnutzer richtet); hier stehen Architektur, Konventionen und
Fallstricke, die man vor dem Editieren kennen sollte.

## Was ist das?

ESP32-Firmware für das Board **LilyGo T-Call A7670E** (A7670 LTE-Modem). Das Gerät
ist ein **MQTT-gesteuertes SMS-/Anruf-Gateway**: Es nimmt JSON-Kommandos über MQTT
entgegen (SMS senden, anrufen, auflegen, annehmen, reboot) und publiziert Events
(eingehende Anrufe/SMS, Anrufstatus) zurück nach MQTT.

C++ / Arduino-Framework, gebaut mit **PlatformIO**.

## Architektur

Drei Klassen-Module plus `main.cpp` als Orchestrator. Alle Quellen liegen in `src/`.

| Datei | Verantwortung |
|-------|---------------|
| `src/main.cpp` | Setup + Loop, JSON-Parsing der Kommandos, Event-Publishing, MAC/Zeit-Helfer |
| `src/SimCommunication.{h,cpp}` | Modem-Lifecycle, AT-Kommandos, Anrufe, SMS senden/lesen, USSD, Netz-Registrierung |
| `src/MqttClient.{h,cpp}` | Wrapper um ESP-IDF `esp_mqtt_client`, Connect/Reconnect, Sub/Pub, Event-Handler |
| `src/WifiConnection.{h,cpp}` | WiFi via WiFiManager (Captive Portal) + mDNS |
| `src/constants.h` | **Sämtliche Konfiguration**: Pins, APN, NTP, MQTT-Zugangsdaten |

### Datenfluss
1. `setup()`: WiFi → NTP-Zeit → Modem-Init → MQTT-Connect → Subscribe auf Command-Topic.
2. `loop()`: MQTT-Reconnect prüfen → `simCommunication.check()` pollt das Modem auf
   eingehende SMS/Anrufe → bei Event `sendSimComEvent()` / `publishCallStatus()`.
3. Eingehende MQTT-Daten landen über den C-Callback `onDataReceived()` in `main.cpp`,
   werden mit ArduinoJson geparst und auf `Action`-Enum gemappt.

### Wichtig: zwei getrennte Transportwege
- **MQTT läuft über WiFi** (ESP-IDF `esp_mqtt_client`), **nicht** über das Modem.
- **SMS/Anrufe laufen über das A7670-Modem** (TinyGSM + rohe AT-Kommandos auf `SerialAT`/`Serial1`).
- Beide `MqttClient` und `SimCommunication` haben je eine **eigene** `StreamDebugger`/`TinyGsm`-Instanz;
  in `main.cpp` existieren zusätzlich noch globale `debugger`/`modem`-Instanzen (Stand jetzt ungenutzt vom Hauptpfad — Vorsicht beim Aufräumen).

## Build & Flash

PlatformIO (CLI). Default-Env: `T-Call-A7670X-V1-1`.

```bash
pio run                    # Build
pio run --target upload    # Flashen
pio device monitor         # Serielle Konsole, 115200 Baud
pio run -t clean           # Clean
```

- Plattform gepinnt: `espressif32@6.12.0`. Nicht ungefragt upgraden.
- Build-Flag `-DLILYGO_T_CALL_A7670_V1_1` selektiert die Board-Variante.
- Lib-Deps (`platformio.ini`): StreamDebugger, WiFiManager, ArduinoJson.
- **TinyGSM ist vendored** in `lib/TinyGSM/` (nicht aus dem Registry). Nicht editieren,
  außer ein Patch wird ausdrücklich gewünscht.
- esptool braucht ggf. das Python-Paket `intelhex` (siehe README).

Es gibt **keine echten Tests** und kein CI; `test/` enthält nur das PlatformIO-Boilerplate-README.

## Konventionen

- Pin-Belegung und alle Magic-Strings gehören in `src/constants.h`, nicht inline.
- Rohe AT-Kommandos via `modem.sendAT(...)` + `modem.waitResponse(...)`; Antworten
  werden mit `indexOf`/`substring` von Hand geparst (kein Parser-Framework).
- Logging durchgängig über `Serial.print*` (115200 Baud). Kein Log-Level-System.
- JSON immer mit ArduinoJson v7 (`JsonDocument`, kein `StaticJsonDocument` mehr).
- Topic-Format: `<basis>/<MAC>[/<subkanal>]`, MAC ist die WiFi-STA-MAC in Hex-Großbuchstaben.

## ⚠️ Bekannte Fallstricke / README-Diskrepanzen

Die README ist teils ungenau — der **Code ist die Wahrheit**:

- **Event-JSON-Feld heißt `time`, nicht `timestamp`** (siehe `sendSimComEvent` in `main.cpp`).
- **Event-Topics haben Subkanäle**, die die README nicht nennt:
  `…/<MAC>/sms`, `…/<MAC>/checkresult`, `…/<MAC>/callstatus`, `…/<MAC>/info`.
- **USSD ist nicht per MQTT erreichbar**: `SimCommunication::sendUSSD()` existiert,
  ist aber im `Action`-Enum / `getAction()` **nicht verdrahtet**. Die README listet es trotzdem.
- **`reboot` ist als Action implementiert**, fehlt aber in der README-Kommandoliste.
- **SMS-Erkennung ist Polling-basiert** über den RING-Pin (kein CNMI-Push); `check()`
  liest bei RING erst alle SMS, sonst prüft es auf Anruf. Gelesene SMS werden vom
  Modem **gelöscht** (`+CMGD`).
- **Klartext-Secrets**: MQTT-User/Passwort und APN stehen unverschlüsselt in
  `src/constants.h` (im Git getrackt). Beim Umgang mit dieser Datei beachten —
  nicht versehentlich in Logs/Commits/PRs ausgeben.

## Hardware

**Board**: LilyGo T-Call A7670E, Revision **V1.1** (ESP32-WROVER-E, 4 MB Flash /
8 MB PSRAM). Bestätigt über die Pins: sie matchen exakt das Macro
`LILYGO_T_CALL_A7670_V1_1` aus LilyGos
[LilyGo-Modem-Series](https://github.com/Xinyuan-LilyGO/LilyGo-Modem-Series)
(V1.0 und das Standalone-T-A7670 haben andere Pins / `RESET_LEVEL HIGH`).

**Modem**: A7670**E** (Europa-Bänder, Voice + SMS), passend zu sipgate/DE. Konkret
`A7670E-FASE` oder `-LASE` (Unterschied nur GPS, das die Firmware nicht nutzt —
`MODEM_GPS_ENABLE_GPIO (-1)`). Die exakte Variante steht im Boot-Log
(`Model Name:` aus `getModemName()` / `AT+SIMCOMATI`) und wird beim Start aufs
MQTT-`/info`-Topic publiziert (`Modem ready: <modell>`).

**Pins (aus `constants.h`)**: Modem-UART TX=27 RX=26 · PWRKEY=4 · DTR=32 · RESET=5
(aktiv LOW) · RING=33 · LED/PowerOn=13. Reset-Sequenz und PWRKEY-Toggle stecken in
`SimCommunication::powerUpModem()`. Hinweis: `BOARD_POWERON_PIN` (=LED 13) ist eine
projekteigene Ergänzung — V1.1 hat keine Modem-Power-Control im Upstream.

## Git

Default-Branch `main`. Commits/Push nur auf ausdrücklichen Wunsch; bei Arbeit auf
`main` vorher einen Branch anlegen. **`src/constants.h` enthält lokal echte Secrets
(MQTT-Passwort) und wird bewusst nicht committet** — beim Stagen nie pauschal
`git add -A` o.ä. verwenden, sondern Dateien gezielt stagen.
