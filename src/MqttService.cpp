#include <Arduino.h>
#include "MqttService.h"

MqttService *MqttService::instance = nullptr;

// PubSubClient's default 256 B buffer is too small for our payloads: incoming
// command JSON and outgoing UCS2-decoded SMS / event JSON can exceed it. 1 KB
// comfortably covers both the largest publish and the largest command.
static const uint16_t MQTT_BUFFER_SIZE = 1024;

// Bound how long a single connect() may block, so a dead broker can't starve
// call/SMS polling or trip the task watchdog.
static const uint16_t MQTT_SOCKET_TIMEOUT_S = 8;

// Reconnect backoff: don't hammer the broker; one attempt per interval.
static const unsigned long RECONNECT_INTERVAL_MS = 5000;

// Keepalive: short over WiFi (cheap, fast dead-peer detection), long over LTE
// to cut keepalive data on the mobile link.
static const uint16_t KEEPALIVE_WIFI_S = 30;
static const uint16_t KEEPALIVE_LTE_S = 120;

void MqttService::begin(const char *host, uint16_t port,
                        const char *user, const char *pass,
                        const char *clientId,
                        const char *commandTopic,
                        const char *willTopic, const char *willMessage,
                        OnDataCallback onData, OnConnectCallback onConnect,
                        HostResolver resolver)
{
    this->host = host;
    this->port = port;
    this->user = user;
    this->pass = pass;
    this->clientId = clientId;
    this->commandTopic = commandTopic;
    this->willTopic = willTopic;
    this->willMessage = willMessage;
    this->onData = onData;
    this->onConnect = onConnect;
    this->resolver = resolver;

    instance = this;
    // Server is set per-attempt from the resolved IP (see reconnect); this is
    // just a sensible default until the first lookup.
    mqtt.setServer(host, port);
    mqtt.setBufferSize(MQTT_BUFFER_SIZE);
    mqtt.setSocketTimeout(MQTT_SOCKET_TIMEOUT_S);
    mqtt.setKeepAlive(KEEPALIVE_WIFI_S);
    mqtt.setCallback(&MqttService::staticCallback);
}

void MqttService::staticCallback(char *topic, uint8_t *payload, unsigned int len)
{
    // PubSubClient null-terminates the topic but not the payload, so the topic
    // length is strlen and the payload length is authoritative (matches the old
    // esp_mqtt-based signature expected by onDataReceived).
    if (instance && instance->onData)
    {
        instance->onData(topic, (int)strlen(topic), (const char *)payload, (int)len);
    }
}

void MqttService::setTransport(Client &client, bool highKeepAlive)
{
    if (mqtt.connected())
    {
        mqtt.disconnect();
    }
    transport = &client;
    mqtt.setClient(client);
    mqtt.setKeepAlive(highKeepAlive ? KEEPALIVE_LTE_S : KEEPALIVE_WIFI_S);
    // Force a fresh resolve on the new transport (each link has its own DNS);
    // the last-known-good IP is intentionally kept as a fallback.
    haveResolved = false;
    connectFailStreak = 0;
    // Attempt the reconnect on the very next loop() rather than after a backoff.
    lastReconnectAttempt = 0;
}

void MqttService::clearTransport()
{
    if (mqtt.connected())
    {
        mqtt.disconnect();
    }
    transport = nullptr;
}

void MqttService::loop()
{
    if (!transport)
    {
        return;
    }
    if (!mqtt.connected())
    {
        reconnect();
        return;
    }
    mqtt.loop();
}

bool MqttService::connected()
{
    return mqtt.connected();
}

bool MqttService::reconnect()
{
    unsigned long now = millis();
    if (now - lastReconnectAttempt < RECONNECT_INTERVAL_MS)
    {
        return false;
    }
    lastReconnectAttempt = now;

    // Resolve via the active transport's DNS: one fresh attempt per transport,
    // and again after a run of failed connects (the IP may have changed).
    if (resolver && (!haveResolved || connectFailStreak >= 5))
    {
        IPAddress ip;
        if (resolver(host, ip))
        {
            resolvedIp = ip;
            haveResolved = true;
            if (connectFailStreak >= 5)
            {
                connectFailStreak = 0;
            }
            Serial.printf("[mqtt] resolved %s -> %s\n", host, ip.toString().c_str());
        }
    }

    IPAddress target;
    bool haveTarget = false;
    if (haveResolved)
    {
        target = resolvedIp;
        haveTarget = true;
    }
    else if (haveGoodIp)
    {
        // Fresh DNS failed but we connected before: reuse that IP so a DNS
        // outage doesn't strand us.
        target = lastGoodIp;
        haveTarget = true;
        Serial.printf("[mqtt] DNS unavailable, using last-known-good %s\n",
                      target.toString().c_str());
    }

    if (haveTarget)
    {
        mqtt.setServer(target, port);
        Serial.printf("[mqtt] connecting to %s (%s):%u ...\n",
                      host, target.toString().c_str(), port);
    }
    else if (!resolver)
    {
        // No resolver configured: let the client resolve the hostname itself.
        mqtt.setServer(host, port);
        Serial.printf("[mqtt] connecting to %s:%u ...\n", host, port);
    }
    else
    {
        Serial.printf("[mqtt] cannot resolve %s yet, will retry\n", host);
        return false;
    }

    // Set a retained Last Will on the info topic so the broker flips the device
    // to "offline" if the link drops ungracefully; onConnect overwrites it.
    bool ok = mqtt.connect(clientId, user, pass,
                           willTopic.c_str(), 1, true, willMessage.c_str());
    if (ok)
    {
        connectFailStreak = 0;
        if (haveTarget)
        {
            lastGoodIp = target;
            haveGoodIp = true;
        }
        Serial.println("[mqtt] connected");
        mqtt.subscribe(commandTopic.c_str());
        if (onConnect)
        {
            onConnect();
        }
    }
    else
    {
        connectFailStreak++;
        Serial.printf("[mqtt] connect failed, state=%d (streak %d)\n",
                      mqtt.state(), connectFailStreak);
    }
    return ok;
}

void MqttService::publish(const char *topic, const char *message, bool retain)
{
    if (!transport || !mqtt.connected())
    {
        Serial.printf("[mqtt] publish dropped (offline): %s\n", topic);
        return;
    }
    if (!mqtt.publish(topic, (const uint8_t *)message, strlen(message), retain))
    {
        Serial.printf("[mqtt] publish failed (payload too large?): %s\n", topic);
    }
}
