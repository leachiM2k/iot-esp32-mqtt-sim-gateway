#ifndef MQTT_CLIENT_H
#define MQTT_CLIENT_H

#include <Arduino.h>
#include <mqtt_client.h>

class MqttClient
{
public:
    typedef void (*OnDataCallback)(const char *topic, int topic_len, const char *data, int data_len);

    MqttClient();
    MqttClient(const char *server, const char *user, const char *password, const char *commandTopic, const char *eventTopic, OnDataCallback onDataCallback);

    void init();
    bool connected();
    void reconnect();
    void connect();
    // retain=true keeps the message on the broker as the topic's last-known
    // value, so new subscribers receive it immediately on subscribe.
    void publish(const char *topic, const char *message, bool retain = false);
    void subscribe(const char *topic);

private:
    static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);
    
    const char *mqtt_server;
    const char *mqtt_user;
    const char *mqtt_password;
    String mqtt_command_topic;
    String mqtt_event_topic;
    esp_mqtt_client_handle_t client;
    OnDataCallback onDataCallback;
    bool isConnected; // Track connection status manually
};

#endif // MQTT_CLIENT_H