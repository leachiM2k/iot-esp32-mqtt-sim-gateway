#include <Arduino.h>
#include <constants.h>
#include <StreamDebugger.h>
#include <TinyGsmClient.h>
#include "SimCommunication.h"

// public methods

void SimCommunication::init()
{
    SerialAT.begin(115200, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);
    currentCallStatus = NO_CALL;

    powerUpModem();
    startModem();

    modem.sendAT("+SIMCOMATI");
    modem.waitResponse();

    // Wait PB DONE
    Serial.println("Wait SMS Done.");
    if (!modem.waitResponse(100000UL, "SMS DONE")) {
        Serial.println("Can't wait from sms register ....");
        return;
    }

    isSimCardOnline();
    setNetworkMode();
    setNetworkApn();
    awaitNetworkRegistration();
    
    // wait until MODEM_RING_PIN is HIGH to avoid false ring detection at startup
    while (digitalRead(MODEM_RING_PIN) == LOW)
    {
        Serial.println("Waiting for RING pin to go HIGH...");
        delay(100);
    }
    Serial.println("RING pin is HIGH, proceeding with initialization.");

    // Enable SMS notifications
    // modem.sendAT("+CNMI=1,2,0,0,0"); // Enable SMS notifications to terminal
    // modem.waitResponse();
    
    // Set SMS text mode
    modem.sendAT("+CMGF=1"); // Set SMS text mode (not PDU mode)
    modem.waitResponse();
    
    // Configure SMS to be stored in SIM memory instead of being forwarded directly
    // Format: AT+CNMI=<mode>,<mt>,<bm>,<ds>,<bfr>
    // mode=1: buffer unsolicited result codes in TA when TA-TE link is reserved
    // mt=1: SMS-DELIVER is stored and indication is sent to TE
    // bm=0: no SMS-STATUS-REPORTs are routed
    // ds=0: no SMS-STATUS-REPORTs are routed  
    // bfr=0: buffer is flushed when mode 1 is entered
    modem.sendAT("+CNMI=1,1,0,0,0"); // Store SMS and send notification
    modem.waitResponse();
    
    // Set preferred SMS storage to SIM card
    modem.sendAT("+CPMS=\"SM\",\"SM\",\"SM\""); // Use SIM memory for read, write, and receive
    modem.waitResponse();
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
    
    int smsCount = getSMSCount();
    if (smsCount == 0)
    {
        Serial.println("No SMS messages to read.");
        return smsList;
    }

    // Read each SMS and print content
    int index = 1;
    String smsContent = "";

    for (int index = 1; index <= smsCount; index++)
    {
        smsContent = readSMS(index);
        if (smsContent.length() > 0)
        {
            Serial.println("SMS Content:");
            Serial.println(smsContent);
            // append to smsList
            smsList += smsContent + "\n";
        }
    }
    
    return smsList;
}

String SimCommunication::readSMS(int index)
{
    String smsContent = "";

    // Use AT+CMGR command to read SMS at specific index
    modem.sendAT("+CMGR=", index);
    String response;
    int8_t res = modem.waitResponse(5000, response);

    if (res != 1)
    {
        // Transient AT failure: leave the message in storage and retry on a
        // later pass. Do NOT delete — we never actually retrieved it.
        Serial.print("Failed to read SMS at index ");
        Serial.println(index);
        return smsContent;
    }

    // A successful response without a +CMGR: line means the slot is empty
    // (storage indices are not contiguous after deletions). Nothing to do.
    int cmgrIndex = response.indexOf("+CMGR:");
    if (cmgrIndex == -1)
    {
        return smsContent;
    }

    // A message is present. Best-effort parse of the header and body.
    // Format: +CMGR: "status","sender","","timestamp"
    // Message content follows on the next line.
    String sender = "";
    String message = "";

    // Sender is the second quoted field in the header.
    int firstQuote = response.indexOf("\"", cmgrIndex);
    int secondQuote = response.indexOf("\"", firstQuote + 1);
    int thirdQuote = response.indexOf("\"", secondQuote + 1);
    int fourthQuote = response.indexOf("\"", thirdQuote + 1);

    if (firstQuote != -1 && secondQuote != -1 && thirdQuote != -1 && fourthQuote != -1)
    {
        sender = response.substring(thirdQuote + 1, fourthQuote);

        // Message content is on the line after the header.
        int newlineIndex = response.indexOf("\n", fourthQuote);
        if (newlineIndex != -1)
        {
            int messageStart = newlineIndex + 1;
            int okIndex = response.indexOf("\nOK", messageStart);
            if (okIndex == -1)
            {
                okIndex = response.length(); // fall back to end of buffer
            }
            message = response.substring(messageStart, okIndex);
            message.trim();
        }
    }

    if (sender.length() > 0 || message.length() > 0)
    {
        // Format: "sender:message"
        smsContent = sender + ":" + message;

        Serial.print("SMS from ");
        Serial.print(sender);
        Serial.print(": ");
        Serial.println(message);
    }
    else
    {
        Serial.print("Could not parse SMS at index ");
        Serial.print(index);
        Serial.println(", deleting to avoid blocking the queue");
    }

    // Always delete a message we actually retrieved, even if parsing was
    // incomplete. Otherwise an unparseable message would be re-read forever
    // and block every message behind it.
    modem.sendAT("+CMGD=", index);
    modem.waitResponse(5000);

    return smsContent;
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

void SimCommunication::startModem()
{
    // Check if the modem is online
    Serial.println("Start modem...");

    int retry = 0;
    while (!modem.testAT(1000))
    {
        Serial.println(".");
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
}

void SimCommunication::isSimCardOnline()
{
    SimStatus sim = SIM_ERROR;
    while (sim != SIM_READY)
    {
        sim = modem.getSimStatus();
        switch (sim)
        {
        case SIM_READY:
            Serial.println("SIM card online");
            break;
        case SIM_LOCKED:
            Serial.println("The SIM card is locked. Please unlock the SIM card first.");
            // const char *SIMCARD_PIN_CODE = "123456";
            // modem.simUnlock(SIMCARD_PIN_CODE);
            break;
        default:
            break;
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

void SimCommunication::awaitNetworkRegistration()
{
    // Check network registration status and network signal status
    int16_t sq;
    Serial.print("Wait for the modem to register with the network.");
    RegStatus status = REG_NO_RESULT;
    while (status == REG_NO_RESULT || status == REG_SEARCHING || status == REG_UNREGISTERED)
    {
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
            return;
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
}
