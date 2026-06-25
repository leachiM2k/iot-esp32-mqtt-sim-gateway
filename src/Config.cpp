#include <Arduino.h>
#include <Preferences.h>
#include <constants.h>
#include "Config.h"

// NVS namespace for the device config.
static const char *CFG_NS = "cfg";

void Config::load()
{
    Preferences p;
    p.begin(CFG_NS, /*readOnly=*/true);
    values.mqttHost = p.getString("mqtt_host", MQTT_HOST);
    values.mqttPort = p.getUShort("mqtt_port", MQTT_PORT);
    values.mqttUser = p.getString("mqtt_user", MQTT_USER);
    values.mqttPass = p.getString("mqtt_pass", MQTT_PASSWORD);
    values.apn = p.getString("apn", NETWORK_APN);
    values.dns1 = p.getString("dns1", NETWORK_DNS_PRIMARY);
    values.dns2 = p.getString("dns2", NETWORK_DNS_SECONDARY);
    values.ntp = p.getString("ntp", NTP_SERVER);
    values.tz = p.getString("tz", TZ_INFO);
    p.end();
}

void Config::save()
{
    Preferences p;
    p.begin(CFG_NS, /*readOnly=*/false);
    p.putString("mqtt_host", values.mqttHost);
    p.putUShort("mqtt_port", values.mqttPort);
    p.putString("mqtt_user", values.mqttUser);
    p.putString("mqtt_pass", values.mqttPass);
    p.putString("apn", values.apn);
    p.putString("dns1", values.dns1);
    p.putString("dns2", values.dns2);
    p.putString("ntp", values.ntp);
    p.putString("tz", values.tz);
    p.end();
}
