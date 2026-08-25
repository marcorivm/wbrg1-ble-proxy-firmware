#include "esphome_api.h"

extern void wdtKick();   // defined in the sketch

#include <Arduino.h>
#include <string.h>
#include <errno.h>

// ard_socket.h declares these without extern "C"; declare them with C linkage
// so they resolve against the C-compiled ard_socket.c symbols.
extern "C" {
int start_server(unsigned short port, unsigned char protMode);
int sock_listen(int sock, int max);
int get_available(int sock);
int recv_data(int sock, const unsigned char *data, unsigned short len, int flag);
int send_data(int sock, const unsigned char *data, unsigned short len, int flag);
int get_sock_errno(int sock);
int set_sock_recv_timeout(int sock, int timeout);
void close_socket(int sock);
}

// ---- protobuf varint helpers -------------------------------------------

static size_t putVarint(uint8_t *buf, size_t pos, uint64_t v) {
    do {
        uint8_t b = v & 0x7F;
        v >>= 7;
        if (v) b |= 0x80;
        buf[pos++] = b;
    } while (v);
    return pos;
}

static bool getVarint(const uint8_t *buf, size_t *idx, size_t end, uint64_t *out) {
    uint64_t v = 0;
    int shift = 0;
    size_t i = *idx;
    while (i < end) {
        uint8_t b = buf[i++];
        v |= (uint64_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) {
            *out = v;
            *idx = i;
            return true;
        }
        shift += 7;
        if (shift > 63) return false;
    }
    return false;
}

// Read a varint field by number from a protobuf message payload.
static bool pbField(const uint8_t *buf, size_t len, uint32_t field, uint64_t *out) {
    size_t i = 0;
    while (i < len) {
        uint64_t tag;
        if (!getVarint(buf, &i, len, &tag)) break;
        uint32_t f = (uint32_t)(tag >> 3), wt = (uint32_t)(tag & 7);
        if (wt == 0) {
            uint64_t v;
            if (!getVarint(buf, &i, len, &v)) break;
            if (f == field) { *out = v; return true; }
        } else if (wt == 2) {
            uint64_t l;
            if (!getVarint(buf, &i, len, &l)) break;
            i += (size_t)l;
        } else if (wt == 5) i += 4;
        else if (wt == 1) i += 8;
        else break;
    }
    return false;
}

static size_t pbString(uint8_t *buf, size_t pos, uint32_t field, const char *s) {
    size_t n = strlen(s);
    buf[pos++] = (uint8_t)((field << 3) | 2);
    pos = putVarint(buf, pos, n);
    memcpy(buf + pos, s, n);
    return pos + n;
}

static size_t pbUint(uint8_t *buf, size_t pos, uint32_t field, uint64_t v) {
    buf[pos++] = (uint8_t)((field << 3) | 0);
    return putVarint(buf, pos, v);
}

// Locate a length-delimited (bytes) field in a protobuf payload.
static bool pbFieldBytes(const uint8_t *buf, size_t len, uint32_t field,
                         const uint8_t **out, size_t *olen) {
    size_t i = 0;
    while (i < len) {
        uint64_t tag;
        if (!getVarint(buf, &i, len, &tag)) break;
        uint32_t f = (uint32_t)(tag >> 3), wt = (uint32_t)(tag & 7);
        if (wt == 0) {
            uint64_t v;
            if (!getVarint(buf, &i, len, &v)) break;
        } else if (wt == 2) {
            uint64_t l;
            if (!getVarint(buf, &i, len, &l)) break;
            if (i + (size_t)l > len) break;
            if (f == field) { *out = buf + i; *olen = (size_t)l; return true; }
            i += (size_t)l;
        } else if (wt == 5) i += 4;
        else if (wt == 1) i += 8;
        else break;
    }
    return false;
}

static size_t pbBytes(uint8_t *buf, size_t pos, uint32_t field, const uint8_t *d, size_t n) {
    buf[pos++] = (uint8_t)((field << 3) | 2);
    pos = putVarint(buf, pos, n);
    memcpy(buf + pos, d, n);
    return pos + n;
}

// protobuf fixed32 (wire type 5): used for sensor key + float state.
static size_t pbFixed32(uint8_t *buf, size_t pos, uint32_t field, uint32_t v) {
    buf[pos++] = (uint8_t)((field << 3) | 5);
    buf[pos++] = (uint8_t)(v & 0xFF);
    buf[pos++] = (uint8_t)((v >> 8) & 0xFF);
    buf[pos++] = (uint8_t)((v >> 16) & 0xFF);
    buf[pos++] = (uint8_t)((v >> 24) & 0xFF);
    return pos;
}
static size_t pbFloat(uint8_t *buf, size_t pos, uint32_t field, float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return pbFixed32(buf, pos, field, u);
}

// Telemetry sensors advertised to HA over the native API (keys 1..NTELEM).
struct TelemSensor {
    const char *object_id;
    const char *name;
    const char *unit;
    const char *device_class;
    uint32_t state_class;     // 1 = measurement, 2 = total_increasing
};
static const TelemSensor kTelem[] = {
    {"wifi_rssi",   "WiFi RSSI",   "dBm", "signal_strength", 1},
    {"free_heap",   "Free heap",   "B",   "data_size",       1},
    {"uptime",      "Uptime",      "s",   "duration",        2},
    {"adverts",     "Adverts seen","",    "",                2},
    {"queue_drops", "Queue drops", "",    "",                2},
};

// ---- raw-SDK GATT client (discovery + read/write/notify) ---------------

static EspHomeApi *g_api = nullptr;   // singleton for the C callbacks

