#include <Arduino.h>
#include <constants.h>
#include <StreamDebugger.h>
#include <TinyGsmClient.h>
#include "SimCommunication.h"

// public methods

void SimCommunication::init()
{
    SerialAT.begin(115200, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);
    powerUpModem();
    startModem();

    modem.sendAT("+SIMCOMATI");
    modem.waitResponse();

    isSimCardOnline();
    setNetworkMode();
    setNetworkApn();
    awaitNetworkRegistration();
    
    // Enable SMS notifications
    modem.sendAT("+CNMI=1,2,0,0,0"); // Enable SMS notifications to terminal
    modem.waitResponse();
    
    // Set SMS text mode
    modem.sendAT("+CMGF=1"); // Set SMS text mode (not PDU mode)
    modem.waitResponse();
}

bool ringDetected()
{
    return digitalRead(MODEM_RING_PIN) == LOW;
}

sim_com_check_result SimCommunication::check()
{
    // Check for incoming SMS
    String response;
    if (modem.stream.available())
    {
        response = modem.stream.readString();
        Serial.print("Modem response: ");
        Serial.println(response);
        
        // Check for SMS notification (+CMTI)
        int cmtiIndex = response.indexOf("+CMTI:");
        if (cmtiIndex != -1)
        {
            Serial.println("SMS received notification detected");
            
            // Extract SMS index from +CMTI response
            int commaIndex = response.indexOf(",", cmtiIndex);
            if (commaIndex != -1)
            {
                String smsIndexStr = response.substring(commaIndex + 1);
                smsIndexStr.trim();
                int smsIndex = smsIndexStr.toInt();
                
                // Read the SMS content
                String smsContent = readSMS(smsIndex);
                if (smsContent.length() > 0)
                {
                    return {SIM_COM_SMS, smsContent};
                }
            }
        }
    }

    if (ringDetected())
    {
        if (currentCallStatus != RINGING)
        {
            Serial.print("Incoming call...");

            int retry = 0;

            while (retry++ < 10)
            {
                modem.sendAT(GF("+CLCC"));
                String data;
                int8_t res = modem.waitResponse(1000, data);
                if (data == "")
                {
                    delay(100);
                    continue;
                }

                if (res == 1)
                {
                    int index = data.indexOf("+CLCC: ");
                    if (index == -1)
                    {
                        delay(100);
                        continue;
                    }
                    int start = data.indexOf("\"", index) + 1;
                    int end = data.indexOf("\"", start);
                    String callingNumber = data.substring(start, end);
                    Serial.print("Calling number: ");
                    Serial.println(callingNumber);

                    currentCallStatus = RINGING;

                    return {SIM_COM_CALL, callingNumber};
                }
                else
                {
                    Serial.println("No response");
                }
            }
        }
    }
    else
    {
        if(currentCallStatus == RINGING)
        {
            Serial.println("Resetting call status to NO_CALL");
            currentCallStatus = NO_CALL;
            return {SIM_COM_CALL_UPDATE, ""};
        }
    }

    return {SIM_COM_NOTHING, ""};
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
}

String SimCommunication::readSMS(int index)
{
    String smsContent = "";
    
    // Use AT+CMGR command to read SMS at specific index
    modem.sendAT("+CMGR=", index);
    String response;
    int8_t res = modem.waitResponse(5000, response);
    
    if (res == 1 && response.indexOf("OK") != -1)
    {
        // Parse the SMS response
        // Format: +CMGR: "status","sender","","timestamp"
        // Message content follows on next line
        
        int cmgrIndex = response.indexOf("+CMGR:");
        if (cmgrIndex != -1)
        {
            // Find the sender number (second quoted string)
            int firstQuote = response.indexOf("\"", cmgrIndex);
            int secondQuote = response.indexOf("\"", firstQuote + 1);
            int thirdQuote = response.indexOf("\"", secondQuote + 1);
            int fourthQuote = response.indexOf("\"", thirdQuote + 1);
            
            if (firstQuote != -1 && secondQuote != -1 && thirdQuote != -1 && fourthQuote != -1)
            {
                String sender = response.substring(thirdQuote + 1, fourthQuote);
                
                // Find the message content (after the header line)
                int newlineIndex = response.indexOf("\n", fourthQuote);
                if (newlineIndex != -1)
                {
                    int messageStart = newlineIndex + 1;
                    int okIndex = response.indexOf("\nOK", messageStart);
                    if (okIndex != -1)
                    {
                        String message = response.substring(messageStart, okIndex);
                        message.trim();
                        
                        // Format: "sender:message"
                        smsContent = sender + ":" + message;
                        
                        Serial.print("SMS from ");
                        Serial.print(sender);
                        Serial.print(": ");
                        Serial.println(message);
                        
                        // Delete the SMS after reading to prevent memory issues
                        modem.sendAT("+CMGD=", index);
                        modem.waitResponse(5000);
                    }
                }
            }
        }
    }
    else
    {
        Serial.print("Failed to read SMS at index ");
        Serial.println(index);
    }
    
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
