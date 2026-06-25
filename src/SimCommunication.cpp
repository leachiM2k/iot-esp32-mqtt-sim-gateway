#include <Arduino.h>
#include <constants.h>
#include <StreamDebugger.h>
#include <TinyGsmClient.h>
#include <esp_task_wdt.h>
#include "SimCommunication.h"

// Upper bounds for the blocking modem startup steps. Without these the device
// could hang forever in setup() with no SIM / no antenna / no network. Each
// loop also feeds the task watchdog so a single stuck AT call still triggers a
// reset rather than a silent freeze.
static const uint32_t MODEM_START_TIMEOUT_MS = 30000;
static const uint32_t SMS_DONE_TIMEOUT_MS    = 30000;
static const uint32_t SIM_READY_TIMEOUT_MS   = 30000;
static const uint32_t NET_REG_TIMEOUT_MS     = 120000;
static const uint32_t RING_SETTLE_TIMEOUT_MS = 10000;

// Feed the task watchdog. No-op if the current task isn't subscribed.
static inline void feedWatchdog()
{
    esp_task_wdt_reset();
}

// public methods

bool SimCommunication::init()
{
    SerialAT.begin(115200, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);
    currentCallStatus = NO_CALL;
    modemReady = false;

    powerUpModem();

    if (!startModem())
    {
        Serial.println("Modem did not respond; aborting modem init.");
        return false;
    }

    modem.sendAT("+SIMCOMATI");
    modem.waitResponse();

    // Wait for the modem to finish its SMS/phonebook startup (PB/SMS DONE).
    Serial.println("Wait SMS Done.");
    feedWatchdog();
    if (!modem.waitResponse(SMS_DONE_TIMEOUT_MS, "SMS DONE")) {
        Serial.println("Timed out waiting for SMS DONE; aborting modem init.");
        return false;
    }
    feedWatchdog();

    if (!isSimCardOnline())
    {
        Serial.println("SIM card not ready; aborting modem init.");
        return false;
    }

    setNetworkMode();
    setNetworkApn();

    if (!awaitNetworkRegistration())
    {
        Serial.println("Network registration failed; aborting modem init.");
        return false;
    }

    // Wait until MODEM_RING_PIN settles HIGH to avoid a false ring at startup,
    // but don't block forever if it is stuck LOW.
    uint32_t ringStart = millis();
    while (digitalRead(MODEM_RING_PIN) == LOW)
    {
        feedWatchdog();
        Serial.println("Waiting for RING pin to go HIGH...");
        if (millis() - ringStart > RING_SETTLE_TIMEOUT_MS)
        {
            Serial.println("RING pin still LOW after timeout, proceeding anyway.");
            break;
        }
        delay(100);
    }

    // Set SMS text mode (not PDU mode)
    modem.sendAT("+CMGF=1");
    modem.waitResponse();

    // Configure SMS to be stored in SIM memory instead of being forwarded directly
    // Format: AT+CNMI=<mode>,<mt>,<bm>,<ds>,<bfr>
    // mode=1: buffer unsolicited result codes in TA when TA-TE link is reserved
    // mt=1: SMS-DELIVER is stored and indication is sent to TE
    // bm=0/ds=0: no SMS-STATUS-REPORTs are routed
    // bfr=0: buffer is flushed when mode 1 is entered
    modem.sendAT("+CNMI=1,1,0,0,0");
    modem.waitResponse();

    // Set preferred SMS storage to SIM card (read, write, receive)
    modem.sendAT("+CPMS=\"SM\",\"SM\",\"SM\"");
    modem.waitResponse();

    // Cache the IMEI once (it never changes); used in the /info payload.
    imei = modem.getIMEI();
    Serial.print("IMEI: ");
    Serial.println(imei);

    modemReady = true;
    Serial.println("Modem init complete.");
    return true;
}

bool SimCommunication::isModemReady() const
{
    return modemReady;
}

String SimCommunication::getModemName()
{
    if (!modemReady)
    {
        return "";
    }
    return modem.getModemName();
}

String SimCommunication::getImei()
{
    return imei;
}

