#include "esphome_api.h"

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
    xTaskCreate(apiTaskTrampoline, "esphome_api", 2048, this,
                tskIDLE_PRIORITY + 1, NULL);
}

void EspHomeApi::taskRun() {
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
}

void EspHomeApi::serveClient() {
    set_sock_recv_timeout(_cliFd, 100);  // loop ~10 Hz to stream adverts
    unsigned long lastRx = millis();
    for (;;) {
        if (_rxLen >= RXCAP) _rxLen = 0;  // wedged; drop
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
    }
    if (count > 0) sendMessage(93, buf, p);
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
            // flags: PASSIVE_SCAN(1) | ACTIVE_CONNECTIONS(2) | RAW_ADVERTISEMENTS(32)
            p = pbUint(buf, p, 15, 35);
            sendMessage(10, buf, p);
            break;
        case 11:  // ListEntitiesRequest -> ListEntitiesDoneResponse(19)
            sendEmpty(19);
            break;
        case 20:  // SubscribeStatesRequest -> nothing (no entities)
            break;
        case 66:  // SubscribeBluetoothLEAdvertisementsRequest
            _btSub = true;
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
        int idx = findConn(address);
        if (idx >= 0) {
            BLE.configConnection()->disconnect(_conns[idx].connId);
            _conns[idx] = Conn();
        }
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
    uint8_t bd[6];
    for (int i = 0; i < 6; i++) bd[i] = (uint8_t)((address >> (8 * i)) & 0xFF);
    BLEAddr addr(bd);
    BLE.configConnection()->connect(addr, (T_GAP_REMOTE_ADDR_TYPE)addrType, 4000);

    int8_t connId = -1;
    for (int t = 0; t < 40; t++) {
        connId = BLE.configConnection()->getConnId(bd, (uint8_t)addrType);
        if (connId >= 0 && BLE.connected((uint8_t)connId)) break;
        vTaskDelay(100);
    }
    if (connId >= 0 && BLE.connected((uint8_t)connId)) {
        BLE.configClient();
        BLEClient *c = BLE.addClient((uint8_t)connId);
        _conns[idx].used = true;
        _conns[idx].address = address;
        _conns[idx].connId = connId;
        _conns[idx].client = c;
        sendConnResponse(address, true, 247, 0);  // MTU refined later
    } else {
        sendConnResponse(address, false, 0, -1);
    }
    sendConnectionsFree();
}
