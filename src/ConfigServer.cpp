#include <Arduino.h>
#include "ConfigServer.h"

// Minimal HTML-escaping for values placed inside double-quoted attributes.
static String esc(const String &s)
{
    String o;
    o.reserve(s.length() + 8);
    for (size_t i = 0; i < s.length(); i++)
    {
        char c = s[i];
        switch (c)
        {
        case '&': o += "&amp;"; break;
        case '<': o += "&lt;"; break;
        case '>': o += "&gt;"; break;
        case '"': o += "&quot;"; break;
        default: o += c; break;
        }
    }
    return o;
}

static String row(const String &label, const String &name, const String &value,
                   const String &type = "text", const String &placeholder = "")
{
    String s = "<label>" + esc(label) + "<input name=\"" + name + "\" type=\"" + type + "\"";
    if (value.length()) s += " value=\"" + esc(value) + "\"";
    if (placeholder.length()) s += " placeholder=\"" + esc(placeholder) + "\"";
    s += "></label>";
    return s;
}

void ConfigServer::begin(Config &cfg)
{
    this->cfg = &cfg;
    server.on("/", HTTP_GET, [this]() { handleRoot(); });
    server.on("/save", HTTP_POST, [this]() { handleSave(); });
}

void ConfigServer::handleRoot()
{
    const DeviceConfig &v = cfg->values;
    String h =
        "<!doctype html><html lang=\"de\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>Geräte-Konfiguration</title><style>"
        "body{font-family:system-ui,sans-serif;max-width:34rem;margin:1.5rem auto;padding:0 1rem;color:#111}"
        "h1{font-size:1.2rem}fieldset{border:1px solid #ccc;border-radius:8px;margin:1rem 0;padding:.5rem 1rem}"
        "legend{font-weight:600;padding:0 .4rem}label{display:block;margin:.6rem 0;font-size:.9rem}"
        "input{display:block;width:100%;box-sizing:border-box;padding:.5rem;margin-top:.2rem;"
        "border:1px solid #bbb;border-radius:6px;font-size:1rem}"
        "button{background:#2563eb;color:#fff;border:0;border-radius:6px;padding:.7rem 1.2rem;font-size:1rem;cursor:pointer}"
        ".note{color:#666;font-size:.8rem}</style></head><body>"
        "<h1>Geräte-Konfiguration</h1>"
        "<p class=\"note\">Gespeicherte Werte liegen im NVS. Speichern startet das Gerät neu.</p>"
        "<form method=\"POST\" action=\"/save\">"
        "<fieldset><legend>MQTT</legend>";
    h += row("Host", "mqtt_host", v.mqttHost);
    h += row("Port", "mqtt_port", String(v.mqttPort), "number");
    h += row("User", "mqtt_user", v.mqttUser);
    h += row("Passwort", "mqtt_pass", "", "password", "unverändert lassen");
    h += "</fieldset><fieldset><legend>Mobilfunk</legend>";
    h += row("APN", "apn", v.apn);
    h += row("DNS primär", "dns1", v.dns1);
    h += row("DNS sekundär", "dns2", v.dns2);
    h += "</fieldset><fieldset><legend>Zeit</legend>";
    h += row("NTP-Server", "ntp", v.ntp);
    h += row("Zeitzone (TZ)", "tz", v.tz);
    h += "</fieldset><button type=\"submit\">Speichern &amp; Neustart</button></form>"
         "</body></html>";
    server.send(200, "text/html; charset=utf-8", h);
}

void ConfigServer::handleSave()
{
    DeviceConfig &v = cfg->values;
    if (server.hasArg("mqtt_host")) v.mqttHost = server.arg("mqtt_host");
    if (server.hasArg("mqtt_port")) v.mqttPort = (uint16_t)server.arg("mqtt_port").toInt();
    if (server.hasArg("mqtt_user")) v.mqttUser = server.arg("mqtt_user");
    // Empty password field = keep the stored one (it is never sent to the page).
    if (server.hasArg("mqtt_pass") && server.arg("mqtt_pass").length()) v.mqttPass = server.arg("mqtt_pass");
    if (server.hasArg("apn")) v.apn = server.arg("apn");
    if (server.hasArg("dns1")) v.dns1 = server.arg("dns1");
    if (server.hasArg("dns2")) v.dns2 = server.arg("dns2");
    if (server.hasArg("ntp")) v.ntp = server.arg("ntp");
    if (server.hasArg("tz")) v.tz = server.arg("tz");

    cfg->save();
    Serial.println("[cfg] saved to NVS, rebooting to apply");

    server.send(200, "text/html; charset=utf-8",
                "<!doctype html><meta charset=\"utf-8\">"
                "<meta http-equiv=\"refresh\" content=\"8; url=/\">"
                "<body style=\"font-family:system-ui;max-width:34rem;margin:2rem auto\">"
                "<h1>Gespeichert</h1><p>Das Gerät startet neu und übernimmt die Einstellungen…</p></body>");
    // Reboot shortly after, so the response can flush first.
    rebootAt = millis() + 1200;
}

void ConfigServer::tick(bool wifiConnected)
{
    if (rebootAt && millis() >= rebootAt)
    {
        ESP.restart();
    }

    if (wifiConnected && !running)
    {
        server.begin();
        running = true;
        Serial.println("[cfg] config web server started on :80");
    }
    else if (!wifiConnected && running)
    {
        // Free port 80 so WiFiManager's AP/captive portal can use it.
        server.stop();
        running = false;
        Serial.println("[cfg] config web server stopped (WiFi down)");
    }

    if (running)
    {
        server.handleClient();
    }
}
