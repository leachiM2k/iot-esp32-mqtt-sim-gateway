#ifndef CONFIG_SERVER_H
#define CONFIG_SERVER_H

#include <Arduino.h>
#include <WebServer.h>
#include "Config.h"

// Minimal on-device configuration web page. Serves a settings form at the
// device's IP / mDNS hostname (port 80) while the STA is connected. It is only
// active when WiFi is connected so it never fights WiFiManager's captive portal
// for port 80 (that runs only in AP/config mode). Saving persists to NVS and
// reboots to apply.
class ConfigServer
{
public:
    void begin(Config &cfg);
    // Call every loop(): starts/stops the server on WiFi state changes and
    // services requests. wifiConnected = STA associated.
    void tick(bool wifiConnected);

private:
    void handleRoot();
    void handleSave();

    WebServer server{80};
    Config *cfg = nullptr;
    bool running = false;
    unsigned long rebootAt = 0; // 0 = no pending reboot
};

#endif // CONFIG_SERVER_H
