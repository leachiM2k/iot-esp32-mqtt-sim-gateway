#include <Arduino.h>
#include <constants.h>

#include <WiFiManager.h>
#include <ESPmDNS.h>
#include <esp_log.h>
#include <mqtt_client.h>
#include <ArduinoJson.h>
#include <TinyGsmClient.h>
#include <StreamDebugger.h>
#include "MqttClient.h"
#include "WifiConnection.h"
#include "SimCommunication.h"

void onDataReceived(const char *topic, const char *data, int length);

WifiConnection wifiConnection;
StreamDebugger debugger(SerialAT, Serial);
TinyGsm modem(debugger);
MqttClient mqtt;
SimCommunication simCommunication;

void sendSimComEvent(sim_com_check_result &result)
{
    // Get current time
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    // format time to ISO8601
    char timeStr[30];
    strftime(timeStr, sizeof(timeStr), "%FT%TZ", &timeinfo);

    // Get MAC address
    uint8_t mac[6];
    char macStr[18] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(macStr, sizeof(macStr), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    // Generate JSON
    JsonDocument doc;

    doc["mac"] = macStr;
    doc["time"] = timeStr;
    doc["timestamp"] = now;
    doc["event"] = result.event;
    doc["data"] = result.data;

    char json[200];
    size_t n = serializeJson(doc, json);

    Serial.println(json);

    char topic[60] = {0};
    sprintf(topic, "%s/%s/checkresult", MQTT_EVENT_TOPIC, macStr);

    mqtt.publish(topic, json);
}

void publishCallStatus(const char *status)
{
    // Get current time
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    // format time to ISO8601
    char timeStr[30];
    strftime(timeStr, sizeof(timeStr), "%FT%TZ", &timeinfo);

    // Get MAC address
    uint8_t mac[6];
    char macStr[18] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(macStr, sizeof(macStr), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    // Generate JSON
    JsonDocument doc;

    doc["mac"] = macStr;
    doc["time"] = timeStr;
    doc["timestamp"] = now;
    doc["status"] = status;

    char json[200];
    size_t n = serializeJson(doc, json);

    Serial.println(json);

    char topic[60] = {0};
    sprintf(topic, "%s/%s/callstatus", MQTT_EVENT_TOPIC, macStr);

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

void onDataReceived(const char *topic, const char *data, int length)
{
    Serial.print("Callback");
    Serial.println(topic);
    Serial.println(data);
    Serial.println(length);

    // Allocate a temporary JsonDocument
    JsonDocument doc;

    // Deserialize the JSON document
    DeserializationError error = deserializeJson(doc, data, length);

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

    Serial.print("Searching Action enum for:");
    Serial.println(action);
    Action act = getAction(action);
    Serial.print("Action enum:");
    Serial.println((int)act);

    switch (act)
    {
    case Action::REBOOT:
        Serial.println("Rebooting...");
        ESP.restart();
        break;

    case Action::CALL:
        number = doc["number"];
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

    Serial.printf("Callback - Topic: %s, Data: %s\n", topic, data);
}

void setup()
{
    Serial.begin(115200); // Set console baud rate

    wifiConnection.init();

    configTzTime(TZ_INFO, NTP_SERVER);

    simCommunication.init();

    uint8_t mac[6];
    char macStr[18] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(macStr, sizeof(macStr), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    char commandsTopic[60] = {0};
    sprintf(commandsTopic, "%s/%s", MQTT_COMMAND_TOPIC, macStr);
    mqtt = MqttClient(MQTT_SERVER, MQTT_USER, MQTT_PASSWORD, commandsTopic, MQTT_EVENT_TOPIC, onDataReceived);
    mqtt.connect();

    char infoTopic[60] = {0};
    sprintf(infoTopic, "%s/%s/info", MQTT_EVENT_TOPIC, macStr);
    mqtt.publish(infoTopic, "Device started");
}

void loop()
{
    sim_com_check_result result = simCommunication.check();

    if (result.event != SIM_COM_NOTHING)
    {
        sendSimComEvent(result);
        publishCallStatus(simCommunication.getCallStatus().c_str());
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
