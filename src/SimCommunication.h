#ifndef SIMCOMMUNICATION_H
#define SIMCOMMUNICATION_H

#include <Arduino.h>
// constants.h defines TINY_GSM_MODEM_A7670 and SerialAT, and must be included
// before TinyGsmClient.h (which #errors without a modem model). Keeping these
// includes here makes the header self-contained for any translation unit.
#include <constants.h>
#include <StreamDebugger.h>
#include <TinyGsmClient.h>
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

typedef struct {
    bool fix;        // true if a 2D/3D position fix was obtained
    double lat;      // decimal degrees (negative = south)
    double lon;      // decimal degrees (negative = west)
    float altitude;  // meters (WGS84 ellipsoidal height, as the modem reports it)
    int satellites;  // GPS satellites used
    float hdop;      // horizontal dilution of precision (lower = better; <1 ideal)
} gps_result;

typedef struct {
    bool enabled;        // true = VoLTE preferred (CEVDP 3/4)
    int cevdp;           // raw voice domain preference (1=CS only … 3=IMS pref, 4=IMS only)
    bool imsAvailable;   // +CAVIMS (network offers IMS voice)
    bool imsRegistered;  // +CIREG (device registered to IMS)
} volte_status;

typedef struct {
    String oper;   // network operator (name or numeric MCC-MNC)
    String mode;   // access technology: LTE / WCDMA / GSM
    int signal;    // signal quality (CSQ 0..31, 99 = unknown)
    String band;   // radio band, e.g. "EUTRAN-BAND1" (from +CPSI), "" if unknown
} network_info;

class SimCommunication
{
public:
    // Returns true once the modem is fully initialized and ready. On failure
    // the caller can keep running in a degraded (modem-less) mode.
    bool init();
    bool isModemReady() const;
    // Modem model string (e.g. "A7670E-FASE"); empty if the modem isn't ready.
    String getModemName();
    // Modem IMEI, cached at init (empty if the modem isn't ready).
    String getImei();
    // Live radio/network info (operator, access tech, signal, band). Issues a
    // few AT commands; call from the loop task.
    network_info readNetworkInfo();

    // --- Data path (for MQTT-over-LTE; see Transport/Connectivity) -----------
    // The underlying TinyGsm instance, so an LteTransport can build a
    // TinyGsmClient bound to the same modem. All access stays on the loop task.
    TinyGsm &getModem();
    // True when the modem is registered AND the data context (NETOPEN) is up,
    // i.e. an IP socket can be opened. Issues AT commands: call from loop() only,
    // and throttle (it is not free).
    bool isDataConnected();
    // (Re)open the data context if it dropped (e.g. after a CSFB call on a
    // non-VoLTE network). Returns true if data is available afterwards.
    bool ensureDataConnection();
    // Resolve a hostname to an IP via the modem's DNS (AT+CDNSGIP). The A7670's
    // inline CIPOPEN DNS is unreliable over the mobile APN ("+CIPOPEN: x,11"),
    // so MQTT-over-LTE resolves up front and connects by IP. Loop task only.
    bool resolveHost(const char *host, IPAddress &out);
    // Hard-reset the IP stack (AT+NETCLOSE then AT+NETOPEN). The modem's socket
    // service can wedge after a dropped TCP connection so a fresh CIPOPEN keeps
    // failing ("+CIPOPEN: x,1"); this clears it. Blocks a few seconds; loop only.
    bool resetDataConnection();
    // UTC epoch from the modem's network clock (AT+CCLK, kept current by CTZU).
    // Works on LTE where SNTP/NTP (lwIP/WiFi only) can't reach a time server.
    // Returns 0 if the modem has no valid time yet. Loop task only.
    time_t getNetworkEpochUTC();
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

    // --- GPS (asynchronous, driven from loop() so the modem UART is only ever
    //     touched by one task) ---
    // Request a position fix (just sets a flag; safe to call from any task).
    void requestGps();
    // Power GNSS down to save energy (flag; acted on in updateGps()).
    void powerDownGps();
    // Run the GNSS state machine. Call once per loop() on the loop task.
    void updateGps();
    // True when a fresh GPS result is ready to be published.
    bool gpsResultPending() const;
    // Consume the pending result (clears the pending flag).
    gps_result takeGpsResult();

    // --- VoLTE (voice domain preference), also async via loop() ---
    // Request VoLTE on (IMS preferred) or off (CS/CSFB only). Flag only.
    void requestVolte(bool on);
    // Apply a pending VoLTE change. Call once per loop() on the loop task.
    void updateVolte();
    bool volteResultPending() const;
    volte_status takeVolteStatus();
    // Query the current VoLTE/IMS state directly (blocking AT, use on loop task).
    volte_status readVolteStatus();

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
    String imei; // cached at init; the IMEI never changes

    // GPS state machine (all GNSS modem access happens in updateGps()).
    bool gpsEnabled = false;            // GNSS currently powered
    volatile bool gpsRequested = false; // a fix has been requested
    volatile bool gpsPowerDownRequested = false;
    bool gpsResultReady = false;        // lastGpsResult holds a fresh result
    gps_result lastGpsResult = {false, 0.0, 0.0, 0.0f, 0, 0.0f};
    unsigned long gpsRequestStart = 0;
    unsigned long lastGpsPoll = 0;

    // VoLTE toggle state.
    volatile bool volteChangeRequested = false;
    volatile bool volteTarget = false;
    bool volteResultReady = false;
    volte_status lastVolteStatus = {false, -1, false, false};
    // Pending incoming SMS ("sender:message"), emitted one per check() so each
    // becomes its own MQTT event even when several arrive in one poll.
    std::vector<String> smsQueue;
};

#endif // SIMCOMMUNICATION_H