network_info SimCommunication::readNetworkInfo()
{
    network_info n;
    n.signal = 99; // 99 = unknown (CSQ convention)
    if (!modemReady)
    {
        return n;
    }

    feedWatchdog();
    n.oper = modem.getOperator();
    n.signal = modem.getSignalQuality(); // CSQ 0..31, 99 = unknown

    // Both the active radio access tech and the band sit in the +CPSI? line, e.g.
    //   +CPSI: LTE,Online,262-03,0x8CB6,16718643,154,EUTRAN-BAND3,...
    // The first field is the real RAT (LTE/GSM/WCDMA/NR), unlike AT+CNMP which
    // only reports the *preference* (often AUTO).
    feedWatchdog();
    modem.sendAT("+CPSI?");
    String resp;
    if (modem.waitResponse(2000, resp) == 1)
    {
        int p = resp.indexOf("+CPSI:");
        if (p >= 0)
        {
            int s = p + 6; // past "+CPSI:"
            int comma = resp.indexOf(',', s);
            if (comma > s)
            {
                n.mode = resp.substring(s, comma);
                n.mode.trim();
            }
        }
        int b = resp.indexOf("BAND");
        if (b >= 0)
        {
            int start = resp.lastIndexOf(',', b) + 1;
            int end = resp.indexOf(',', b);
            if (end < 0)
            {
                end = resp.indexOf('\n', b);
            }
            if (end > start)
            {
                n.band = resp.substring(start, end);
                n.band.trim();
            }
        }
    }
    if (n.mode.length() == 0)
    {
        n.mode = modem.getNetworkModes(); // fallback: preference (AUTO/LTE/...)
    }
    return n;
}

TinyGsm &SimCommunication::getModem()
{
    return modem;
}

bool SimCommunication::isDataConnected()
{
    if (!modemReady)
    {
        return false;
    }
    // isNetworkConnected() = registered (CEREG/CGREG); isGprsConnected() on the
    // A7670 confirms the socket data context is open (NETOPEN? -> 1). Both must
    // hold before a TCP socket can be opened.
    return modem.isNetworkConnected() && modem.isGprsConnected();
}

bool SimCommunication::ensureDataConnection()
{
    if (!modemReady)
    {
        return false;
    }
    if (modem.isGprsConnected())
    {
        return true;
    }
    // Data context dropped (typically only on a CSFB call over a non-VoLTE
    // network). Re-open it (AT+NETOPEN). On the verified VoLTE setup data
    // survives the call, so this is effectively a no-op there.
    feedWatchdog();
    Serial.println("Data context down, re-opening (AT+NETOPEN)...");
    modem.setNetworkActive();
    return modem.isGprsConnected();
}

bool SimCommunication::resolveHost(const char *host, IPAddress &out)
{
    if (!modemReady)
    {
        return false;
    }
    feedWatchdog();
    // AT+CDNSGIP="host" -> ... +CDNSGIP: <ok>,"<domain>","<ip>"[,"<ip2>"]
    // The URC may arrive before or after OK depending on firmware, so we wait
    // for the "+CDNSGIP:" tag itself, then collect the rest of that line.
    modem.sendAT("+CDNSGIP=\"", host, "\"");
    String before;
    if (modem.waitResponse(12000L, before, GF("+CDNSGIP:")) != 1)
    {
        Serial.printf("DNS: no +CDNSGIP for %s\n", host);
        return false;
    }
    feedWatchdog();
    // Read the remainder of the URC line. There is no trailing OK after the URC
    // in the async case, so this typically returns on timeout with the line in
    // 'rest'; we parse it regardless of the return code.
    String rest;
    modem.waitResponse(3000L, rest);

    // rest looks like:  1,"dev.rotmanov.de","217.160.172.133"
    rest.trim();
    if (rest.startsWith("0"))
    {
        Serial.printf("DNS: lookup failed for %s\n", host);
        return false;
    }
    int q1 = rest.indexOf('"');            // domain open quote
    int q2 = rest.indexOf('"', q1 + 1);    // domain close quote
    int q3 = rest.indexOf('"', q2 + 1);    // ip open quote
    int q4 = rest.indexOf('"', q3 + 1);    // ip close quote
    if (q3 < 0 || q4 < 0)
    {
        Serial.printf("DNS: unparseable reply for %s: %s\n", host, rest.c_str());
        return false;
    }
    String ip = rest.substring(q3 + 1, q4);
    if (!out.fromString(ip))
    {
        Serial.printf("DNS: bad IP '%s' for %s\n", ip.c_str(), host);
        return false;
    }
    Serial.printf("DNS: %s -> %s\n", host, ip.c_str());
    return true;
}

bool SimCommunication::resetDataConnection()
{
    if (!modemReady)
    {
        return false;
    }
    Serial.println("Resetting LTE IP stack (NETCLOSE/NETOPEN)...");
    feedWatchdog();
    modem.sendAT("+NETCLOSE");
    modem.waitResponse(20000L); // may report already-closed/error; tolerate it
    delay(1000);
    feedWatchdog();
    bool ok = modem.setNetworkActive(); // NETOPEN
    feedWatchdog();
    bool up = ok && modem.isGprsConnected();
    Serial.printf("IP stack reset %s\n", up ? "ok" : "failed");
    return up;
}

