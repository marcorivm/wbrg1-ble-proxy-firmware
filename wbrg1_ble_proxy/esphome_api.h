#pragma once

#include <stdint.h>
#include <stddef.h>

#include "advert.h"
#include "BLEDevice.h"
#include "FreeRTOS.h"
#include "semphr.h"
#ifdef __cplusplus
extern "C" {
#endif
#include "profile_client.h"
#ifdef __cplusplus
}
#endif

// Minimal ESPHome native-API server (plaintext framing) so Home Assistant's
// ESPHome integration adopts this device. Phase 1: handshake + device info +
// empty entity list + ping keepalive. Bluetooth-proxy messages come later.
//
// Framing (plaintext): 0x00, varint(payload_len), varint(msg_type), payload.
//
// Runs in its own FreeRTOS task using the core's ard_socket.h wrappers (blocking
// accept + recv with a 3 s timeout), so it never touches the BLE scan loop.
class EspHomeApi {
public:
    void begin(uint16_t port, const char *name, const char *mac,
               const char *model);
    bool clientConnected() const { return _cliFd >= 0; }
    bool btSubscribed() const { return _btSub; }
    bool stateSubscribed() const { return _stateSub; }
    uint32_t advertsSent() const { return _advSent; }

    // Fed by the main loop; streamed to HA as raw adverts when subscribed.
    void pushAdvert(const Advert &a) { if (_btSub) _adv.push(a); }

    // Called from loop(): executes any pending BLE control op in the loop task
    // context (BLE stop-scan/connect must NOT run from the API task).
    void serviceBleOp();

    // Call once from setup() (loop task) after BLE.beginCentral(): does the
    // SDK client_init + registers our GATT client callbacks. Returns false if
    // the SDK refused the registration.
    bool initGatt();
    bool gattReady() const { return _gattOk; }

    // internal (public so the task trampoline can reach it)
    void taskRun();

    // Diagnostic telemetry as native HA sensors (replaces the MQTT telemetry).
    // Called from loop(): stores the latest values; the API task sends them.
    void setTelemetry(int rssi, uint32_t heap, uint32_t uptime,
                      uint32_t adverts, uint32_t drops);

private:
    void serveClient();
    void flushAdverts();
    void closeClient();
    // BLE connections (proxy)
    void handleDeviceRequest(const uint8_t *payload, size_t len);
    void sendConnResponse(uint64_t address, bool connected, uint32_t mtu, int32_t err);
    void sendConnectionsFree();
    int findConn(uint64_t address);
    int findConnByConnId(int connId);
    int freeConnSlot();
    // GATT (raw SDK client)
    void handleGetServices(const uint8_t *payload, size_t len);
public:
    // SDK client callbacks (routed from static trampolines to the singleton)
    void onDiscState(uint8_t conn_id, T_DISCOVERY_STATE st);
    void onDiscResult(uint8_t conn_id, T_DISCOVERY_RESULT_TYPE t, T_DISCOVERY_RESULT_DATA d);
    void onDisconnect(uint8_t conn_id);
    void onReadResult(uint8_t conn_id, uint16_t cause, uint16_t handle, uint16_t size, uint8_t *val);
    void onWriteResult(uint8_t conn_id, uint16_t cause, uint16_t handle);
    void onNotifyInd(uint8_t conn_id, bool notify, uint16_t handle, uint16_t size, uint8_t *val);
private:
    void handleFrame(uint32_t msgType, const uint8_t *payload, size_t len);
    void sendMessage(uint32_t msgType, const uint8_t *payload, size_t len);
    void sendEmpty(uint32_t msgType);
    void sendListEntities();        // advertise the telemetry sensors to HA
    void sendSensorStates();        // push current telemetry values
    void sendScannerState();        // report active-scanning state to HA (msg 126)
    static const int NTELEM = 5;    // rssi, heap, uptime, adverts, drops
    volatile float _telem[NTELEM] = {0, 0, 0, 0, 0};
    bool _stateSub = false;         // HA subscribed to entity states
    unsigned long _lastTelemSend = 0;

    uint16_t _port = 0;
    int _srvFd = -1;
    int _cliFd = -1;

    static const size_t RXCAP = 1024;
    uint8_t _rx[RXCAP];
    size_t _rxLen = 0;

    const char *_name = "";
    const char *_mac = "";
    const char *_model = "";

    volatile bool _btSub = false;   // HA subscribed to BLE advertisements
    uint32_t _advSent = 0;          // raw adverts streamed to HA (diagnostic)
    AdvertQueue _adv;               // main loop -> API task

    static const int MAX_CONN = 3;
    struct Conn {
        bool used = false;
        uint64_t address = 0;
        int8_t connId = -1;
        BLEClient *client = nullptr;
    } _conns[MAX_CONN];

    // discovered GATT table (for the connection currently being discovered)
    static const int MAX_SVC = 12, MAX_CHR = 48, MAX_DSC = 48;
    struct GSvc { uint16_t start, end; uint8_t uuid[16]; uint8_t ulen; };
    struct GChr { uint16_t decl, value, props; uint8_t uuid[16]; uint8_t ulen; };
    struct GDsc { uint16_t handle; uint8_t uuid[16]; uint8_t ulen; };
    GSvc _svc[MAX_SVC]; int _nsvc = 0;
    GChr _chr[MAX_CHR]; int _nchr = 0;
    GDsc _dsc[MAX_DSC]; int _ndsc = 0;
    volatile int _discState = 0;   // 0 idle, 2 done, 3 failed
    int _discConnId = -1;
    SemaphoreHandle_t _discSem = nullptr;
    uint8_t _gattClientId = 0xff;
    bool _gattOk = false;
    volatile uint8_t _discMask = 0;     // conn_ids disconnected by the stack, awaiting loop()/API-task handling
    volatile bool _resumeScan = false;  // loop(): restart scanning (set from BLE ctx)
    void serviceDisconnects();          // API task: send responses for dropped links

    // GATT read/write: API task blocks on _rwSem; result callbacks signal
    volatile uint16_t _rwCause = 0xFFFF;
    volatile uint16_t _rwLen = 0;
    uint16_t _rwHandle = 0;
    int _rwConnId = -1;
    uint8_t _rwBuf[256];
    SemaphoreHandle_t _rwSem = nullptr;
    // notifications/indications: BLE ctx -> ring -> API task -> msg 79
    struct Ntf { uint8_t connId; uint16_t handle; uint8_t len; uint8_t data[64]; };
    static const int NTF_CAP = 8;
    Ntf _ntf[NTF_CAP];
    volatile int _ntfHead = 0;
    volatile int _ntfTail = 0;
    uint32_t _ntfDropped = 0;
    void handleGattRead(const uint8_t *payload, size_t len);
    void handleGattWrite(const uint8_t *payload, size_t len, bool isDescriptor);
    void handleGattNotify(const uint8_t *payload, size_t len);
    void drainNotifies();
    void sendGattError(uint64_t addr, uint32_t handle, int32_t err);

    // BLE control ops marshalled from the API task to loop()
    volatile int _pendOp = 0;          // 0 none, 1 connect, 2 disconnect
    volatile uint64_t _pendAddr = 0;
    volatile uint8_t _pendType = 0;
    volatile int _pendConnId = -1;
    volatile bool _pendOk = false;
    SemaphoreHandle_t _opSem = nullptr;
    void execConnectLoop();
    void execDisconnectLoop();
};
