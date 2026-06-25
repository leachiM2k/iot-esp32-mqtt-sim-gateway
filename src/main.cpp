#include <Arduino.h>
#include <constants.h>

#include <WiFi.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>
#include <sys/time.h>
#include "MqttService.h"
#include "WifiConnection.h"
#include "SimCommunication.h"
#include "Connectivity.h"

// Task-watchdog timeout. Must exceed the longest single blocking call in the
// modem init (the SMS DONE wait, ~30 s) while still catching a real hang. The
// bounded init loops feed the watchdog so they don't trip it prematurely.
static const uint32_t WDT_TIMEOUT_S = 60;

void onDataReceived(const char *topic, int topic_len, const char *data, int data_len);

WifiConnection wifiConnection;
SimCommunication simCommunication;
MqttService mqtt;
Connectivity connectivity;

String getCurrentTimeISO8601()
{
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    char timeStr[30];
    strftime(timeStr, sizeof(timeStr), "%FT%TZ", &timeinfo);
    
    return String(timeStr);
}

String getCurrentMacAddress()
{
    uint8_t mac[6];
    char macStr[18] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(macStr, sizeof(macStr), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return String(macStr);
}

void sendSimComEvent(sim_com_check_result &result)
{
    String timeStr = getCurrentTimeISO8601();

    String macStr = getCurrentMacAddress();

    // Generate JSON
    JsonDocument doc;

    doc["mac"] = macStr;
    doc["time"] = timeStr;
    doc["event"] = result.event;
    
    // Special handling for SMS events
    if (result.event == SIM_COM_SMS && result.data.indexOf(":") != -1)
    {
        // Parse sender:message format
        int colonIndex = result.data.indexOf(":");
        String sender = result.data.substring(0, colonIndex);
        String message = result.data.substring(colonIndex + 1);
        
        doc["sender"] = sender;
        doc["message"] = message;
        doc["data"] = result.data; // Keep original format as backup
    }
    else
    {
        doc["data"] = result.data;
    }

    char json[300];
    size_t n = serializeJson(doc, json);

    Serial.println(json);

    char topic[60] = {0};
    if (result.event == SIM_COM_SMS)
    {
        sprintf(topic, "%s/%s/sms", MQTT_EVENT_TOPIC, macStr.c_str());
    }
    else
    {
        sprintf(topic, "%s/%s/checkresult", MQTT_EVENT_TOPIC, macStr.c_str());
    }

    mqtt.publish(topic, json);
}

void publishCallStatus(const char *status)
{
    String timeStr = getCurrentTimeISO8601();

    String macStr = getCurrentMacAddress();

    // Generate JSON
    JsonDocument doc;

    doc["mac"] = macStr;
    doc["time"] = timeStr;
    doc["status"] = status;

    char json[200];
    size_t n = serializeJson(doc, json);

    Serial.println(json);

    char topic[60] = {0};
    sprintf(topic, "%s/%s/callstatus", MQTT_EVENT_TOPIC, macStr.c_str());

    // Retained: the current call state stays available to new subscribers.
    mqtt.publish(topic, json, true);
}

void publishGps(const gps_result &g)
{
    String macStr = getCurrentMacAddress();

    JsonDocument doc;
    doc["mac"] = macStr;
    doc["time"] = getCurrentTimeISO8601();
    doc["fix"] = g.fix;
    if (g.fix)
    {
        // serialized() keeps full decimal precision for the coordinates.
        doc["lat"] = serialized(String(g.lat, 6));
        doc["lon"] = serialized(String(g.lon, 6));
        doc["alt"] = g.altitude; // WGS84 ellipsoidal height (m)
        doc["satellites"] = g.satellites;
        // Horizontal dilution of precision: lower is better (<1 ideal, <5 good).
        // Rough horizontal accuracy ≈ hdop × 5 m.
        doc["hdop"] = serialized(String(g.hdop, 2));
    }

    char json[256];
    serializeJson(doc, json);
    Serial.println(json);

    char topic[60] = {0};
    sprintf(topic, "%s/%s/gps", MQTT_EVENT_TOPIC, macStr.c_str());

    // Retain only a real fix, so the last known position stays available to new
    // subscribers; a "no fix" reply is transient and must not overwrite it.
    mqtt.publish(topic, json, g.fix);
}

void publishVolte(const volte_status &v)
{
    String macStr = getCurrentMacAddress();

    JsonDocument doc;
    doc["mac"] = macStr;
    doc["time"] = getCurrentTimeISO8601();
    doc["enabled"] = v.enabled;
    doc["cevdp"] = v.cevdp;
    doc["ims_available"] = v.imsAvailable;
    doc["ims_registered"] = v.imsRegistered;

    char json[200];
    serializeJson(doc, json);
    Serial.println(json);

    char topic[60] = {0};
    sprintf(topic, "%s/%s/volte", MQTT_EVENT_TOPIC, macStr.c_str());

    // Retained: current VoLTE state stays available to new subscribers.
    mqtt.publish(topic, json, true);
}

// Publish the retained /info topic as JSON: liveness, modem model, current
// transport (wlan/lte), IMEI and radio-network details. Retained so a new
// subscriber immediately sees this board and its state.
void publishInfo(const char *status)
{
    String macStr = getCurrentMacAddress();

    JsonDocument doc;
    doc["mac"] = macStr;
    doc["time"] = getCurrentTimeISO8601();
    doc["status"] = status;                       // "online"
    doc["transport"] = connectivity.activeName(); // "WIFI" / "LTE" / "NONE"

    if (simCommunication.isModemReady())
    {
        doc["modem"] = simCommunication.getModemName();
        doc["imei"] = simCommunication.getImei();

        network_info net = simCommunication.readNetworkInfo();
        JsonObject n = doc["network"].to<JsonObject>();
        n["operator"] = net.oper;
        n["mode"] = net.mode;     // LTE / WCDMA / GSM
        n["signal"] = net.signal; // CSQ 0..31, 99 = unknown
        n["band"] = net.band;
    }
    else
    {
        doc["modem"] = nullptr; // degraded mode: modem never came up
    }

    char json[384];
    serializeJson(doc, json);
    Serial.println(json);

    char topic[60] = {0};
    sprintf(topic, "%s/%s/info", MQTT_EVENT_TOPIC, macStr.c_str());
    mqtt.publish(topic, json, true);
}

// Called after every successful MQTT (re)connect, on the loop task. Re-publishes
// the retained state so a fresh broker session (or a transport switch) is
// immediately consistent again, and overwrites the "offline" Last Will.
void onMqttConnect()
{
    publishInfo("online");
    if (simCommunication.isModemReady())
    {
        publishCallStatus(simCommunication.getCallStatus().c_str());
        publishVolte(simCommunication.readVolteStatus());
    }
}

enum class Action
{
    REBOOT,
    CALL,
    ACCEPT,
    HANGUP,
    SMS,
    GPS,
    GPS_OFF,
    VOLTE,
    UNKNOWN
};

Action getAction(const char *action)
{
    if (strcmp(action, "reboot") == 0)
    {
        return Action::REBOOT;
    }
    else if (strcmp(action, "call") == 0)
    {
        return Action::CALL;
    }
    else if (strcmp(action, "accept") == 0)
    {
        return Action::ACCEPT;
    }
    else if (strcmp(action, "hangup") == 0)
    {
        return Action::HANGUP;
    }
    else if (strcmp(action, "sms") == 0)
    {
        return Action::SMS;
    }
    else if (strcmp(action, "gps") == 0)
    {
        return Action::GPS;
    }
    else if (strcmp(action, "gpsoff") == 0)
    {
        return Action::GPS_OFF;
    }
    else if (strcmp(action, "volte") == 0)
    {
        return Action::VOLTE;
    }
    else
    {
        return Action::UNKNOWN;
    }
}

void onDataReceived(const char *topic, int topic_len, const char *data, int data_len)
{
    // topic and data come straight from the MQTT event and are NOT
    // null-terminated, so they must only ever be printed with an explicit
    // length (%.*s) — never %s / println, which would read past the buffer.
    Serial.printf("Callback - Topic: %.*s, Data: %.*s (%d bytes)\n",
                  topic_len, topic, data_len, data, data_len);

    // Allocate a temporary JsonDocument
    JsonDocument doc;

    // Deserialize the JSON document (length-bounded, so the missing
    // terminator is fine here)
    DeserializationError error = deserializeJson(doc, data, data_len);

    // Test if parsing succeeds
    if (error)
    {
        Serial.print(F("deserializeJson() failed: "));
        Serial.println(error.f_str());
        return;
    }

    // Extract values
    const char *action = doc["action"];
    const char *number;

    // A missing "action" yields a null pointer; guard before strcmp/println
    // so a malformed command can't crash the device.
    if (!action)
    {
        Serial.println("Command missing 'action' field, ignoring");
        return;
    }

    Serial.print("Searching Action enum for:");
    Serial.println(action);
    Action act = getAction(action);
    Serial.print("Action enum:");
    Serial.println((int)act);

    // Reject modem-dependent commands while the modem is not ready (degraded
    // mode), but always allow a remote reboot so the device stays controllable.
    if (act != Action::REBOOT && act != Action::UNKNOWN && !simCommunication.isModemReady())
    {
        Serial.println("Modem not ready, ignoring command");
        return;
    }

    switch (act)
    {
    case Action::REBOOT:
        Serial.println("Rebooting...");
        ESP.restart();
        break;

    case Action::CALL:
        number = doc["number"];
        if (!number)
        {
            Serial.println("call action requires 'number' field");
            break;
        }
        simCommunication.makeCall(number);
        publishCallStatus(simCommunication.getCallStatus().c_str());
        break;

    case Action::ACCEPT:
        simCommunication.acceptCall();
        publishCallStatus(simCommunication.getCallStatus().c_str());
        break;

    case Action::HANGUP:
        simCommunication.hangupCall();
        publishCallStatus(simCommunication.getCallStatus().c_str());
        break;

    case Action::SMS:
        {
            const char *smsNumber = doc["number"];
            const char *smsMessage = doc["message"];
            if (smsNumber && smsMessage) {
                Serial.println("Sending SMS...");
                simCommunication.sendSMS(smsNumber, smsMessage);
                Serial.printf("SMS sent to %s: %s\n", smsNumber, smsMessage);
            } else {
                Serial.println("SMS action requires 'number' and 'message' fields");
            }
        }
        break;

    case Action::GPS:
        // Just request a fix; the loop task acquires it and publishes /gps.
        Serial.println("GPS position requested.");
        simCommunication.requestGps();
        break;

    case Action::GPS_OFF:
        Serial.println("GPS power-down requested.");
        simCommunication.powerDownGps();
        break;

    case Action::VOLTE:
        {
            // {"action":"volte","enable":true|false}
            if (!doc["enable"].is<bool>())
            {
                Serial.println("volte action requires boolean 'enable' field");
                break;
            }
            bool enable = doc["enable"];
            Serial.printf("VoLTE %s requested.\n", enable ? "ON" : "OFF");
            simCommunication.requestVolte(enable);
        }
        break;

    default:
        break;
    }
}

// Resolve the broker hostname using whichever transport is currently active.
// WiFi uses the lwIP resolver; LTE uses the modem's DNS (AT+CDNSGIP), because
// the A7670's inline CIPOPEN DNS is unreliable over the mobile APN.
bool resolveBroker(const char *host, IPAddress &out)
{
    switch (connectivity.active())
    {
    case Transport::WIFI:
        return WiFi.hostByName(host, out) == 1;
    case Transport::LTE:
        return simCommunication.resolveHost(host, out);
    default:
        return false;
    }
}

void setup()
{
    Serial.begin(115200); // Set console baud rate

    configTzTime(TZ_INFO, NTP_SERVER);

    String macStr = getCurrentMacAddress();

    // 1. Arm the task watchdog before the modem init (which contains the
    //    long-running waits). The WiFiManager portal is non-blocking now, so it
    //    can no longer stall setup() past the timeout. Init helpers feed the
    //    watchdog, so a genuine hang triggers a reset instead of a silent freeze.
    esp_task_wdt_init(WDT_TIMEOUT_S, true);
    esp_task_wdt_add(NULL);

    // 2. Modem init first: it is needed for the LTE transport (data context)
    //    AND for calls/SMS. Bounded by timeouts; on failure stay up in a
    //    degraded mode so the device remains reachable (e.g. for a remote reboot
    //    once a transport comes up).
    bool modemOk = simCommunication.init();
    if (!modemOk)
    {
        Serial.println("Modem initialization failed - running in degraded mode.");
    }
    else
    {
        int smsCount = simCommunication.getSMSCount();
        Serial.print("Number of SMS on SIM card: ");
        Serial.println(smsCount);

        // Seed the clock from the modem's network time so the first retained
        // publishes carry a real timestamp even on an LTE-only boot, where NTP
        // (which runs over lwIP/WiFi only) cannot reach a time server.
        time_t e = simCommunication.getNetworkEpochUTC();
        if (e > 0)
        {
            struct timeval tv = {e, 0};
            settimeofday(&tv, nullptr);
            Serial.printf("Clock seeded from modem: %ld\n", (long)e);
        }
    }

    // 3. WiFi bring-up (non-blocking): connects to the saved AP if present,
    //    otherwise leaves a config portal open while the device runs over LTE.
    wifiConnection.init();

    // 4. MQTT + connectivity. The Last Will (retained "offline" on /info) lets
    //    the broker reflect an ungraceful drop; onMqttConnect overwrites it with
    //    the live status on every successful (re)connect. MQTT connects as soon
    //    as Connectivity selects a transport (WiFi preferred, else LTE).
    char commandsTopic[60] = {0};
    sprintf(commandsTopic, "%s/%s", MQTT_COMMAND_TOPIC, macStr.c_str());

    char infoTopic[60] = {0};
    sprintf(infoTopic, "%s/%s/info", MQTT_EVENT_TOPIC, macStr.c_str());

    static String clientId = "esp32a7670-" + macStr;

    // Last Will: a retained JSON "offline" on /info, so the broker reflects an
    // ungraceful drop in the same shape as the live status; onMqttConnect
    // overwrites it with the full "online" payload on every (re)connect.
    static String willMessage = "{\"status\":\"offline\",\"mac\":\"" + macStr + "\"}";

    mqtt.begin(MQTT_HOST, MQTT_PORT, MQTT_USER, MQTT_PASSWORD,
               clientId.c_str(), commandsTopic,
               infoTopic, willMessage.c_str(),
               onDataReceived, onMqttConnect, resolveBroker);

    connectivity.begin(wifiConnection, simCommunication, mqtt);

    // Pick a transport and attempt the first connect right away, so retained
    // state is published as soon as a link is available.
    connectivity.update();
    mqtt.loop();
}

void loop()
{
    // Strict, single-task sequence so the modem UART is only ever touched by one
    // thing at a time (raw AT polling AND the LTE MQTT socket share it). Never
    // interleave these steps.

    // 1./2. Service WiFi portal and pick/switch the MQTT transport (WiFi>LTE).
    connectivity.update();

    // 3. Service MQTT: reconnect (with backoff) or pump PubSubClient. Incoming
    //    commands are dispatched from here via onDataReceived, on this task, so
    //    the handlers may touch the modem directly.
    mqtt.loop();

    // 4. Poll the modem for incoming calls/SMS.
    sim_com_check_result result = simCommunication.check();
    if (result.event != SIM_COM_NOTHING)
    {
        sendSimComEvent(result);

        // Only publish call status for call-related events
        if (result.event == SIM_COM_CALL || result.event == SIM_COM_CALL_UPDATE)
        {
            publishCallStatus(simCommunication.getCallStatus().c_str());
        }
    }

    // 5. Drive the GNSS state machine and publish a position once ready.
    simCommunication.updateGps();
    if (simCommunication.gpsResultPending())
    {
        publishGps(simCommunication.takeGpsResult());
    }

    // 6. Apply a pending VoLTE toggle and publish the resulting state.
    simCommunication.updateVolte();
    if (simCommunication.volteResultPending())
    {
        publishVolte(simCommunication.takeVolteStatus());
    }

    // 6a. Periodically refresh the retained /info (transport, signal, band,
    //     timestamp) so the broker reflects the current state even without a
    //     reconnect. Only while connected; throttled to limit AT traffic.
    static unsigned long lastInfoPublish = 0;
    static const unsigned long INFO_PUBLISH_INTERVAL_MS = 60000;
    if (mqtt.connected() && millis() - lastInfoPublish >= INFO_PUBLISH_INTERVAL_MS)
    {
        lastInfoPublish = millis();
        publishInfo("online");
    }

    // 6b. Keep the clock real. SNTP runs over lwIP (WiFi) only, so on an
    //     LTE-only link it never sets the time; seed it from the modem's network
    //     clock until it is valid. Once set (or once NTP syncs over WiFi) this
    //     stops. Throttled, and only issues an AT while the time is still unset.
    static unsigned long lastClockTry = 0;
    if (time(nullptr) < 1700000000UL && millis() - lastClockTry > 15000)
    {
        lastClockTry = millis();
        time_t e = simCommunication.getNetworkEpochUTC();
        if (e > 0)
        {
            struct timeval tv = {e, 0};
            settimeofday(&tv, nullptr);
            Serial.printf("Clock seeded from modem: %ld\n", (long)e);
        }
    }

    // 7. Keep the watchdog happy; a stuck loop will reset the device.
    //    (The raw SerialAT<->Serial bridge was removed: with MQTT now riding the
    //    modem UART over LTE, draining SerialAT here would steal the TinyGSM
    //    socket bytes.)
    esp_task_wdt_reset();
    delay(1);
}