time_t SimCommunication::getNetworkEpochUTC()
{
    if (!modemReady)
    {
        return 0;
    }
    int yr = 0, mo = 0, dy = 0, hh = 0, mm = 0, ss = 0;
    float tz = 0; // local offset in quarter-hours (CCLK convention)
    feedWatchdog();
    if (!modem.getNetworkTime(&yr, &mo, &dy, &hh, &mm, &ss, &tz))
    {
        return 0;
    }
    if (yr < 2020)
    {
        return 0; // modem hasn't obtained network time yet
    }

    // Convert the broken-down date to a UTC epoch directly (newlib has no
    // timegm, and mktime would apply the configured local TZ). Howard Hinnant's
    // days-from-civil algorithm for the proleptic Gregorian calendar.
    int y = yr - (mo <= 2);
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + dy - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = (long)era * 146097 + (long)doe - 719468;
    time_t asUtc = (time_t)days * 86400 + hh * 3600 + mm * 60 + ss;
    // CCLK gives *local* time plus the zone offset (in 15-min units); subtract
    // the offset to get true UTC.
    return asUtc - (time_t)(tz * 15.0f * 60.0f);
}

static const unsigned long GPS_POLL_INTERVAL_MS = 2000;   // CGNSSINFO poll spacing
static const unsigned long GPS_FIX_TIMEOUT_MS   = 90000;  // give up acquiring a fix

void SimCommunication::requestGps()
{
    // Only sets a flag — no modem access here, so this is safe to call from the
    // MQTT task. The actual GNSS work happens in updateGps() on the loop task.
    gpsRequested = true;
}

void SimCommunication::powerDownGps()
{
    gpsPowerDownRequested = true;
}

bool SimCommunication::gpsResultPending() const
{
    return gpsResultReady;
}

gps_result SimCommunication::takeGpsResult()
{
    gpsResultReady = false;
    return lastGpsResult;
}

void SimCommunication::updateGps()
{
    if (!modemReady)
    {
        return;
    }

    // Power-down request wins (saves energy when GPS is not needed).
    if (gpsPowerDownRequested)
    {
        gpsPowerDownRequested = false;
        gpsRequested = false;
        if (gpsEnabled)
        {
            feedWatchdog();
            modem.disableGPS();
            gpsEnabled = false;
            Serial.println("GNSS powered down.");
        }
        return;
    }

    if (!gpsRequested)
    {
        return;
    }

    // Power up GNSS once (may take a few seconds, but we own the modem here).
    if (!gpsEnabled)
    {
        Serial.println("Enabling GNSS...");
        feedWatchdog();
        if (!modem.enableGPS())
        {
            Serial.println("Failed to enable GNSS.");
            lastGpsResult = {false, 0.0, 0.0, 0.0f, 0, 0.0f};
            gpsResultReady = true;
            gpsRequested = false;
            return;
        }
        gpsEnabled = true;
        gpsRequestStart = millis();
        lastGpsPoll = 0;
    }

    // Poll CGNSSINFO at a throttled rate until we get a fix or time out.
    unsigned long now = millis();
    if (now - lastGpsPoll < GPS_POLL_INTERVAL_MS)
    {
        return;
    }
    lastGpsPoll = now;

    uint8_t status = 0;
    float lat = 0, lon = 0, speed = 0, alt = 0, accuracy = 0;
    int vsat = 0, usat = 0;
    feedWatchdog();
    // The accuracy out-param carries the horizontal DOP for this firmware (the
    // +CGNSSINFO field TinyGSM lands on is HDOP, e.g. 3.35).
    if (modem.getGPS(&status, &lat, &lon, &speed, &alt, &vsat, &usat, &accuracy))
    {
        lastGpsResult.fix = true;
        // This modem's +CGNSSINFO already reports lat/lon in decimal degrees
        // (e.g. 51.2450867,N) with the hemisphere applied by TinyGSM — use them
        // directly. (Despite the TinyGSM comment claiming ddmm.mmmmmm, the
        // A7670E-FASE firmware emits decimal degrees here.)
        lastGpsResult.lat = lat;
        lastGpsResult.lon = lon;
        lastGpsResult.altitude = alt;
        lastGpsResult.satellites = vsat;
        lastGpsResult.hdop = accuracy;
        gpsResultReady = true;
        gpsRequested = false;
        Serial.printf("GPS fix: %.6f, %.6f (%d sats, hdop %.2f)\n",
                      lastGpsResult.lat, lastGpsResult.lon,
                      lastGpsResult.satellites, lastGpsResult.hdop);
        return;
    }

    if (now - gpsRequestStart > GPS_FIX_TIMEOUT_MS)
    {
        Serial.println("GPS fix timeout (GNSS stays on; try again).");
        lastGpsResult = {false, 0.0, 0.0, 0.0f, 0, 0.0f};
        gpsResultReady = true;
        gpsRequested = false;
    }
}