static void cb_disc_state(uint8_t conn_id, T_DISCOVERY_STATE st) {
    if (g_api) g_api->onDiscState(conn_id, st);
}
static void cb_disc_result(uint8_t conn_id, T_DISCOVERY_RESULT_TYPE t,
                           T_DISCOVERY_RESULT_DATA d) {
    if (g_api) g_api->onDiscResult(conn_id, t, d);
}
static void cb_read(uint8_t c, uint16_t cause, uint16_t h, uint16_t sz, uint8_t *v) {
    if (g_api) g_api->onReadResult(c, cause, h, sz, v);
}
static void cb_write(uint8_t c, T_GATT_WRITE_TYPE t, uint16_t h, uint16_t cause, uint8_t credits) {
    (void)t; (void)credits;
    if (g_api) g_api->onWriteResult(c, cause, h);
}
static T_APP_RESULT cb_notify(uint8_t c, bool notify, uint16_t h, uint16_t sz, uint8_t *v) {
    if (g_api) g_api->onNotifyInd(c, notify, h, sz, v);
    return APP_RESULT_SUCCESS;
}
static void cb_disconnect(uint8_t conn_id) { if (g_api) g_api->onDisconnect(conn_id); }

static T_APP_RESULT cb_general(T_CLIENT_ID, uint8_t, void *) { return APP_RESULT_SUCCESS; }

static T_FUN_CLIENT_CBS g_gatt_cbs = {
    cb_disc_state, cb_disc_result, cb_read, cb_write, cb_notify, cb_disconnect,
};

// ---- task ---------------------------------------------------------------

static void apiTaskTrampoline(void *param) {
    ((EspHomeApi *)param)->taskRun();
    vTaskDelete(NULL);
}

void EspHomeApi::begin(uint16_t port, const char *name, const char *mac,
                       const char *model) {
    _port = port;
    _name = name;
    _mac = mac;
    _model = model;
    xTaskCreate(apiTaskTrampoline, "esphome_api", 8192, this,
                tskIDLE_PRIORITY + 1, NULL);
}

void EspHomeApi::taskRun() {
    g_api = this;
    if (_discSem == nullptr) _discSem = xSemaphoreCreateBinary();
    if (_opSem == nullptr) _opSem = xSemaphoreCreateBinary();
    if (_rwSem == nullptr) _rwSem = xSemaphoreCreateBinary();

    _srvFd = start_server(_port, 0);   // TCP
    if (_srvFd < 0) return;
    if (sock_listen(_srvFd, 1) < 0) {
        _srvFd = -1;
        return;
    }
    for (;;) {
        _cliFd = get_available(_srvFd);   // blocks until a client connects
        if (_cliFd < 0) {
            vTaskDelay(100);
            continue;
        }
        _rxLen = 0;
        serveClient();
        closeClient();
    }
}

void EspHomeApi::closeClient() {
    if (_cliFd >= 0) {
        close_socket(_cliFd);
        _cliFd = -1;
    }
    _rxLen = 0;
    _btSub = false;
    _stateSub = false;
}

void EspHomeApi::serveClient() {
    set_sock_recv_timeout(_cliFd, 100);  // loop ~10 Hz to stream adverts
    unsigned long lastRx = millis();
    for (;;) {
        if (_rxLen >= RXCAP) _rxLen = 0;  // wedged; drop
        serviceDisconnects();
        drainNotifies();
        int n = recv_data(_cliFd, _rx + _rxLen, (uint16_t)(RXCAP - _rxLen), 0);
        if (n > 0) {
            _rxLen += (size_t)n;
            // parse complete frames
            size_t consumed = 0;
            while (true) {
                if (_rxLen - consumed < 1) break;
                if (_rx[consumed] != 0x00) {  // desync / noise(0x01) — drop
                    return;
                }
                size_t idx = consumed + 1;
                uint64_t plen, ptype;
                if (!getVarint(_rx, &idx, _rxLen, &plen)) break;
                if (!getVarint(_rx, &idx, _rxLen, &ptype)) break;
                if (_rxLen - idx < plen) break;
                handleFrame((uint32_t)ptype, _rx + idx, (size_t)plen);
                if (_cliFd < 0) return;  // handler closed
                consumed = idx + (size_t)plen;
            }
            if (consumed > 0) {
                memmove(_rx, _rx + consumed, _rxLen - consumed);
                _rxLen -= consumed;
            }
        } else if (n == 0) {
            return;  // peer closed
        } else {
            int err = get_sock_errno(_cliFd);
            if (!(err == EAGAIN || err == EWOULDBLOCK)) return;  // real error
        }
        if (n > 0) lastRx = millis();
        // HA pings every ~20 s; 60 s of silence means the client is gone. Drop
        // it so accept() can serve a fresh connection (HA reconnects).
        if (millis() - lastRx > 60000) return;
        if (_btSub) flushAdverts();
        if (_stateSub && millis() - _lastTelemSend > 15000) sendSensorStates();
    }
}

