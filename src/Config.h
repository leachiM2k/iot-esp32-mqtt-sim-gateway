#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// Runtime-configurable deployment settings, editable via the on-device web page
// (ConfigServer) and persisted in NVS. The compile-time values in constants.h
// act as the factory defaults when a key has never been written.
//
// Hardware constants (pins, modem timings, modem model) intentionally stay in
// constants.h — they are not deployment config.
struct DeviceConfig
{
    String mqttHost;
    uint16_t mqttPort;
    String mqttUser;
    String mqttPass;
    String apn;
    String dns1;
    String dns2;
    String ntp;
    String tz;
};

class Config
{
public:
    // Load from NVS, falling back to the constants.h defaults per key.
    void load();
    // Persist the current values to NVS.
    void save();

    DeviceConfig values;
};

#endif // CONFIG_H
