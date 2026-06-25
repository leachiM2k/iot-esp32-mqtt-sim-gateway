#pragma once

#include <Arduino.h>
#include <ESPmDNS.h>
#include <WiFiManager.h>

class WifiConnection {
public:
    // Non-blocking bring-up: tries the saved AP, then leaves a config portal
    // open (without blocking) so the device can still come up over LTE while
    // WiFi is being (re)configured. Must be paired with process() in loop().
    void init();
    // Service the WiFiManager config portal. Call every loop() iteration.
    void process();
    // True once the STA is associated with an AP.
    bool isConnected();

private:
    WiFiManager wifiManager;
    String wifiHostname;
};