// Drain queued adverts and stream them as BluetoothLERawAdvertisementsResponse(93).
void EspHomeApi::flushAdverts() {
    Advert a;
    uint8_t buf[1200];
    size_t p = 0;
    int count = 0;
    while (_adv.pop(a)) {
        uint8_t sub[80];
        size_t sp = 0;
        // address (field 1, uint64): MSB-first from Realtek LSB-first bd_addr
        uint64_t address = 0;
        for (int i = 5; i >= 0; i--) address = (address << 8) | a.addr[i];
        sub[sp++] = 0x08;
        sp = putVarint(sub, sp, address);
        // rssi (field 2, sint32 zigzag)
        int32_t r = a.rssi;
        uint32_t zz = ((uint32_t)r << 1) ^ (uint32_t)(r >> 31);
        sub[sp++] = 0x10;
        sp = putVarint(sub, sp, zz);
        // address_type (field 3, uint32)
        sub[sp++] = 0x18;
        sp = putVarint(sub, sp, a.addrType);
        // data (field 4, bytes)
        sub[sp++] = 0x22;
        sp = putVarint(sub, sp, a.dataLen);
        memcpy(sub + sp, a.data, a.dataLen);
        sp += a.dataLen;

        if (p + 2 + sp + 4 > sizeof(buf)) {  // flush partial batch
            sendMessage(93, buf, p);
            p = 0;
            count = 0;
        }
        buf[p++] = 0x0A;                 // repeated field 1, wire type 2
        p = putVarint(buf, p, sp);
        memcpy(buf + p, sub, sp);
        p += sp;
        count++;
        _advSent++;
    }
    if (count > 0) sendMessage(93, buf, p);
}

// loop() context: store the latest telemetry (no socket I/O here).
void EspHomeApi::setTelemetry(int rssi, uint32_t heap, uint32_t uptime,
                              uint32_t adverts, uint32_t drops) {
    _telem[0] = (float)rssi;
    _telem[1] = (float)heap;
    _telem[2] = (float)uptime;
    _telem[3] = (float)adverts;
    _telem[4] = (float)drops;
}

// API task: advertise the telemetry sensors (ListEntitiesSensorResponse=16).
void EspHomeApi::sendListEntities() {
    for (int i = 0; i < NTELEM; i++) {
        char uid[48];
        snprintf(uid, sizeof(uid), "%s_%s", _name, kTelem[i].object_id);
        uint8_t buf[192];
        size_t p = 0;
        p = pbString(buf, p, 1, kTelem[i].object_id);   // object_id
        p = pbFixed32(buf, p, 2, (uint32_t)(i + 1));     // key
        p = pbString(buf, p, 3, kTelem[i].name);         // name
        p = pbString(buf, p, 4, uid);                    // unique_id
        if (kTelem[i].unit[0]) p = pbString(buf, p, 6, kTelem[i].unit);
        p = pbUint(buf, p, 7, 0);                        // accuracy_decimals
        if (kTelem[i].device_class[0]) p = pbString(buf, p, 9, kTelem[i].device_class);
        p = pbUint(buf, p, 10, kTelem[i].state_class);   // state_class
        sendMessage(16, buf, p);
    }
}

// BluetoothScannerStateResponse(126): state=RUNNING(2), mode=ACTIVE(1).
// Tells HA we are actively scanning so the adapter page stops saying
// "No scanning". We always active-scan, so mode/configured_mode are fixed.
void EspHomeApi::sendScannerState() {
    uint8_t buf[12];
    size_t p = 0;
    p = pbUint(buf, p, 1, 2);   // state = BLUETOOTH_SCANNER_STATE_RUNNING
    p = pbUint(buf, p, 2, 1);   // mode = BLUETOOTH_SCANNER_MODE_ACTIVE
    p = pbUint(buf, p, 3, 1);   // configured_mode = ACTIVE
    sendMessage(126, buf, p);
}

// API task: push current sensor values (SensorStateResponse=25).
void EspHomeApi::sendSensorStates() {
    for (int i = 0; i < NTELEM; i++) {
        uint8_t buf[16];
        size_t p = 0;
        p = pbFixed32(buf, p, 1, (uint32_t)(i + 1));     // key
        p = pbFloat(buf, p, 2, _telem[i]);               // state
        sendMessage(25, buf, p);
    }
    _lastTelemSend = millis();
}

void EspHomeApi::sendMessage(uint32_t msgType, const uint8_t *payload,
                             size_t len) {
    if (_cliFd < 0) return;
    uint8_t hdr[16];
    size_t p = 0;
    hdr[p++] = 0x00;
    p = putVarint(hdr, p, len);
    p = putVarint(hdr, p, msgType);
    send_data(_cliFd, hdr, (uint16_t)p, 0);
    if (len) send_data(_cliFd, payload, (uint16_t)len, 0);
}

void EspHomeApi::sendEmpty(uint32_t msgType) {
    sendMessage(msgType, nullptr, 0);
}