// --- VoLTE (voice domain preference) ---------------------------------------

volte_status SimCommunication::readVolteStatus()
{
    volte_status s = {false, -1, false, false};
    if (!modemReady) return s;
    String resp;

    modem.sendAT("+CEVDP?");
    if (modem.waitResponse(2000, resp) == 1)
    {
        int i = resp.indexOf("+CEVDP:");
        if (i >= 0) s.cevdp = resp.substring(i + 7).toInt();
    }

    resp = "";
    modem.sendAT("+CAVIMS?");
    if (modem.waitResponse(2000, resp) == 1)
    {
        int i = resp.indexOf("+CAVIMS:");
        if (i >= 0) s.imsAvailable = (resp.substring(i + 8).toInt() == 1);
    }

    resp = "";
    modem.sendAT("+CIREG?");
    if (modem.waitResponse(2000, resp) == 1)
    {
        // +CIREG: <n>,<reg_info>,...  -> reg_info (2nd field) == 1 means registered
        int i = resp.indexOf("+CIREG:");
        if (i >= 0)
        {
            int c1 = resp.indexOf(",", i);
            if (c1 >= 0) s.imsRegistered = (resp.substring(c1 + 1).toInt() == 1);
        }
    }

    // CEVDP 3 = IMS preferred, 4 = IMS only -> VoLTE on.
    s.enabled = (s.cevdp == 3 || s.cevdp == 4);
    return s;
}

void SimCommunication::requestVolte(bool on)
{
    volteTarget = on;
    volteChangeRequested = true;
}

void SimCommunication::updateVolte()
{
    if (!modemReady || !volteChangeRequested) return;
    volteChangeRequested = false;

    feedWatchdog();
    int cevdp = volteTarget ? 3 : 1; // 3 = IMS preferred (VoLTE), 1 = CS only (CSFB/2G)
    Serial.printf("Setting VoLTE %s (AT+CEVDP=%d)\n", volteTarget ? "ON" : "OFF", cevdp);
    modem.sendAT("+CEVDP=", cevdp);
    modem.waitResponse(3000);

    delay(500);
    feedWatchdog();
    lastVolteStatus = readVolteStatus();
    volteResultReady = true;
    Serial.printf("VoLTE now: cevdp=%d ims_avail=%d ims_reg=%d\n",
                  lastVolteStatus.cevdp, lastVolteStatus.imsAvailable, lastVolteStatus.imsRegistered);
}

bool SimCommunication::volteResultPending() const
{
    return volteResultReady;
}

volte_status SimCommunication::takeVolteStatus()
{
    volteResultReady = false;
    return lastVolteStatus;
}

// Polling cadence for check(). The live call state is polled often; SMS
// storage is polled less frequently. SMS are detected by polling storage
// rather than via the RI pin: the RI pulse on SMS arrival is far too brief
// (~120 ms) to catch reliably and was only ever seen by chance (e.g. while the
// pin stayed LOW during a voice call), so an SMS would not surface until the
// next incoming call.
static const unsigned long CALL_POLL_INTERVAL_MS = 500;
static const unsigned long SMS_POLL_INTERVAL_MS  = 3000;

sim_com_check_result SimCommunication::check()
{
    // Nothing to poll if the modem never came up (degraded mode).
    if (!modemReady)
    {
        return {SIM_COM_NOTHING, ""};
    }

    // Emit any queued SMS one per call, so each becomes its own MQTT event.
    if (!smsQueue.empty())
    {
        String sms = smsQueue.front();
        smsQueue.erase(smsQueue.begin());
        return {SIM_COM_SMS, sms};
    }

    // Poll the SIM's SMS storage periodically (independent of the RI pin),
    // draining all stored messages into the queue.
    static unsigned long lastSmsPoll = 0;
    unsigned long now = millis();
    if (now - lastSmsPoll >= SMS_POLL_INTERVAL_MS)
    {
        lastSmsPoll = now;
        if (readAllSMS() > 0)
        {
            Serial.println("Incoming SMS detected.");
            String sms = smsQueue.front();
            smsQueue.erase(smsQueue.begin());
            return {SIM_COM_SMS, sms};
        }
    }

    // Track the call state from the modem itself rather than from the RING pin
    // alone, so remote hangups and answered outgoing calls are detected too.
    return updateCallState();
}

