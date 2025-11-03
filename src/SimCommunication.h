#ifndef SIMCOMMUNICATION_H
#define SIMCOMMUNICATION_H

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
    void init();
    sim_com_check_result check();
    String getCallStatus();
    void makeCall(const char *number);
    bool acceptCall();
    bool hangupCall();
    void sendSMS(const char *number, const char *message);
    void sendUSSD(const char *ussd);

private:
    void powerUpModem();
    void startModem();
    void isSimCardOnline();
    void setNetworkMode();
    void setNetworkApn();
    void awaitNetworkRegistration();

    // Add private members and methods as needed
    StreamDebugger debugger = StreamDebugger(SerialAT, Serial);
    TinyGsm modem = TinyGsm(debugger);
    current_call_status currentCallStatus = NO_CALL;
};

#endif // SIMCOMMUNICATION_H