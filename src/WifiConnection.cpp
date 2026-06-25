#include <Arduino.h>
#include <ESPmDNS.h>
#include <WiFiManager.h>
#include "WifiConnection.h"

void WifiConnection::init() {
        wifiManager.setDebugOutput(true);
        // get current mac address and append to hostname

        uint8_t mac[6];
        WiFi.macAddress(mac);

        wifiHostname = "SG-ESP32-" + String(mac[3], HEX) + String(mac[4], HEX) + String(mac[5], HEX);

        wifiManager.setHostname(wifiHostname);

        wifiManager.setConnectRetries(10);
        wifiManager.setConnectTimeout(10);
        // Non-blocking: setup() must not stall in the captive portal, so the
        // device can run over LTE while WiFi is missing/being configured.
        wifiManager.setConfigPortalBlocking(false);
        // 0 = no timeout: keep the portal open as long as there is no WiFi, so
        // the AP stays reachable for configuration (acted on via process()).
        wifiManager.setConfigPortalTimeout(0);
        wifiManager.setWiFiAutoReconnect(true);
        // Returns quickly: connects to the saved AP if possible, otherwise
        // starts the (non-blocking) config portal.
        wifiManager.autoConnect(wifiHostname.c_str());

        if (MDNS.begin(wifiHostname))
        {
            MDNS.addService("http", "tcp", 80);
            MDNS.setInstanceName(wifiHostname);
        }
        else
        {
            Serial.println("Could not start mDNS!");
        }
}

void WifiConnection::process() {
        wifiManager.process();
}

bool WifiConnection::isConnected() {
        return WiFi.status() == WL_CONNECTED;
}
