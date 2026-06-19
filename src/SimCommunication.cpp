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

bool ringDetected()
{
    return digitalRead(MODEM_RING_PIN) == LOW;
}

// How often check() queries the modem for the live call state. Polling is
// throttled so we don't flood the AT UART on every loop iteration.
static const unsigned long CALL_POLL_INTERVAL_MS = 500;

sim_com_check_result SimCommunication::check()
{
    // Nothing to poll if the modem never came up (degraded mode).
    if (!modemReady)
    {
        return {SIM_COM_NOTHING, ""};
    }

    // The RI pin pulses on an incoming SMS notification; when asserted, drain
    // any messages stored on the SIM.
    if (ringDetected())
    {
        String smsContent = readAllSMS();
        if (smsContent.length() > 0)
        {
            Serial.println("Incoming SMS detected.");
            return {SIM_COM_SMS, smsContent};
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

String SimCommunication::readAllSMS()
{
    String smsList = "";

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
        return smsList;
    }

    // Each entry looks like:
    //   +CMGL: <index>,"<stat>","<sender>","<alpha>","<timestamp>"
    //   <message body>
    int search = 0;
    while (true)
    {
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
            sender = response.substring(q3 + 1, q4);
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
        }

        if (sender.length() > 0 || message.length() > 0)
        {
            Serial.print("SMS from ");
            Serial.print(sender);
            Serial.print(": ");
            Serial.println(message);
            smsList += sender + ":" + message + "\n";
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

    return smsList;
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
    return true;
}
