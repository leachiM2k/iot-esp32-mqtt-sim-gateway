#ifndef SIMCOMMUNICATION_H
#define SIMCOMMUNICATION_H

#include <vector>

typedef enum {
    SIM_COM_NOTHING,
    SIM_COM_CALL,
    SIM_COM_CALL_UPDATE,
    SIM_COM_SMS
} sim_communication_detected;

typedef struct {
    sim_communication_detected event;
    String data;
} sim_com_check_result;

typedef enum {
    NO_CALL,
    CALLING,
    RINGING,
    ESTABLISHED
} current_call_status;

class SimCommunication
{
public:
    // Returns true once the modem is fully initialized and ready. On failure
    // the caller can keep running in a degraded (modem-less) mode.
    bool init();
    bool isModemReady() const;
    // Modem model string (e.g. "A7670E-FASE"); empty if the modem isn't ready.
    String getModemName();
    sim_com_check_result check();
    String getCallStatus();
    void makeCall(const char *number);
    bool acceptCall();
    bool hangupCall();
    void sendSMS(const char *number, const char *message);
    int getSMSCount();
    // Drain all stored SMS into smsQueue; returns the number queued.
    int readAllSMS();
    void sendUSSD(const char *ussd);

private:
    void powerUpModem();
    // The startup steps below are bounded by timeouts and return false on
    // failure instead of blocking forever.
    bool startModem();
    bool isSimCardOnline();
    void setNetworkMode();
    void setNetworkApn();
    bool awaitNetworkRegistration();

    // Poll the modem (+CLCC) for the live call state and report transitions.
    sim_com_check_result updateCallState();
    current_call_status parseCallStatus(const String &clcc, int clccIndex);
    String parseCallNumber(const String &clcc, int clccIndex);

    // Add private members and methods as needed
    StreamDebugger debugger = StreamDebugger(SerialAT, Serial);
    TinyGsm modem = TinyGsm(debugger);
    current_call_status currentCallStatus = NO_CALL;
    bool modemReady = false;
    // Pending incoming SMS ("sender:message"), emitted one per check() so each
    // becomes its own MQTT event even when several arrive in one poll.
    std::vector<String> smsQueue;
};

#endif // SIMCOMMUNICATION_H