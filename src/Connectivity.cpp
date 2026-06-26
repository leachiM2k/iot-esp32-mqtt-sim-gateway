#include <Arduino.h>
#include "Connectivity.h"

// How often to probe the LTE data link (AT round-trips). Kept well above the
// call-state poll so it doesn't crowd the modem UART.
static const unsigned long LTE_CHECK_INTERVAL_MS = 5000;

// How long MQTT may stay down on an associated WiFi link before we treat WiFi
// as broken and let LTE take over. Must exceed a couple of reconnect attempts
// (5 s backoff + ~8 s socket timeout each) so a brief broker blip doesn't flap.
static const unsigned long WIFI_MQTT_GRACE_MS = 25000;

// While penalized (on LTE because WiFi's broker path was dead), re-test WiFi
// this often in case connectivity returned. The re-test costs a short MQTT
// outage while it switches back and probes, so keep it infrequent.
static const unsigned long WIFI_RETRY_MS = 300000; // 5 min


void Connectivity::begin(WifiConnection &wifi, SimCommunication &sim, MqttService &mqtt)
{
    this->wifi = &wifi;
    this->sim = &sim;
    this->mqtt = &mqtt;
    lteTransport.begin(sim);
    activeTransport = Transport::NONE;
}

const char *Connectivity::activeName() const
{
    switch (activeTransport)
    {
    case Transport::WIFI:
        return "WIFI";
    case Transport::LTE:
        return "LTE";
    default:
        return "NONE";
    }
}

void Connectivity::update()
{
    // 1. Keep the WiFi config portal / reconnection serviced.
    wifi->process();

    unsigned long now = millis();
    bool wifiLink = wifiTransport.available(); // L2 associated (WL_CONNECTED)

    // When the WiFi link itself is down, clear the health bookkeeping so that
    // once it returns it is preferred again right away.
    if (!wifiLink)
    {
        wifiMqttBadSince = 0;
        wifiPenalized = false;
    }

    // 2. Track WiFi's broker reachability while it is the active transport. An
    //    associated link with no path to the broker (MQTT stuck disconnected
    //    past the grace period) gets penalized so LTE can take over.
    if (activeTransport == Transport::WIFI)
    {
        if (mqtt->connected())
        {
            wifiMqttBadSince = 0;
            wifiPenalized = false;
        }
        else if (wifiMqttBadSince == 0)
        {
            wifiMqttBadSince = now;
        }
        else if (!wifiPenalized && now - wifiMqttBadSince > WIFI_MQTT_GRACE_MS)
        {
            wifiPenalized = true;
            wifiPenalizedAt = now;
            Serial.println("[conn] WiFi associated but MQTT unreachable -> allow LTE fallback");
        }
    }

    // Periodically lift the penalty to re-test WiFi (e.g. internet returned).
    if (wifiPenalized && now - wifiPenalizedAt > WIFI_RETRY_MS)
    {
        Serial.println("[conn] re-testing WiFi after penalty window");
        wifiPenalized = false;
        wifiMqttBadSince = 0;
    }

    // NOTE: we deliberately do NOT cycle the modem IP stack (NETCLOSE/NETOPEN)
    // on LTE MQTT failures anymore. Repeatedly tearing down and re-activating the
    // PDP context can trip a network-side activation backoff (3GPP T3396): the
    // modem stays *registered* but the network refuses the data bearer, which
    // looks exactly like the "registered, no data" failures we chased (CDNSGIP
    // timeout / CIPOPEN network failure) and survives even a reboot. MqttService
    // now just retries with exponential backoff, which is gentle on the network.

    // 3. Decide the desired transport: prefer a healthy WiFi link, else LTE,
    //    else whatever link physically exists.
    Transport desired;
    if (wifiLink && !wifiPenalized)
    {
        desired = Transport::WIFI;
    }
    else
    {
        // Re-probe LTE only periodically (the check costs AT commands). When
        // it's down but the modem is up, try to (re)open the data context.
        if (now - lastLteCheck >= LTE_CHECK_INTERVAL_MS || lastLteCheck == 0)
        {
            lastLteCheck = now;
            lteAvailable = lteTransport.available();
            if (!lteAvailable && sim->isModemReady())
            {
                lteAvailable = sim->ensureDataConnection();
            }
        }
        if (lteAvailable)
        {
            desired = Transport::LTE;
        }
        else if (wifiLink)
        {
            // Penalized but no LTE to fall back to: keep trying over WiFi.
            desired = Transport::WIFI;
        }
        else
        {
            desired = Transport::NONE;
        }
    }

    // 4. Switch only on a change of the desired transport.
    if (desired != activeTransport)
    {
        switchTo(desired);
    }
}

void Connectivity::switchTo(Transport t)
{
    activeTransport = t;
    switch (t)
    {
    case Transport::WIFI:
        Serial.println("[conn] transport -> WIFI");
        mqtt->setTransport(wifiTransport.client(), /*highKeepAlive=*/false);
        break;
    case Transport::LTE:
        Serial.println("[conn] transport -> LTE");
        mqtt->setTransport(lteTransport.client(), /*highKeepAlive=*/true);
        break;
    case Transport::NONE:
    default:
        Serial.println("[conn] transport -> NONE (no link)");
        mqtt->clearTransport();
        break;
    }
}