current_call_status SimCommunication::parseCallStatus(const String &clcc, int clccIndex)
{
    // +CLCC: <id>,<dir>,<stat>,<mode>,<mpty>[,"<number>",<type>]
    // <stat>: 0=active 1=held 2=dialing(MO) 3=alerting(MO) 4=incoming(MT) 5=waiting(MT)
    int afterTag = clccIndex + 6; // past "+CLCC:"
    int c1 = clcc.indexOf(",", afterTag);       // after <id>
    if (c1 == -1) return NO_CALL;
    int c2 = clcc.indexOf(",", c1 + 1);         // after <dir>
    if (c2 == -1) return NO_CALL;
    int c3 = clcc.indexOf(",", c2 + 1);         // after <stat>
    if (c3 == -1) return NO_CALL;

    int stat = clcc.substring(c2 + 1, c3).toInt();
    switch (stat)
    {
    case 0: // active
    case 1: // held
        return ESTABLISHED;
    case 2: // dialing (outgoing)
    case 3: // alerting (outgoing)
        return CALLING;
    case 4: // incoming
    case 5: // waiting
        return RINGING;
    default:
        return NO_CALL;
    }
}

String SimCommunication::parseCallNumber(const String &clcc, int clccIndex)
{
    int start = clcc.indexOf("\"", clccIndex);
    if (start == -1) return "";
    int end = clcc.indexOf("\"", start + 1);
    if (end == -1) return "";
    return clcc.substring(start + 1, end);
}

sim_com_check_result SimCommunication::updateCallState()
{
    static unsigned long lastPoll = 0;
    unsigned long now = millis();
    if (now - lastPoll < CALL_POLL_INTERVAL_MS)
    {
        return {SIM_COM_NOTHING, ""};
    }
    lastPoll = now;

    modem.sendAT(GF("+CLCC"));
    String data;
    int8_t res = modem.waitResponse(1000, data);
    if (res != 1)
    {
        // Could not query the modem; keep the last known state.
        return {SIM_COM_NOTHING, ""};
    }

    current_call_status newStatus = NO_CALL;
    String number = "";

    int clccIndex = data.indexOf("+CLCC:");
    if (clccIndex != -1)
    {
        newStatus = parseCallStatus(data, clccIndex);
        number = parseCallNumber(data, clccIndex);
    }
    // No "+CLCC:" line means there is no active call -> NO_CALL.

    if (newStatus == currentCallStatus)
    {
        return {SIM_COM_NOTHING, ""};
    }

    current_call_status previous = currentCallStatus;
    currentCallStatus = newStatus;

    Serial.print("Call state changed: ");
    Serial.print((int)previous);
    Serial.print(" -> ");
    Serial.println((int)newStatus);

    // A fresh incoming call (transition into RINGING) is reported as a CALL
    // event carrying the caller's number; every other transition (answered,
    // remote hangup, outgoing call connecting, etc.) is a status UPDATE.
    if (newStatus == RINGING && previous != RINGING)
    {
        Serial.print("Incoming call from: ");
        Serial.println(number);
        return {SIM_COM_CALL, number};
    }

    return {SIM_COM_CALL_UPDATE, ""};
}

// get current call status as string
String SimCommunication::getCallStatus()
{
    switch (currentCallStatus)
    {
    case NO_CALL:
        return "NO_CALL";
    case CALLING:
        return "CALLING";
    case RINGING:
        return "RINGING";
    case ESTABLISHED:
        return "ESTABLISHED";
    default:
        return "UNKNOWN";
    }
}

void SimCommunication::makeCall(const char *number)
{
    if(modem.callNumber(number))
    {
        currentCallStatus = CALLING;
    }
    else
    {
        Serial.println("Failed to make call.");
    }
}

bool SimCommunication::hangupCall()
{
    modem.sendAT(GF("+CHUP"));
    int8_t res = modem.waitResponse(1000);
    if (res == 1)
    {
        currentCallStatus = NO_CALL;
        Serial.println("Call hung up.");
        return true;
    }
    else
    {
        Serial.println("Failed to hang up call.");
        return false;
    }
}

bool SimCommunication::acceptCall()
{
    modem.sendAT(GF("A"));
    int8_t res = modem.waitResponse(1000);
    if (res == 1)
    {
        currentCallStatus = ESTABLISHED;
        Serial.println("Call accepted.");
        return true;
    }
    else
    {
        Serial.println("Failed to accept call.");
        return false;
    }
}

