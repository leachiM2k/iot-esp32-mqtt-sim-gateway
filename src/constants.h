
#define MODEM_BAUDRATE                      (115200)
#define MODEM_DTR_PIN                       (32)
#define MODEM_TX_PIN                        (27)
#define MODEM_RX_PIN                        (26)
// The modem boot pin needs to follow the startup sequence.
#define BOARD_PWRKEY_PIN                    (4)
#define BOARD_LED_PIN                       (13)
// There is no modem power control, the LED Pin is used as a power indicator here.
#define BOARD_POWERON_PIN                   (BOARD_LED_PIN)
#define MODEM_RING_PIN                      (33)
#define MODEM_RESET_PIN                     (5)
#define MODEM_RESET_LEVEL                   LOW
#define SerialAT                            Serial1

#define MODEM_GPS_ENABLE_GPIO               (-1)
#define MODEM_GPS_ENABLE_LEVEL              (-1)

#ifndef TINY_GSM_MODEM_A7670
#define TINY_GSM_MODEM_A7670
#endif

#define NETWORK_APN     "sipgate"

// Public DNS servers for the LTE data context. The carrier/APN DNS proved
// unreliable (AT+CDNSGIP -> "0,10", AT+CIPOPEN -> ",11" DNS parse failed), so we
// point the modem at Google DNS via AT+CDNSCFG after the data context is up.
#define NETWORK_DNS_PRIMARY     "8.8.8.8"
#define NETWORK_DNS_SECONDARY   "8.8.4.4"

#define NTP_SERVER "de.pool.ntp.org"
#define TZ_INFO "CET-1CEST,M3.5.0,M10.5.0/3"

// MQTT Server.
// PubSubClient needs host and port separately (it has no URL parser), so the
// broker is split out here. MQTT_SERVER is kept for reference / logging only.
#define MQTT_SERVER "mqtt://server.tld"
#define MQTT_HOST "server.tld"
#define MQTT_PORT 1883
#define MQTT_USER "esp32a7670"
#define MQTT_PASSWORD "PASSWORD-PLACEHOLDER"
#define MQTT_COMMAND_TOPIC "esp32/commands"
#define MQTT_EVENT_TOPIC "esp32/events"
