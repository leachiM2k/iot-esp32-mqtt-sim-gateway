#pragma once

#include <Arduino.h>
#include <ESPmDNS.h>
#include <WiFiManager.h>

class WifiConnection {
public:
    void init();

private:
    WiFiManager wifiManager;
    String wifiHostname;
};