void EspHomeApi::handleFrame(uint32_t msgType, const uint8_t *payload,
                             size_t len) {
    (void)payload;
    (void)len;
    uint8_t buf[256];
    size_t p = 0;

    switch (msgType) {
        case 1:  // HelloRequest -> HelloResponse(2)
            p = pbUint(buf, p, 1, 1);
            p = pbUint(buf, p, 2, 10);
            p = pbString(buf, p, 3, "wbrg1-ble-proxy");
            p = pbString(buf, p, 4, _name);
            sendMessage(2, buf, p);
            break;
        case 3:  // ConnectRequest (legacy) -> ConnectResponse(4)
            p = pbUint(buf, p, 1, 0);
            sendMessage(4, buf, p);
            break;
        case 5:  // DisconnectRequest -> DisconnectResponse(6), close
            sendEmpty(6);
            closeClient();
            break;
        case 7:  // PingRequest -> PingResponse(8)
            sendEmpty(8);
            break;
        case 9:  // DeviceInfoRequest -> DeviceInfoResponse(10)
            p = pbString(buf, p, 2, _name);
            p = pbString(buf, p, 3, _mac);
            p = pbString(buf, p, 4, "wbrg1-1.0");
            p = pbString(buf, p, 6, _model);
            p = pbString(buf, p, 12, "Tuya/Realtek");
            p = pbString(buf, p, 13, _name);
            // flags: PASSIVE_SCAN(1)|ACTIVE_CONNECTIONS(2)|RAW_ADVERTISEMENTS(32)
            //        |STATE_AND_MODE(64) so HA shows our scan state, not "No scanning"
            //        |REMOTE_CACHING(4): aioesphomeapi HARD-REFUSES to route GATT
            //        connections through a proxy without it (it then sends the
            //        CONNECT_V3 request types, which handleDeviceRequest accepts).
            p = pbUint(buf, p, 15, 103);
            sendMessage(10, buf, p);
            break;
        case 11:  // ListEntitiesRequest -> sensors, then Done(19)
            sendListEntities();
            sendEmpty(19);
            break;
        case 20:  // SubscribeStatesRequest -> push current states now
            _stateSub = true;
            sendSensorStates();
            break;
        case 66:  // SubscribeBluetoothLEAdvertisementsRequest
            _btSub = true;
            sendScannerState();   // report RUNNING+ACTIVE so HA shows us scanning
            break;
        case 127:  // BluetoothScannerSetModeRequest -> we always active-scan
            sendScannerState();
            break;
        case 87:  // UnsubscribeBluetoothLEAdvertisementsRequest
            _btSub = false;
            break;
        case 80:  // SubscribeBluetoothConnectionsFreeRequest
            sendConnectionsFree();
            break;
        case 68:  // BluetoothDeviceRequest (connect/disconnect)
            handleDeviceRequest(payload, len);
            break;
        case 70:  // BluetoothGATTGetServicesRequest
            handleGetServices(payload, len);
            break;
        case 73:  // BluetoothGATTReadRequest
            handleGattRead(payload, len);
            break;
        case 75:  // BluetoothGATTWriteRequest
            handleGattWrite(payload, len, false);
            break;
        case 76:  // BluetoothGATTReadDescriptorRequest (same shape/reply as read)
            handleGattRead(payload, len);
            break;
        case 77:  // BluetoothGATTWriteDescriptorRequest
            handleGattWrite(payload, len, true);
            break;
        case 78:  // BluetoothGATTNotifyRequest
            handleGattNotify(payload, len);
            break;
        default:  // ignore
            break;
    }
}


// ---- BLE connections (proxy) -------------------------------------------

int EspHomeApi::findConn(uint64_t address) {
    for (int i = 0; i < MAX_CONN; i++)
        if (_conns[i].used && _conns[i].address == address) return i;
    return -1;
}

int EspHomeApi::freeConnSlot() {
    for (int i = 0; i < MAX_CONN; i++)
        if (!_conns[i].used) return i;
    return -1;
}

void EspHomeApi::sendConnectionsFree() {
    int freeN = 0;
    for (int i = 0; i < MAX_CONN; i++) if (!_conns[i].used) freeN++;
    uint8_t buf[64];
    size_t p = 0;
    p = pbUint(buf, p, 1, (uint32_t)freeN);      // free
    p = pbUint(buf, p, 2, (uint32_t)MAX_CONN);   // limit
    for (int i = 0; i < MAX_CONN; i++)           // allocated addresses
        if (_conns[i].used) p = pbUint(buf, p, 3, _conns[i].address);
    sendMessage(81, buf, p);
}

void EspHomeApi::sendConnResponse(uint64_t address, bool connected, uint32_t mtu,
                                  int32_t err) {
    uint8_t buf[32];
    size_t p = 0;
    p = pbUint(buf, p, 1, address);
    p = pbUint(buf, p, 2, connected ? 1 : 0);
    p = pbUint(buf, p, 3, mtu);
    // error is int32 (field 4); non-negative here so plain varint is fine
    p = pbUint(buf, p, 4, (uint32_t)err);
    sendMessage(69, buf, p);
}

void EspHomeApi::handleDeviceRequest(const uint8_t *payload, size_t len) {
    uint64_t address = 0, reqType = 0, addrType = 0;
    pbField(payload, len, 1, &address);
    pbField(payload, len, 2, &reqType);
    pbField(payload, len, 4, &addrType);

    // request_type: 1 = DISCONNECT; 0/4/5 = CONNECT variants
    if (reqType == 1) {
        _pendAddr = address; _pendOp = 2; _pendOk = false;
        xSemaphoreTake(_opSem, pdMS_TO_TICKS(5000));
        sendConnResponse(address, false, 0, 0);
        sendConnectionsFree();
        return;
    }

    // CONNECT
    int idx = findConn(address);
    if (idx < 0) idx = freeConnSlot();
    if (idx < 0) {  // no slots
        sendConnResponse(address, false, 0, -1);
        return;
    }
    // Marshal the actual BLE connect to the loop() task (BLE control calls
    // crash if made from this API task's context).
    _pendAddr = address;
    _pendType = (uint8_t)addrType;
    _pendOk = false;
    _pendConnId = -1;
    _pendOp = 1;
    xSemaphoreTake(_opSem, pdMS_TO_TICKS(12000));  // loop() gives it when done
    if (_pendOk && _pendConnId >= 0) {
        _conns[idx].used = true;
        _conns[idx].address = address;
        _conns[idx].connId = (int8_t)_pendConnId;
        _conns[idx].client = nullptr;
        sendConnResponse(address, true, 247, 0);
    } else {
        sendConnResponse(address, false, 0, -1);
    }
    sendConnectionsFree();
}

bool EspHomeApi::initGatt() {
    g_api = this;
    // The SDK requires client_init() before any spec-client registration
    // (BLEDevice::configClient does the same); without it the registration
    // fails silently and _gattClientId stays invalid.
    client_init(BLE_CENTRAL_APP_MAX_LINKS);
    client_register_general_client_cb(cb_general);
    _gattOk = client_register_spec_client_cb(&_gattClientId, &g_gatt_cbs);
    return _gattOk;
}

