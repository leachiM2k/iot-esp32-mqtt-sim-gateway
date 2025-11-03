#include <Arduino.h>
#include <mqtt_client.h>
#include "MqttClient.h"

MqttClient::MqttClient()
{
    isConnected = false;
}

MqttClient::MqttClient(const char *server, const char *user, const char *password, const char *commandTopic, const char *eventTopic, OnDataCallback onDataCallback)
    : mqtt_server(server), mqtt_user(user), mqtt_password(password), mqtt_command_topic(commandTopic), mqtt_event_topic(eventTopic), onDataCallback(onDataCallback)
{
    isConnected = false;
    init();
}

void MqttClient::init()
{
    esp_mqtt_client_config_t mqtt_cfg = {
        .uri = mqtt_server,
        .username = mqtt_user,
        .password = mqtt_password,
    };

    client = esp_mqtt_client_init(&mqtt_cfg);
}

bool MqttClient::connected()
{
    return isConnected;
}

void MqttClient::reconnect()
{
    if (!connected()) {
        Serial.println("Attempting MQTT reconnection...");
        esp_mqtt_client_reconnect(client);
    }
}

void MqttClient::connect()
{
    esp_mqtt_client_start(client);
    esp_mqtt_client_register_event(client, MQTT_EVENT_ANY, &MqttClient::mqtt_event_handler, this);
}

void MqttClient::publish(const char *topic, const char *message)
{
    esp_mqtt_client_publish(client, topic, message, 0, 1, 0);
}

void MqttClient::subscribe(const char *topic)
{
    esp_mqtt_client_subscribe(client, topic, 0);
}

void MqttClient::mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;
    MqttClient *self = static_cast<MqttClient *>(handler_args);
    switch (event_id)
    {
    case MQTT_EVENT_CONNECTED:
        Serial.println("MQTT_EVENT_CONNECTED");
        self->isConnected = true;
        self->subscribe(self->mqtt_command_topic);
        break;
    case MQTT_EVENT_DISCONNECTED:
        Serial.println("MQTT_EVENT_DISCONNECTED");
        self->isConnected = false;
        break;
    case MQTT_EVENT_SUBSCRIBED:
        Serial.println("MQTT_EVENT_SUBSCRIBED");
        break;
    case MQTT_EVENT_UNSUBSCRIBED:
        Serial.println("MQTT_EVENT_UNSUBSCRIBED");
        break;
    case MQTT_EVENT_PUBLISHED:
        Serial.println("MQTT_EVENT_PUBLISHED");
        break;
    case MQTT_EVENT_DATA:
        if(event->topic_len == 0)
        {
            break;
        }
        Serial.println("MQTT_EVENT_DATA");
        Serial.printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
        Serial.printf("DATA=%.*s\r\n", event->data_len, event->data);
        self->publish(event->topic, "");
        if (self->onDataCallback)
        {
            Serial.println("Calling onDataCallback");
            self->onDataCallback(event->topic, event->data, event->data_len);
        }
        break;
    case MQTT_EVENT_ERROR:
        Serial.println("MQTT_EVENT_ERROR");
        self->isConnected = false;
        break;
    }
}
