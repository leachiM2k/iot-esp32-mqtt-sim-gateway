#ifndef CONNECTIVITY_H
#define CONNECTIVITY_H

#include <Arduino.h>
#include "Transport.h"
#include "WifiConnection.h"
#include "SimCommunication.h"
#include "MqttService.h"

// Picks the MQTT transport each loop and switches MqttService when it changes.
// Priority is WLAN > LTE > none; when WLAN returns it switches back to WLAN
// automatically. Runs entirely on the loop task.
class Connectivity
{
public:
    void begin(WifiConnection &wifi, SimCommunication &sim, MqttService &mqtt);
    // Service WiFi, evaluate the desired transport and switch if needed. Call
    // every loop() iteration.
    void update();

    Transport active() const { return activeTransport; }
    const char *activeName() const;

private:
    void switchTo(Transport t);

    WifiConnection *wifi = nullptr;
    SimCommunication *sim = nullptr;
    MqttService *mqtt = nullptr;

    WifiTransport wifiTransport;
    LteTransport lteTransport;

    Transport activeTransport = Transport::NONE;

    // The LTE availability check issues AT commands, so it is throttled and its
    // result cached between checks (WiFi status is cheap and checked every loop).
    unsigned long lastLteCheck = 0;
    bool lteAvailable = false;

    // WiFi health: a WiFi link can be associated (WL_CONNECTED) yet have no path
    // to the broker ("WiFi up, no internet"). When MQTT stays down on WiFi past a
    // grace period we "penalize" WiFi so LTE can take over, then periodically
    // lift the penalty to re-test WiFi (in case connectivity returns).
    unsigned long wifiMqttBadSince = 0; // when MQTT first seen down on WiFi (0 = ok)
    unsigned long wifiPenalizedAt = 0;  // when the penalty was applied
    bool wifiPenalized = false;

    // LTE health: the modem's socket service can wedge after a dropped TCP
    // connection (CIPOPEN keeps failing). If MQTT stays down on LTE past a
    // threshold we hard-reset the IP stack (NETCLOSE/NETOPEN) to clear it.
    unsigned long lteMqttBadSince = 0;
};

#endif // CONNECTIVITY_H
