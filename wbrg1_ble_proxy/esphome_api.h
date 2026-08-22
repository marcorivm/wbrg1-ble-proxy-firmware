#pragma once

#include <stdint.h>
#include <stddef.h>

#include "advert.h"
#include "BLEDevice.h"

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

    // Fed by the main loop; streamed to HA as raw adverts when subscribed.
    void pushAdvert(const Advert &a) { if (_btSub) _adv.push(a); }

    // internal (public so the task trampoline can reach it)
    void taskRun();

private:
    void serveClient();
    void flushAdverts();
    void closeClient();
    // BLE connections (proxy)
    void handleDeviceRequest(const uint8_t *payload, size_t len);
    void sendConnResponse(uint64_t address, bool connected, uint32_t mtu, int32_t err);
    void sendConnectionsFree();
    int findConn(uint64_t address);
    int freeConnSlot();
    void handleFrame(uint32_t msgType, const uint8_t *payload, size_t len);
    void sendMessage(uint32_t msgType, const uint8_t *payload, size_t len);
    void sendEmpty(uint32_t msgType);

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
    AdvertQueue _adv;               // main loop -> API task

    static const int MAX_CONN = 3;
    struct Conn {
        bool used = false;
        uint64_t address = 0;
        int8_t connId = -1;
        BLEClient *client = nullptr;
    } _conns[MAX_CONN];
};