// executed in loop() context
void EspHomeApi::serviceBleOp() {
    if (_resumeScan) {
        _resumeScan = false;
        BLE.configScan()->startScan();
    }
    if (_pendOp == 0) return;
    int op = _pendOp;
    if (op == 1) execConnectLoop();
    else if (op == 2) execDisconnectLoop();
    _pendOp = 0;
    if (_opSem) xSemaphoreGive(_opSem);
}

extern "C" void flashClkSafe(void);
void apiLogLine(const char *msg);   // sketch logLine bridge
void cancelPendingConnects();   // sketch: cancels stuck create-connections

void EspHomeApi::execConnectLoop() {
    flashClkSafe();   // slow SPIC before the fragile connect window
    uint8_t bd[6];
    for (int i = 0; i < 6; i++) bd[i] = (uint8_t)((_pendAddr >> (8 * i)) & 0xFF);
    BLEAddr addr(bd);
    BLE.configScan()->stopScan();
    delay(60);
    BLE.configConnection()->connect(addr, (T_GAP_REMOTE_ADDR_TYPE)_pendType, 5000);
    int connId = -1;
    for (int t = 0; t < 60; t++) {
        delay(100);
        wdtKick();
        int8_t cid = BLE.configConnection()->getConnId(bd, _pendType);
        if (cid >= 0 && BLE.connected((uint8_t)cid) && findConnByConnId(cid) < 0) { connId = cid; break; }
        for (uint8_t i = 0; i < MAX_CONN; i++)
            if (BLE.connected(i) && findConnByConnId(i) < 0) { connId = i; break; }
        if (connId >= 0) break;
    }
    if (connId >= 0 && BLE.connected((uint8_t)connId)) {
        _pendConnId = connId;
        _pendOk = true;
    } else {
        _pendOk = false;
        // A timed-out connect stays PENDING in the controller and blocks all
        // scanning until cancelled — startScan() alone cannot revive it.
        cancelPendingConnects();
        delay(150);
        BLE.configScan()->startScan();  // failed; resume scanning
    }
}

void EspHomeApi::execDisconnectLoop() {
    int idx = findConn(_pendAddr);
    if (idx >= 0) {
        BLE.configConnection()->disconnect(_conns[idx].connId);
        _conns[idx] = Conn();
    }
    bool any = false;
    for (int i = 0; i < MAX_CONN; i++) if (_conns[i].used) any = true;
    if (!any) BLE.configScan()->startScan();
    _pendOk = true;
}


// scan must be stopped while a connection is set up (RTL can't do observer +
// connection-setup at once); resume it when no connections remain.
static void resumeScanIfIdle(EspHomeApi *) {}


// ---- GATT read / write / notify (3c) ------------------------------------

void EspHomeApi::onReadResult(uint8_t conn_id, uint16_t cause, uint16_t handle,
                              uint16_t size, uint8_t *val) {
    if ((int)conn_id != _rwConnId || handle != _rwHandle) return;
    uint16_t n = size > sizeof(_rwBuf) ? (uint16_t)sizeof(_rwBuf) : size;
    if (cause == 0 && val) memcpy(_rwBuf, val, n); else n = 0;
    _rwLen = n;
    _rwCause = cause;
    if (_rwSem) xSemaphoreGive(_rwSem);
}

void EspHomeApi::onWriteResult(uint8_t conn_id, uint16_t cause, uint16_t handle) {
    (void)handle;   // some stacks report 0 here; match on connection only
    if ((int)conn_id != _rwConnId) return;
    _rwLen = 0;
    _rwCause = cause;
    if (_rwSem) xSemaphoreGive(_rwSem);
}

// BLE-stack context: copy into the ring and leave.
void EspHomeApi::onNotifyInd(uint8_t conn_id, bool notify, uint16_t handle,
                             uint16_t size, uint8_t *val) {
    if (!notify) client_attr_ind_confirm(conn_id);
    int next = (_ntfHead + 1) % NTF_CAP;
    if (next == _ntfTail) { _ntfDropped++; return; }
    Ntf &n = _ntf[_ntfHead];
    n.connId = conn_id;
    n.handle = handle;
    n.len = size > sizeof(n.data) ? (uint8_t)sizeof(n.data) : (uint8_t)size;
    if (val) memcpy(n.data, val, n.len); else n.len = 0;
    _ntfHead = next;
}

// API task: stream ring entries to HA as BluetoothGATTNotifyDataResponse(79).
void EspHomeApi::drainNotifies() {
    while (_ntfTail != _ntfHead) {
        Ntf &n = _ntf[_ntfTail];
        int idx = findConnByConnId(n.connId);
        if (idx >= 0) {
            uint8_t buf[96];
            size_t p = 0;
            p = pbUint(buf, p, 1, _conns[idx].address);
            p = pbUint(buf, p, 2, n.handle);
            p = pbBytes(buf, p, 3, n.data, n.len);
            sendMessage(79, buf, p);
        }
        _ntfTail = (_ntfTail + 1) % NTF_CAP;
    }
}

void EspHomeApi::sendGattError(uint64_t addr, uint32_t handle, int32_t err) {
    uint8_t buf[40];
    size_t p = 0;
    p = pbUint(buf, p, 1, addr);
    p = pbUint(buf, p, 2, handle);
    p = pbUint(buf, p, 3, (uint32_t)err);
    sendMessage(82, buf, p);
}

