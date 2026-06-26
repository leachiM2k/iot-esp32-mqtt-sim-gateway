#ifndef MQTT_SERVICE_H
#define MQTT_SERVICE_H

#include <Arduino.h>
#include <Client.h>
#include <PubSubClient.h>

// MQTT client built on PubSubClient (replaces the esp_mqtt/IDF wrapper). It is
// transport-agnostic: Connectivity hands it the Client& of the currently active
// transport (WiFi or LTE) via setTransport(). Everything runs on the loop()
// task — loop() must be called every iteration; the incoming-command callback
// fires from inside it, so handlers may touch the modem directly (no cross-task
// race anymore).
class MqttService
{
public:
    // Incoming command callback. topic/data are NOT null-terminated; the
    // lengths are authoritative (kept identical to the old esp_mqtt signature
    // so main.cpp's onDataReceived is unchanged).
    typedef void (*OnDataCallback)(const char *topic, int topic_len,
                                   const char *data, int data_len);
    // Called once after every successful (re)connect, on the loop task, right
    // after re-subscribing. Used to re-publish retained state (/info, …).
    typedef void (*OnConnectCallback)();
    // Resolve the broker hostname to an IP using the currently active transport
    // (WiFi DNS or modem DNS). The A7670's inline CIPOPEN DNS is unreliable, so
    // we resolve up front and connect by IP. Returns true on success.
    typedef bool (*HostResolver)(const char *host, IPAddress &out);

    // server/user/pass/clientId must be string literals or otherwise outlive
    // this object (stored by pointer). willTopic/willMessage are copied.
    void begin(const char *host, uint16_t port,
               const char *user, const char *pass,
               const char *clientId,
               const char *commandTopic,
               const char *willTopic, const char *willMessage,
               OnDataCallback onData, OnConnectCallback onConnect = nullptr,
               HostResolver resolver = nullptr);

    // Switch the underlying transport. Disconnects any current session so the
    // next reconnect uses the new Client. highKeepAlive raises the MQTT
    // keepalive (fewer pings) to save mobile data on LTE.
    void setTransport(Client &client, bool highKeepAlive);
    // No usable transport: drop the connection and go idle until one returns.
    void clearTransport();

    // Service PubSubClient and drive reconnect (with backoff). Call every loop.
    void loop();
    bool connected();
    // retain=true keeps the message as the topic's last-known value on the
    // broker. Dropped (with a log line) if MQTT is not currently connected.
    void publish(const char *topic, const char *message, bool retain = false);

private:
    bool reconnect();
    void growBackoff(); // grow the reconnect backoff window (exponential, capped)
    // PubSubClient's callback type differs between cores (plain function pointer
    // vs std::function), so we register a static trampoline that forwards to the
    // single instance — works regardless of MQTT_CALLBACK_SIGNATURE.
    static void staticCallback(char *topic, uint8_t *payload, unsigned int len);
    static MqttService *instance;

    PubSubClient mqtt;
    Client *transport = nullptr; // active transport client (nullptr = idle)

    const char *host = nullptr;
    uint16_t port = 0;
    const char *user = nullptr;
    const char *pass = nullptr;
    const char *clientId = nullptr;
    String commandTopic;
    String willTopic;
    String willMessage;
    OnDataCallback onData = nullptr;
    OnConnectCallback onConnect = nullptr;
    HostResolver resolver = nullptr;

    // Freshly resolved broker IP for the current transport. Reset on a transport
    // switch so each link resolves via its own DNS; re-resolved after a run of
    // failed connects in case the IP changed.
    IPAddress resolvedIp;
    bool haveResolved = false;
    // The IP of the last *successful* MQTT connection, kept across transport
    // switches. Used as a fallback when fresh DNS fails — once the device has
    // connected once it survives later DNS outages (flaky modem DNS). A poisoned
    // resolve (e.g. a sinkhole IP) never connects, so it never becomes good.
    IPAddress lastGoodIp;
    bool haveGoodIp = false;
    int connectFailStreak = 0;

    unsigned long lastReconnectAttempt = 0;
    unsigned long reconnectDelay = 5000; // current backoff window (grows on failure)
};

#endif // MQTT_SERVICE_H
