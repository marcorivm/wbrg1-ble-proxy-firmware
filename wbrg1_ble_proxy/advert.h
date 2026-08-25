#pragma once

#include <Arduino.h>

// A BLE advertisement exactly as the radio saw it. The payload is kept raw
// on purpose: names, service data and manufacturer data are parsed on the
// receiving side, which is what a Home Assistant remote scanner expects.
struct Advert {
    uint8_t addr[6];
    uint8_t addrType;
    uint8_t advType;
    int8_t  rssi;
    uint8_t dataLen;
    uint8_t data[31];
};

// Where adverts go once collected. MQTT is the first implementation; an
// ESPHome-native-API sink can replace it without the scan layer changing.
class AdvertSink {
public:
    virtual ~AdvertSink() {}
    virtual bool begin() = 0;
    virtual void loop() = 0;
    virtual bool ready() = 0;
    virtual void publish(const Advert *adverts, size_t count) = 0;
};

// Single-producer / single-consumer ring. The producer is the BLE stack
// callback, so push() does nothing but copy and bump an index -- no
// allocation and no network from inside that context.
class AdvertQueue {
public:
    static const size_t CAPACITY = 64;

    void push(const Advert &a) {
        size_t next = (_head + 1) % CAPACITY;
        if (next == _tail) {
            _dropped++;    // consumer is behind; newest advert is lost
            return;
        }
        _buf[_head] = a;
        _head = next;
    }

    bool pop(Advert &out) {
        if (_tail == _head) {
            return false;
        }
        out = _buf[_tail];
        _tail = (_tail + 1) % CAPACITY;
        return true;
    }

    uint32_t dropped() const { return _dropped; }
    void resetDropped() { _dropped = 0; }

private:
    Advert _buf[CAPACITY];
    volatile size_t _head = 0;
    volatile size_t _tail = 0;
    volatile uint32_t _dropped = 0;
};