void EspHomeApi::handleGattRead(const uint8_t *payload, size_t len) {
    uint64_t address = 0, handle = 0;
    pbField(payload, len, 1, &address);
    pbField(payload, len, 2, &handle);
    int idx = findConn(address);
    if (idx < 0) { sendGattError(address, (uint32_t)handle, -1); return; }
    _rwConnId = _conns[idx].connId;
    _rwHandle = (uint16_t)handle;
    _rwCause = 0xFFFF;
    _rwLen = 0;
    if (_rwSem) xSemaphoreTake(_rwSem, 0);
    if (client_attr_read((uint8_t)_conns[idx].connId, _gattClientId, (uint16_t)handle) != GAP_CAUSE_SUCCESS) {
        sendGattError(address, (uint32_t)handle, -1);
        return;
    }
    if (!_rwSem || xSemaphoreTake(_rwSem, pdMS_TO_TICKS(5000)) != pdTRUE || _rwCause != 0) {
        sendGattError(address, (uint32_t)handle, _rwCause == 0xFFFF ? -2 : (int32_t)_rwCause);
        return;
    }
    uint8_t buf[300];
    size_t p = 0;
    p = pbUint(buf, p, 1, address);
    p = pbUint(buf, p, 2, handle);
    p = pbBytes(buf, p, 3, _rwBuf, _rwLen);
    sendMessage(74, buf, p);
}

void EspHomeApi::handleGattWrite(const uint8_t *payload, size_t len, bool isDescriptor) {
    uint64_t address = 0, handle = 0, response = isDescriptor ? 1 : 0;
    pbField(payload, len, 1, &address);
    pbField(payload, len, 2, &handle);
    if (!isDescriptor) pbField(payload, len, 3, &response);
    const uint8_t *data = nullptr;
    size_t dlen = 0;
    pbFieldBytes(payload, len, isDescriptor ? 3 : 4, &data, &dlen);
    int idx = findConn(address);
    if (idx < 0 || data == nullptr) { sendGattError(address, (uint32_t)handle, -1); return; }
    if (dlen > 244) dlen = 244;
    _rwConnId = _conns[idx].connId;
    _rwHandle = (uint16_t)handle;
    _rwCause = 0xFFFF;
    if (_rwSem) xSemaphoreTake(_rwSem, 0);
    T_GATT_WRITE_TYPE wt = response ? GATT_WRITE_TYPE_REQ : GATT_WRITE_TYPE_CMD;
    if (client_attr_write((uint8_t)_conns[idx].connId, _gattClientId, wt,
                          (uint16_t)handle, (uint16_t)dlen, (uint8_t *)data) != GAP_CAUSE_SUCCESS) {
        sendGattError(address, (uint32_t)handle, -1);
        return;
    }
    if (response) {
        if (!_rwSem || xSemaphoreTake(_rwSem, pdMS_TO_TICKS(5000)) != pdTRUE || _rwCause != 0) {
            sendGattError(address, (uint32_t)handle, _rwCause == 0xFFFF ? -2 : (int32_t)_rwCause);
            return;
        }
    }
    uint8_t buf[32];
    size_t p = 0;
    p = pbUint(buf, p, 1, address);
    p = pbUint(buf, p, 2, handle);
    sendMessage(83, buf, p);
}

void EspHomeApi::handleGattNotify(const uint8_t *payload, size_t len) {
    uint64_t address = 0, handle = 0, enable = 0;
    pbField(payload, len, 1, &address);
    pbField(payload, len, 2, &handle);
    pbField(payload, len, 3, &enable);
    int idx = findConn(address);
    if (idx < 0) { sendGattError(address, (uint32_t)handle, -1); return; }
    // characteristic properties (notify vs indicate), from the discovery table
    uint16_t props = 0;
    for (int ci = 0; ci < _nchr; ci++)
        if (_chr[ci].value == (uint16_t)handle) { props = _chr[ci].props; break; }
    // the char's CCCD: first 0x2902 descriptor after the value handle, bounded
    // by the next characteristic declaration
    uint16_t bound = 0xFFFF;
    for (int ci = 0; ci < _nchr; ci++)
        if (_chr[ci].decl > (uint16_t)handle && _chr[ci].decl < bound) bound = _chr[ci].decl;
    uint16_t cccd = 0xFFFF;
    for (int di = 0; di < _ndsc; di++) {
        if (_dsc[di].ulen == 2 && _dsc[di].uuid[0] == 0x02 && _dsc[di].uuid[1] == 0x29 &&
            _dsc[di].handle > (uint16_t)handle && _dsc[di].handle < bound &&
            _dsc[di].handle < cccd)
            cccd = _dsc[di].handle;
    }
    if (cccd == 0xFFFF) { sendGattError(address, (uint32_t)handle, -4); return; }
    uint16_t v = enable ? ((props & 0x10) ? 0x0001 : 0x0002) : 0x0000;
    uint8_t val[2] = { (uint8_t)(v & 0xFF), (uint8_t)(v >> 8) };
    _rwConnId = _conns[idx].connId;
    _rwHandle = cccd;
    _rwCause = 0xFFFF;
    if (_rwSem) xSemaphoreTake(_rwSem, 0);
    if (client_attr_write((uint8_t)_conns[idx].connId, _gattClientId, GATT_WRITE_TYPE_REQ,
                          cccd, 2, val) != GAP_CAUSE_SUCCESS) {
        sendGattError(address, (uint32_t)handle, -1);
        return;
    }
    if (!_rwSem || xSemaphoreTake(_rwSem, pdMS_TO_TICKS(5000)) != pdTRUE || _rwCause != 0) {
        sendGattError(address, (uint32_t)handle, _rwCause == 0xFFFF ? -2 : (int32_t)_rwCause);
        return;
    }
    uint8_t buf[32];
    size_t p = 0;
    p = pbUint(buf, p, 1, address);
    p = pbUint(buf, p, 2, handle);
    sendMessage(84, buf, p);
}