void SimCommunication::sendUSSD(const char *ussd)
{
    String response = modem.sendUSSD(ussd);
    Serial.print("USSD response: ");
    Serial.println(response);
}

void SimCommunication::sendSMS(const char *number, const char *message)
{
    if (!modem.sendSMS(number, message))
    {
        Serial.println("Failed to send SMS");
    }
    else 
    {
        Serial.println("SMS sent successfully");
    }
}

int SimCommunication::getSMSCount()
{
    int smsCount = 0;
    
    // Use AT+CPMS? command to query message storage status
    modem.sendAT(GF("+CPMS?"));
    String response;
    int8_t res = modem.waitResponse(5000, response);
    
    if (res == 1 && response.indexOf("OK") != -1)
    {
        // Response format: +CPMS: "SM",used,total,"SM",used,total,"SM",used,total
        // We want the first 'used' value which represents SMS in SIM storage
        
        int cpmsIndex = response.indexOf("+CPMS:");
        if (cpmsIndex != -1)
        {
            // Find the first comma after +CPMS: "SM",
            int firstQuoteEnd = response.indexOf("\",", cpmsIndex);
            if (firstQuoteEnd != -1)
            {
                int usedStart = firstQuoteEnd + 2; // Skip ","
                int usedEnd = response.indexOf(",", usedStart);
                if (usedEnd != -1)
                {
                    String usedStr = response.substring(usedStart, usedEnd);
                    smsCount = usedStr.toInt();
                    
                    Serial.print("SMS count on SIM card: ");
                    Serial.println(smsCount);
                }
            }
        }
    }
    else
    {
        Serial.println("Failed to query SMS storage status");
        
        // Alternative method: Use AT+CMGL="ALL" and count entries
        modem.sendAT(GF("+CMGL=\"ALL\""));
        String listResponse;
        res = modem.waitResponse(5000, listResponse);
        
        if (res == 1 && listResponse.indexOf("OK") != -1)
        {
            // Count occurrences of "+CMGL:" which indicates individual SMS
            int index = 0;
            while ((index = listResponse.indexOf("+CMGL:", index)) != -1)
            {
                smsCount++;
                index += 6; // Move past "+CMGL:"
            }
            
            Serial.print("SMS count (alternative method): ");
            Serial.println(smsCount);
        }
    }
    
    return smsCount;
}

// True if s is a UCS2 (UTF-16BE) hex string as produced by AT+CSCS="UCS2":
// non-empty, length a multiple of 4, all hex digits. A plain "+49..." number
// fails this (contains '+'), so it is left untouched.
static bool isUcs2Hex(const String &s)
{
    int n = s.length();
    if (n == 0 || (n % 4) != 0) return false;
    for (int i = 0; i < n; i++)
    {
        char c = s[i];
        bool hex = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
        if (!hex) return false;
    }
    return true;
}

