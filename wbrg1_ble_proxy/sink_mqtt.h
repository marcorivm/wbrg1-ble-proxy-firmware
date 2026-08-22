#pragma once

#include <WiFi.h>
#include <PubSubClient.h>

#include "advert.h"

// Publishes batches of adverts as JSON over MQTT. Keeps WiFi and the broker
// connection alive without blocking the scan loop.
class MqttSink : public AdvertSink {
public:
    MqttSink();

    bool begin() override;
    void loop() override;
    bool ready() override;
    void publish(const Advert *adverts, size_t count) override;

    // Count of broker (re)connections after the first. A steady climb means
    // the link is flapping.
    uint32_t reconnects() const { return _reconnects; }

    // Register a command topic + handler; (re)subscribed on every connect.
    void onCommand(const char *topic, void (*cb)(char *, uint8_t *, unsigned int));
    void publishRaw(const char *topic, const char *payload) { if (ready()) _mqtt.publish(topic, payload); }

    // Publish a JSON telemetry line for the diagnostic sensors (HA discovery
    // config is auto-published on connect).
    void publishTelemetry(int rssi, uint32_t heap, uint32_t uptime_s,
                          uint32_t adverts, uint32_t reconnects, uint32_t drops);

private:
    bool ensureWifi();
    bool ensureBroker();
    void publishDiscovery();   // HA MQTT-discovery configs for the diag sensors
    // Appends one advert as a JSON object. Returns false if it would not fit,
    // leaving the buffer untouched so the caller can flush and retry.
    bool appendAdvert(const Advert &a);
    void flushBuffer();
    void startBatch();

    WiFiClient _wifi;
    PubSubClient _mqtt;

    static const size_t ADV_BUF_SIZE = 2048;
    char _buf[ADV_BUF_SIZE];
    size_t _len = 0;
    size_t _inBatch = 0;

    unsigned long _lastWifiTry = 0;
    unsigned long _lastBrokerTry = 0;

    bool _connectedOnce = false;
    uint32_t _reconnects = 0;

    const char *_cmdTopic = nullptr;
};