// BLE-stack context: only record the event. Scan resume happens in loop()
// (serviceBleOp) and the socket sends happen in the API task
// (serviceDisconnects) -- never from here (see CONNECTABLE_DESIGN.md).
void EspHomeApi::onDisconnect(uint8_t conn_id) {
    if (conn_id < 8) _discMask |= (uint8_t)(1u << conn_id);
}

// API task context
void EspHomeApi::serviceDisconnects() {
    uint8_t mask = _discMask;
    if (!mask) return;
    _discMask = 0;
    for (uint8_t cid = 0; cid < 8; cid++) {
        if (!(mask & (1u << cid))) continue;
        int idx = findConnByConnId((int)cid);
        if (idx >= 0) {
            uint64_t addr = _conns[idx].address;
            _conns[idx] = Conn();
            sendConnResponse(addr, false, 0, 0);
        }
    }
    bool any = false;
    for (int i = 0; i < MAX_CONN; i++) if (_conns[i].used) any = true;
    if (!any) _resumeScan = true;
    sendConnectionsFree();
}

// ---- GATT discovery + services (raw SDK) --------------------------------

int EspHomeApi::findConnByConnId(int connId) {
    for (int i = 0; i < MAX_CONN; i++)
        if (_conns[i].used && _conns[i].connId == connId) return i;
    return -1;
}

void EspHomeApi::onDiscResult(uint8_t conn_id, T_DISCOVERY_RESULT_TYPE t,
                              T_DISCOVERY_RESULT_DATA d) {
    if ((int)conn_id != _discConnId) return;
    switch (t) {
        case DISC_RESULT_ALL_SRV_UUID16:
            if (_nsvc < MAX_SVC) {
                _svc[_nsvc].start = d.p_srv_uuid16_disc_data->att_handle;
                _svc[_nsvc].end = d.p_srv_uuid16_disc_data->end_group_handle;
                _svc[_nsvc].uuid[0] = d.p_srv_uuid16_disc_data->uuid16 & 0xff;
                _svc[_nsvc].uuid[1] = (d.p_srv_uuid16_disc_data->uuid16 >> 8) & 0xff;
                _svc[_nsvc].ulen = 2;
                _nsvc++;
            }
            break;
        case DISC_RESULT_ALL_SRV_UUID128:
            if (_nsvc < MAX_SVC) {
                _svc[_nsvc].start = d.p_srv_uuid128_disc_data->att_handle;
                _svc[_nsvc].end = d.p_srv_uuid128_disc_data->end_group_handle;
                memcpy(_svc[_nsvc].uuid, d.p_srv_uuid128_disc_data->uuid128, 16);
                _svc[_nsvc].ulen = 16;
                _nsvc++;
            }
            break;
        case DISC_RESULT_CHAR_UUID16:
            if (_nchr < MAX_CHR) {
                _chr[_nchr].decl = d.p_char_uuid16_disc_data->decl_handle;
                _chr[_nchr].value = d.p_char_uuid16_disc_data->value_handle;
                _chr[_nchr].props = d.p_char_uuid16_disc_data->properties;
                _chr[_nchr].uuid[0] = d.p_char_uuid16_disc_data->uuid16 & 0xff;
                _chr[_nchr].uuid[1] = (d.p_char_uuid16_disc_data->uuid16 >> 8) & 0xff;
                _chr[_nchr].ulen = 2;
                _nchr++;
            }
            break;
        case DISC_RESULT_CHAR_UUID128:
            if (_nchr < MAX_CHR) {
                _chr[_nchr].decl = d.p_char_uuid128_disc_data->decl_handle;
                _chr[_nchr].value = d.p_char_uuid128_disc_data->value_handle;
                _chr[_nchr].props = d.p_char_uuid128_disc_data->properties;
                memcpy(_chr[_nchr].uuid, d.p_char_uuid128_disc_data->uuid128, 16);
                _chr[_nchr].ulen = 16;
                _nchr++;
            }
            break;
        case DISC_RESULT_CHAR_DESC_UUID16:
            if (_ndsc < MAX_DSC) {
                _dsc[_ndsc].handle = d.p_char_desc_uuid16_disc_data->handle;
                _dsc[_ndsc].uuid[0] = d.p_char_desc_uuid16_disc_data->uuid16 & 0xff;
                _dsc[_ndsc].uuid[1] = (d.p_char_desc_uuid16_disc_data->uuid16 >> 8) & 0xff;
                _dsc[_ndsc].ulen = 2;
                _ndsc++;
            }
            break;
        case DISC_RESULT_CHAR_DESC_UUID128:
            if (_ndsc < MAX_DSC) {
                _dsc[_ndsc].handle = d.p_char_desc_uuid128_disc_data->handle;
                memcpy(_dsc[_ndsc].uuid, d.p_char_desc_uuid128_disc_data->uuid128, 16);
                _dsc[_ndsc].ulen = 16;
                _ndsc++;
            }
            break;
        default:
            break;
    }
}

void EspHomeApi::onDiscState(uint8_t conn_id, T_DISCOVERY_STATE st) {
    if ((int)conn_id != _discConnId) return;
    switch (st) {
        case DISC_STATE_SRV_DONE:
            client_all_char_discovery(conn_id, _gattClientId, 0x0001, 0xffff);
            break;
        case DISC_STATE_CHAR_DONE:
            client_all_char_descriptor_discovery(conn_id, _gattClientId, 0x0001, 0xffff);
            break;
        case DISC_STATE_CHAR_DESCRIPTOR_DONE:
            _discState = 2;
            if (_discSem) xSemaphoreGive(_discSem);
            break;
        case DISC_STATE_FAILED:
            _discState = 3;
            if (_discSem) xSemaphoreGive(_discSem);
            break;
        default:
            break;
    }
}