static void appendUtf8(String &out, uint32_t cp)
{
    if (cp < 0x80)
    {
        out += (char)cp;
    }
    else if (cp < 0x800)
    {
        out += (char)(0xC0 | (cp >> 6));
        out += (char)(0x80 | (cp & 0x3F));
    }
    else if (cp < 0x10000)
    {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
    else
    {
        out += (char)(0xF0 | (cp >> 18));
        out += (char)(0x80 | ((cp >> 12) & 0x3F));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

// Decode a modem UCS2 hex field to UTF-8. Non-UCS2 input is returned as-is, so
// this is safe to apply to fields that may already be plain text (e.g. a
// numeric sender the modem chose to return verbatim).
static String decodeSmsField(const String &s)
{
    if (!isUcs2Hex(s)) return s;

    String out;
    int n = s.length();
    for (int i = 0; i + 4 <= n; i += 4)
    {
        uint16_t cu = (uint16_t)strtoul(s.substring(i, i + 4).c_str(), nullptr, 16);
        uint32_t cp = cu;
        // Combine a UTF-16 surrogate pair (e.g. emoji) into one code point.
        if (cu >= 0xD800 && cu <= 0xDBFF && i + 8 <= n)
        {
            uint16_t lo = (uint16_t)strtoul(s.substring(i + 4, i + 8).c_str(), nullptr, 16);
            if (lo >= 0xDC00 && lo <= 0xDFFF)
            {
                cp = 0x10000 + (((uint32_t)(cu - 0xD800)) << 10) + (lo - 0xDC00);
                i += 4;
            }
        }
        appendUtf8(out, cp);
    }
    return out;
}

int SimCommunication::readAllSMS()
{
    int count = 0;

    // Read in UCS2 so non-GSM characters (umlauts, emoji, ...) survive: the
    // modem then returns the header fields and body as UTF-16BE hex, which we
    // decode to UTF-8 below. TinyGSM's sendSMS resets CSCS to "GSM", so we
    // re-assert UCS2 on every read.
    modem.sendAT(GF("+CSCS=\"UCS2\""));
    modem.waitResponse();

    // List every stored message in one shot. AT+CMGL returns each message with
    // its real storage index, so we never miss messages sitting in
    // non-contiguous slots (which iterating 1..count would skip).
    modem.sendAT(GF("+CMGL=\"ALL\""));
    String response;
    int8_t res = modem.waitResponse(10000, response);
    if (res != 1)
    {
        // Transient AT failure: leave messages in storage, retry on a later pass.
        Serial.println("Failed to list SMS messages.");
        return count;
    }

    // Each entry looks like:
    //   +CMGL: <index>,"<stat>","<sender>","<alpha>","<timestamp>"
    //   <message body>
    int search = 0;
    while (true)
    {
        feedWatchdog(); // deleting many messages (5 s each) could otherwise add up
        int headerStart = response.indexOf("+CMGL:", search);
        if (headerStart == -1)
        {
            break;
        }

        // Storage index is the first field after "+CMGL:".
        int idxStart = headerStart + 6;
        int idxEnd = response.indexOf(",", idxStart);
        if (idxEnd == -1)
        {
            break; // malformed header, stop parsing
        }
        int smsIndex = response.substring(idxStart, idxEnd).toInt();

        // Sender is the second quoted field (quotes 3 and 4 on the header line).
        String sender = "";
        int q1 = response.indexOf("\"", idxEnd);
        int q2 = response.indexOf("\"", q1 + 1);
        int q3 = response.indexOf("\"", q2 + 1);
        int q4 = response.indexOf("\"", q3 + 1);
        if (q1 != -1 && q2 != -1 && q3 != -1 && q4 != -1)
        {
            sender = decodeSmsField(response.substring(q3 + 1, q4));
        }

        // Body spans from the line after the header to the next entry (or OK).
        String message = "";
        int bodyStart = response.indexOf("\n", headerStart);
        int nextHeader = -1;
        if (bodyStart != -1)
        {
            bodyStart += 1;
            nextHeader = response.indexOf("+CMGL:", bodyStart);
            int bodyEnd;
            if (nextHeader != -1)
            {
                bodyEnd = nextHeader;
            }
            else
            {
                int okIndex = response.indexOf("\nOK", bodyStart);
                bodyEnd = (okIndex != -1) ? okIndex : (int)response.length();
            }
            message = response.substring(bodyStart, bodyEnd);
            message.trim();
            message = decodeSmsField(message);
        }

        if (sender.length() > 0 || message.length() > 0)
        {
            Serial.print("SMS from ");
            Serial.print(sender);
            Serial.print(": ");
            Serial.println(message);
            smsQueue.push_back(sender + ":" + message);
            count++;
        }

        // Delete by real storage index regardless of how well the body parsed,
        // so an unparseable message can never block the queue.
        modem.sendAT("+CMGD=", smsIndex);
        modem.waitResponse(5000);

        // Advance past this entry; stop if there is no further header.
        if (nextHeader == -1)
        {
            break;
        }
        search = nextHeader;
    }

    return count;
}

// private methods

void SimCommunication::powerUpModem()
{
    // Turn on DC boost to power on the modem
#ifdef BOARD_POWERON_PIN
    pinMode(BOARD_POWERON_PIN, OUTPUT);
    digitalWrite(BOARD_POWERON_PIN, HIGH);
#endif

    // Set modem reset pin ,reset modem
    pinMode(MODEM_RESET_PIN, OUTPUT);
    digitalWrite(MODEM_RESET_PIN, !MODEM_RESET_LEVEL);
    delay(100);
    digitalWrite(MODEM_RESET_PIN, MODEM_RESET_LEVEL);
    delay(2600);
    digitalWrite(MODEM_RESET_PIN, !MODEM_RESET_LEVEL);

    // Turn on modem
    pinMode(BOARD_PWRKEY_PIN, OUTPUT);
    digitalWrite(BOARD_PWRKEY_PIN, LOW);
    delay(100);
    digitalWrite(BOARD_PWRKEY_PIN, HIGH);
    delay(100);
    digitalWrite(BOARD_PWRKEY_PIN, LOW);

    // Set ring pin input
    pinMode(MODEM_RING_PIN, INPUT_PULLUP);
}

bool SimCommunication::startModem()
{
    // Check if the modem is online
    Serial.println("Start modem...");

    uint32_t start = millis();
    int retry = 0;
    while (!modem.testAT(1000))
    {
        feedWatchdog();
        Serial.println(".");
        if (millis() - start > MODEM_START_TIMEOUT_MS)
        {
            Serial.println("Modem not responding to AT within timeout.");
            return false;
        }
        if (retry++ > 10)
        {
            digitalWrite(BOARD_PWRKEY_PIN, LOW);
            delay(100);
            digitalWrite(BOARD_PWRKEY_PIN, HIGH);
            delay(1000);
            digitalWrite(BOARD_PWRKEY_PIN, LOW);
            retry = 0;
        }
    }

    Serial.println("Init success");

    Serial.print("Model Name:");
    Serial.println(modem.getModemName());
    return true;
}

bool SimCommunication::isSimCardOnline()
{
    uint32_t start = millis();
    while (true)
    {
        feedWatchdog();
        SimStatus sim = modem.getSimStatus();
        if (sim == SIM_READY)
        {
            Serial.println("SIM card online");
            return true;
        }
        if (sim == SIM_LOCKED)
        {
            Serial.println("The SIM card is locked. Please unlock the SIM card first.");
            // const char *SIMCARD_PIN_CODE = "123456";
            // modem.simUnlock(SIMCARD_PIN_CODE);
        }
        if (millis() - start > SIM_READY_TIMEOUT_MS)
        {
            Serial.println("SIM card not ready within timeout.");
            return false;
        }
        delay(1000);
    }
}

void SimCommunication::setNetworkMode()
{
    if (!modem.setNetworkMode(MODEM_NETWORK_AUTO))
    {
        Serial.println("Set network mode failed!");
    }

    String mode = modem.getNetworkModes();
    Serial.print("Current network mode : ");
    Serial.println(mode);
}

void SimCommunication::setNetworkApn()
{
    Serial.printf("Set network apn : %s\n", NETWORK_APN);
    modem.sendAT(GF("+CGDCONT=1,\"IP\",\""), NETWORK_APN, "\"");
    if (modem.waitResponse() != 1)
    {
        Serial.println("Set network apn error !");
    }
}

bool SimCommunication::awaitNetworkRegistration()
{
    // Check network registration status and network signal status
    int16_t sq;
    Serial.print("Wait for the modem to register with the network.");
    uint32_t start = millis();
    RegStatus status = REG_NO_RESULT;
    while (status == REG_NO_RESULT || status == REG_SEARCHING || status == REG_UNREGISTERED)
    {
        feedWatchdog();
        status = modem.getRegistrationStatus();
        switch (status)
        {
        case REG_UNREGISTERED:
        case REG_SEARCHING:
            sq = modem.getSignalQuality();
            Serial.printf("[%lu] Signal Quality:%d\n", millis() / 1000, sq);
            delay(1000);
            break;
        case REG_DENIED:
            Serial.println("Network registration was rejected, please check if the APN is correct");
            return false;
        case REG_OK_HOME:
            Serial.println("Online registration successful");
            break;
        case REG_OK_ROAMING:
            Serial.println("Network registration successful, currently in roaming mode");
            break;
        default:
            Serial.printf("Registration Status:%d\n", status);
            delay(1000);
            break;
        }

        if (millis() - start > NET_REG_TIMEOUT_MS)
        {
            Serial.println("\nNetwork registration timed out.");
            return false;
        }
    }
    Serial.println();
    Serial.printf("Registration Status:%d\n", status);

    String ueInfo;
    if (modem.getSystemInformation(ueInfo))
    {
        Serial.print("Inquiring UE system information:");
        Serial.println(ueInfo);
    }

    if (!modem.setNetworkActive())
    {
        Serial.println("Enable network failed!");
    }

    // Point the modem at public DNS: the carrier/APN resolver was unreliable
    // (CDNSGIP "0,10" / CIPOPEN ",11"). Set after the data context is open.
    Serial.printf("Set DNS servers: %s / %s\n", NETWORK_DNS_PRIMARY, NETWORK_DNS_SECONDARY);
    modem.sendAT("+CDNSCFG=\"", NETWORK_DNS_PRIMARY, "\",\"", NETWORK_DNS_SECONDARY, "\"");
    if (modem.waitResponse() != 1)
    {
        Serial.println("Set DNS failed (modem may not support AT+CDNSCFG).");
    }
    return true;
}
