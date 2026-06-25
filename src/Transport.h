#ifndef TRANSPORT_H
#define TRANSPORT_H

#include <Arduino.h>
#include <Client.h>
#include <WiFi.h>
// SimCommunication.h pulls in constants.h + TinyGsmClient.h in the right order
// (the modem model must be defined before TinyGsmClient.h).
#include "SimCommunication.h"

// Which physical link MQTT currently rides on. WLAN is preferred; LTE is the
// fallback. NONE means no usable link right now.
enum class Transport
{
    NONE,
    WIFI,
    LTE
};

// A transport abstracts "is the link up?" and "which Arduino Client do I hand
// to PubSubClient?". The two transports are fully independent (separate Client
// instances); Connectivity picks one and feeds its client() to MqttService.
class ITransport
{
public:
    virtual ~ITransport() {}
    virtual bool available() = 0;   // is the underlying link usable right now?
    virtual Client &client() = 0;   // the Client to use for MQTT
    virtual const char *name() = 0;
};

// MQTT over the WiFi IP stack (Arduino WiFiClient).
class WifiTransport : public ITransport
{
public:
    bool available() override { return WiFi.status() == WL_CONNECTED; }
    Client &client() override { return cli; }
    const char *name() override { return "WIFI"; }

private:
    WiFiClient cli;
};

// MQTT over the LTE data context via TinyGSM (TinyGsmClient bound to the modem
// owned by SimCommunication). availability is the modem's data state; the AT
// checks behind it are not free, so callers should throttle available() polls.
class LteTransport : public ITransport
{
public:
    // Bind the client to the modem. Must be called once the modem exists
    // (after SimCommunication::init()). mux 0 is reserved for MQTT here.
    void begin(SimCommunication &sim)
    {
        this->sim = &sim;
        cli.init(&sim.getModem(), 0);
    }

    bool available() override { return sim && sim->isDataConnected(); }
    Client &client() override { return cli; }
    const char *name() override { return "LTE"; }

private:
    SimCommunication *sim = nullptr;
    TinyGsmClient cli;
};

#endif // TRANSPORT_H