// append a UUID: 16-bit -> short_uuid field; 128-bit -> two uint64 (high,low)
static size_t appendUuid(uint8_t *buf, size_t p, const uint8_t *uuid, uint8_t ulen,
                         uint32_t field128, uint32_t fieldShort) {
    if (ulen == 2) {
        return pbUint(buf, p, fieldShort, (uint32_t)(uuid[0] | (uuid[1] << 8)));
    }
    uint8_t be[16];
    for (int i = 0; i < 16; i++) be[i] = uuid[15 - i];  // LE -> BE
    uint64_t hi = 0, lo = 0;
    for (int i = 0; i < 8; i++) hi = (hi << 8) | be[i];
    for (int i = 8; i < 16; i++) lo = (lo << 8) | be[i];
    p = pbUint(buf, p, field128, hi);   // repeated uuid[0] = high64
    p = pbUint(buf, p, field128, lo);   // repeated uuid[1] = low64
    return p;
}

void EspHomeApi::handleGetServices(const uint8_t *payload, size_t len) {
    uint64_t address = 0;
    pbField(payload, len, 1, &address);
    int idx = findConn(address);
    if (idx < 0) { apiLogLine("[gatt] getServices: no conn entry"); return; }
    int connId = _conns[idx].connId;

    _nsvc = _nchr = _ndsc = 0;
    _discConnId = connId;
    _discState = 0;
    if (_discSem) xSemaphoreTake(_discSem, 0);  // clear
    T_GAP_CAUSE drc = client_all_primary_srv_discovery(connId, _gattClientId);
    if (drc != GAP_CAUSE_SUCCESS) {
        char dm[64]; snprintf(dm, sizeof(dm), "[gatt] srv_discovery rc=%d connId=%d", (int)drc, connId);
        apiLogLine(dm);
        // still send Done so HA doesn't hang
        uint8_t db[16]; size_t dp = pbUint(db, 0, 1, address); sendMessage(72, db, dp);
        return;
    }
    BaseType_t got = _discSem ? xSemaphoreTake(_discSem, pdMS_TO_TICKS(10000)) : pdFALSE;
    {
        char dm[80];
        snprintf(dm, sizeof(dm), "[gatt] disc done=%d state=%d svc=%d chr=%d dsc=%d", (int)got, (int)_discState, _nsvc, _nchr, _ndsc);
        apiLogLine(dm);
    }

    // one BluetoothGATTGetServicesResponse(71) per service
    for (int si = 0; si < _nsvc; si++) {
        uint8_t svc[512];
        size_t sp = 0;
        sp = appendUuid(svc, sp, _svc[si].uuid, _svc[si].ulen, 1, 4);  // service uuid
        sp = pbUint(svc, sp, 2, _svc[si].start);                       // service handle
        for (int ci = 0; ci < _nchr; ci++) {
            if (_chr[ci].decl <= _svc[si].start || _chr[ci].decl > _svc[si].end) continue;
            uint8_t ch[160];
            size_t cp = 0;
            cp = appendUuid(ch, cp, _chr[ci].uuid, _chr[ci].ulen, 1, 5);
            cp = pbUint(ch, cp, 2, _chr[ci].value);   // handle = value handle
            cp = pbUint(ch, cp, 3, _chr[ci].props);   // properties
            // descriptors (field 4): handles after the value handle, bounded by
            // the next characteristic declaration in this service (or svc end).
            // Without these, HA's bleak client refuses start_notify ("does not
            // have a characteristic client config descriptor").
            uint16_t bound = _svc[si].end;
            for (int cj = 0; cj < _nchr; cj++)
                if (_chr[cj].decl > _chr[ci].value && _chr[cj].decl <= _svc[si].end &&
                    _chr[cj].decl - 1 < bound) bound = _chr[cj].decl - 1;
            for (int di = 0; di < _ndsc; di++) {
                if (_dsc[di].handle <= _chr[ci].value || _dsc[di].handle > bound) continue;
                uint8_t de[32];
                size_t dp2 = appendUuid(de, 0, _dsc[di].uuid, _dsc[di].ulen, 1, 3);
                dp2 = pbUint(de, dp2, 2, _dsc[di].handle);
                if (cp + 2 + dp2 > sizeof(ch)) break;
                ch[cp++] = (uint8_t)((4 << 3) | 2);   // characteristic.descriptors
                cp = putVarint(ch, cp, dp2);
                memcpy(ch + cp, de, dp2); cp += dp2;
            }
            if (sp + 2 + cp + 4 > sizeof(svc)) break; // guard
            svc[sp++] = (uint8_t)((3 << 3) | 2);      // service.characteristics (field 3)
            sp = putVarint(svc, sp, cp);
            memcpy(svc + sp, ch, cp); sp += cp;
        }
        // wrap into message 71: address(1) + services(2, this one svc submsg)
        uint8_t msg[600];
        size_t mp = 0;
        mp = pbUint(msg, mp, 1, address);
        msg[mp++] = (uint8_t)((2 << 3) | 2);          // services (field 2)
        mp = putVarint(msg, mp, sp);
        memcpy(msg + mp, svc, sp); mp += sp;
        sendMessage(71, msg, mp);
    }
    uint8_t db[16]; size_t dp = pbUint(db, 0, 1, address); sendMessage(72, db, dp);
}
