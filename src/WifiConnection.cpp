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
        wifiManager.setConfigPortalTimeout(180);
        wifiManager.setWiFiAutoReconnect(true);
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
