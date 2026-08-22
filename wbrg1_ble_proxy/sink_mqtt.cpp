#include "sink_mqtt.h"
#include "config.h"

namespace {

const unsigned long RETRY_INTERVAL_MS = 5000;

void hexEncode(const uint8_t *in, size_t len, char *out) {
    static const char *digits = "0123456789ABCDEF";
    for (size_t i = 0; i < len; i++) {
        out[i * 2]     = digits[(in[i] >> 4) & 0x0F];
        out[i * 2 + 1] = digits[in[i] & 0x0F];
    }
    out[len * 2] = '\0';
}

}    // namespace

MqttSink::MqttSink() : _mqtt(_wifi) {}

bool MqttSink::begin() {
    _mqtt.setServer(MQTT_HOST, MQTT_PORT);
    // The 256-byte default would truncate every batch.
    _mqtt.setBufferSize(ADV_BUF_SIZE);
    // We publish every second, so the broker always sees us as alive and never
    // times us out. PubSubClient's own keepalive, though, is driven by *inbound*
    // silence -- and since we never subscribe, the only inbound traffic is the
    // PINGRESP to its own PINGREQ. This core's PubSubClient::loop() mishandles
    // that ping cycle (it self-closes the socket at each keepAlive boundary), so
    // we push keepAlive far out: our constant publishing keeps the broker happy,
    // and a genuinely dropped socket is still caught by connected()/reconnect.
    // A short socket timeout keeps a failed connect() from blocking the scan
    // loop for ~60 s.
    _mqtt.setKeepAlive(3600);
    _mqtt.setSocketTimeout(5);
    startBatch();
    return true;
}

void MqttSink::onCommand(const char *topic, void (*cb)(char *, uint8_t *, unsigned int)) {
    _cmdTopic = topic;
    _mqtt.setCallback(cb);
}

bool MqttSink::ensureWifi() {
    if (WiFi.status() == WL_CONNECTED) {
        return true;
    }
    unsigned long now = millis();
    if (now - _lastWifiTry < RETRY_INTERVAL_MS) {
        return false;
    }
    _lastWifiTry = now;
    Serial.println("[wifi] connecting");
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    return (WiFi.status() == WL_CONNECTED);
}

bool MqttSink::ensureBroker() {
    if (_mqtt.connected()) {
        return true;
    }
    unsigned long now = millis();
    if (now - _lastBrokerTry < RETRY_INTERVAL_MS) {
        return false;
    }
    _lastBrokerTry = now;

    bool ok;
    if (strlen(MQTT_USER) > 0) {
        ok = _mqtt.connect(SCANNER_ID, MQTT_USER, MQTT_PASS, MQTT_STATUS, 0, true, "offline");
    } else {
        ok = _mqtt.connect(SCANNER_ID, MQTT_STATUS, 0, true, "offline");
    }

    if (ok) {
        if (_connectedOnce) {
            _reconnects++;
        }
        _connectedOnce = true;
        Serial.print("[mqtt] connected (reconnects=");
        Serial.print(_reconnects);
        Serial.println(")");
        _mqtt.publish(MQTT_STATUS, "online", true);
        if (_cmdTopic) {
            _mqtt.subscribe(_cmdTopic);
        }
    } else {
        Serial.print("[mqtt] connect failed, rc=");
        Serial.println(_mqtt.state());
    }
    return ok;
}

void MqttSink::loop() {
    if (ensureWifi()) {
        ensureBroker();
    }
    _mqtt.loop();
}

bool MqttSink::ready() {
    return (WiFi.status() == WL_CONNECTED) && _mqtt.connected();
}

void MqttSink::startBatch() {
    _len = snprintf(_buf, ADV_BUF_SIZE, "{\"scanner\":\"%s\",\"adv\":[", SCANNER_ID);
    _inBatch = 0;
}

bool MqttSink::appendAdvert(const Advert &a) {
    char mac[13];
    hexEncode(a.addr, 6, mac);

    char payload[63];
    hexEncode(a.data, a.dataLen, payload);

    // Reserve room for the closing "]}" plus the NUL.
    size_t remaining = ADV_BUF_SIZE - _len - 3;
    int written = snprintf(_buf + _len, remaining,
                           "%s{\"mac\":\"%s\",\"at\":%u,\"et\":%u,\"rssi\":%d,\"data\":\"%s\"}",
                           (_inBatch > 0) ? "," : "",
                           mac,
                           (unsigned)a.addrType,
                           (unsigned)a.advType,
                           (int)a.rssi,
                           payload);

    if (written < 0 || (size_t)written >= remaining) {
        return false;    // does not fit; caller flushes and retries
    }
    _len += (size_t)written;
    _inBatch++;
    return true;
}

void MqttSink::flushBuffer() {
    if (_inBatch == 0) {
        return;
    }
    _len += snprintf(_buf + _len, ADV_BUF_SIZE - _len, "]}");
    if (!_mqtt.publish(MQTT_TOPIC, _buf)) {
        Serial.println("[mqtt] publish failed");
    }
    startBatch();
}

void MqttSink::publish(const Advert *adverts, size_t count) {
    if (!ready()) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        if (!appendAdvert(adverts[i])) {
            flushBuffer();
            appendAdvert(adverts[i]);    // fits in a fresh buffer
        }
    }
    flushBuffer();
}
