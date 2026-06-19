#include <Arduino.h>
#include <constants.h>

#include <WiFiManager.h>
#include <ESPmDNS.h>
#include <esp_log.h>
#include <mqtt_client.h>
#include <ArduinoJson.h>
#include <TinyGsmClient.h>
#include <StreamDebugger.h>
#include <esp_task_wdt.h>
#include "MqttClient.h"
#include "WifiConnection.h"
#include "SimCommunication.h"

// Task-watchdog timeout. Must exceed the longest single blocking call in the
// modem init (the SMS DONE wait, ~30 s) while still catching a real hang. The
// bounded init loops feed the watchdog so they don't trip it prematurely.
static const uint32_t WDT_TIMEOUT_S = 60;

void onDataReceived(const char *topic, int topic_len, const char *data, int data_len);

WifiConnection wifiConnection;
StreamDebugger debugger(SerialAT, Serial);
TinyGsm modem(debugger);
MqttClient mqtt;
SimCommunication simCommunication;

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

    mqtt.publish(topic, json);
}

enum class Action
{
    REBOOT,
    CALL,
    ACCEPT,
    HANGUP,
    SMS,
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

    default:
        break;
    }
}

void setup()
{
    Serial.begin(115200); // Set console baud rate

    // 1. Bring up WiFi + MQTT first, so the device is reachable and can report
    //    problems even if the modem never comes up. WiFiManager may block in
    //    its captive portal here, which is why the watchdog is armed only
    //    afterwards.
    wifiConnection.init();

    configTzTime(TZ_INFO, NTP_SERVER);

    String macStr = getCurrentMacAddress();

    char commandsTopic[60] = {0};
    sprintf(commandsTopic, "%s/%s", MQTT_COMMAND_TOPIC, macStr.c_str());
    mqtt = MqttClient(MQTT_SERVER, MQTT_USER, MQTT_PASSWORD, commandsTopic, MQTT_EVENT_TOPIC, onDataReceived);
    mqtt.connect();

    char infoTopic[60] = {0};
    sprintf(infoTopic, "%s/%s/info", MQTT_EVENT_TOPIC, macStr.c_str());

    String infoMessage = "Device started at " + getCurrentTimeISO8601() + ", initializing modem";
    mqtt.publish(infoTopic, infoMessage.c_str());

    // 2. Arm the task watchdog before the modem init (which contains the
    //    long-running waits). Reconfigures the default 5 s TWDT and subscribes
    //    this task; the init helpers feed it, so a genuine hang triggers a
    //    reset instead of a silent freeze.
    esp_task_wdt_init(WDT_TIMEOUT_S, true);
    esp_task_wdt_add(NULL);

    // 3. Modem init, bounded by timeouts. On failure stay up in a degraded mode
    //    so the device remains reachable (e.g. for a remote reboot).
    if (!simCommunication.init())
    {
        Serial.println("Modem initialization failed - running in degraded mode.");
        mqtt.publish(infoTopic, "Modem initialization failed");
        return;
    }

    int smsCount = simCommunication.getSMSCount();
    Serial.print("Number of SMS on SIM card: ");
    Serial.println(smsCount);

    mqtt.publish(infoTopic, "Modem ready");
}

void loop()
{
    // Keep the watchdog happy each iteration; a stuck loop will reset the device.
    esp_task_wdt_reset();

    // Check MQTT connection and reconnect if necessary
    if (!mqtt.connected()) {
        Serial.println("MQTT disconnected, attempting reconnect...");
        mqtt.reconnect();
        delay(1000); // Give it time to connect
    }

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

    if (SerialAT.available())
    {
        Serial.write(SerialAT.read());
    }
    if (Serial.available())
    {
        SerialAT.write(Serial.read());
    }
    delay(1);
}
