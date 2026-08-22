/*
 * WBRG1 BLE advertisement proxy
 *
 * Scans BLE advertisements on the Realtek RTL8721CSM inside a Tuya WBRG1
 * module and forwards them off-box, so Home Assistant can register the
 * module as a Bluetooth scanner and Bermuda can use it for presence.
 *
 * The scan layer below knows nothing about the transport: it fills a queue
 * with raw adverts. Swapping MQTT for an ESPHome-native-API sink means
 * replacing the AdvertSink implementation, nothing else.
 *
 * Build:
 *   arduino-cli compile --fqbn realtek:AmebaD:Ameba_AMB21_AMB22 .
 *
 * NOTE: AMB22 (RTL8722CSM) is the closest available target to the WBRG1's
 * RTL8721CSM. A clean build proves the toolchain, not that this image is
 * correct for that part. Back up the module's 8 MB flash before writing it.
 */

#include "BLEDevice.h"
#include <OTA.h>   // http_update_ota(), ota_platform_reset()

#include "advert.h"
#include "config.h"
#include "sink_mqtt.h"
#include "esphome_api.h"

static AdvertQueue queue;
static MqttSink sink;

// ESPHome native API server (Phase 1: adoption). Runs alongside MQTT.
static EspHomeApi espApi;
static bool espApiStarted = false;
static char espMac[18] = {0};

// Coalesced view of one flush window.
static const size_t BATCH_MAX = 48;
static Advert batch[BATCH_MAX];

// OTA over WiFi, on demand. MQTT command:  ota <host> <port> <resource>
// The module pulls an OTA_All-format image (see flashtool/make_ota.py) over
// plain HTTP into the inactive slot, verifies, and reboots into it. Runs from
// loop() on a flag set by the MQTT callback.
static volatile bool otaPending = false;
static char otaHost[48] = {0};
static int  otaPort = 0;
static char otaRes[80] = {0};

static unsigned long lastFlush = 0;
static unsigned long lastStats = 0;
static unsigned long lastMqtt  = 0;
static uint32_t seenTotal = 0;
static size_t batchCount = 0;

// PubSubClient::loop() blocks ~500 ms in this core, so it is serviced on a
// timer rather than every pass. The queue is drained every pass regardless.
static const unsigned long MQTT_SERVICE_MS = 1000;

// Runs in the BLE stack's context. Copy and leave -- no network, no
// allocation, no Serial.
void onMqttCommand(char *topic, uint8_t *payload, unsigned int len) {
    (void)topic;
    char buf[168];
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    memcpy(buf, payload, len);
    buf[len] = 0;
    if (strncmp(buf, "ota", 3) != 0) return;
    char host[48] = {0}, res[80] = {0};
    int port = 0;
    if (sscanf(buf, "ota %47s %d %79s", host, &port, res) == 3) {
        strncpy(otaHost, host, sizeof(otaHost) - 1);
        otaPort = port;
        strncpy(otaRes, res, sizeof(otaRes) - 1);
        otaPending = true;
    }
}

void scanCallback(T_LE_CB_DATA *p_data) {
    if (p_data == NULL || p_data->p_le_scan_info == NULL) {
        return;
    }
    T_LE_SCAN_INFO *info = p_data->p_le_scan_info;

    Advert a;
    memcpy(a.addr, info->bd_addr, 6);
    a.addrType = (uint8_t)info->remote_addr_type;
    a.advType = (uint8_t)info->adv_type;
    a.rssi = info->rssi;
    a.dataLen = (info->data_len > 31) ? 31 : info->data_len;
    memcpy(a.data, info->data, a.dataLen);

    queue.push(a);
}

// Drains the queue into `batch`, keeping only the newest advert per
// (address, advert type). A device seen twenty times in a flush window is
// worth one report carrying its latest RSSI. Called every loop pass so the
// ring never backs up while MQTT is being serviced.
static void drainQueue() {
    Advert a;
    while (queue.pop(a)) {
        seenTotal++;
        espApi.pushAdvert(a);   // ESPHome native BLE proxy (no-op unless HA subscribed)
        bool merged = false;
        for (size_t i = 0; i < batchCount; i++) {
            if (batch[i].advType == a.advType && memcmp(batch[i].addr, a.addr, 6) == 0) {
                batch[i] = a;
                merged = true;
                break;
            }
        }
        if (!merged && batchCount < BATCH_MAX) {
            batch[batchCount++] = a;
        }
        // Keep draining even once the batch is full, so the queue does not
        // stall and back up into the BLE callback.
    }
}

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println();
    Serial.println("[wbrg1] BLE advertisement proxy starting");
    Serial.print("[wbrg1] scanner id: ");
    Serial.println(SCANNER_ID);

    sink.begin();
    sink.onCommand(MQTT_CMD, onMqttCommand);

    BLE.init();
#if SCAN_ACTIVE
    BLE.configScan()->setScanMode(GAP_SCAN_MODE_ACTIVE);
#else
    BLE.configScan()->setScanMode(GAP_SCAN_MODE_PASSIVE);
#endif
    BLE.configScan()->setScanInterval(SCAN_INTERVAL_MS);
    BLE.configScan()->setScanWindow(SCAN_WINDOW_MS);
    // Presence needs the repeats: each one is a fresh RSSI sample.
    BLE.configScan()->setScanDuplicateFilter(false);
    BLE.configScan()->updateScanParams();
    BLE.setScanCallback(scanCallback);

    BLE.beginCentral(3);   // allow up to 3 GATT connections (proxy) + scan
    BLE.configScan()->startScan();    // continuous

    Serial.println("[wbrg1] scanning");
    lastFlush = millis();
    lastStats = millis();
}

void loop() {
    // Every pass: keep the BLE ring empty. This must not be gated behind the
    // MQTT servicing below, which blocks ~500 ms per call in this core.
    drainQueue();

    // ESPHome native API: start once WiFi is up, then service every pass.
    if (!espApiStarted && WiFi.status() == WL_CONNECTED) {
        espApiStarted = true;
        uint8_t m[6] = {0};
        WiFi.macAddress(m);
        snprintf(espMac, sizeof(espMac), "%02X:%02X:%02X:%02X:%02X:%02X",
                 m[0], m[1], m[2], m[3], m[4], m[5]);
        espApi.begin(6053, SCANNER_ID, espMac, "WBRG1 (RTL8721CSM)");
        Serial.println("[esp-api] server started on :6053");
    }

    unsigned long now = millis();

    // Service WiFi/MQTT (and its blocking loop()) on its own cadence.
    if (now - lastMqtt >= MQTT_SERVICE_MS) {
        lastMqtt = now;
        sink.loop();
    }

    if (otaPending) {
        otaPending = false;
        Serial.print("[ota] pulling http://");
        Serial.print(otaHost); Serial.print(":"); Serial.print(otaPort);
        Serial.println(otaRes);
        int ret = http_update_ota(otaHost, otaPort, otaRes);
        Serial.print("[ota] result="); Serial.println(ret);
        if (ret == 0) {
            Serial.println("[ota] success -- rebooting");
            delay(200);
            ota_platform_reset();
        } else {
            Serial.println("[ota] failed -- staying on current image");
        }
    }

    if (now - lastFlush >= FLUSH_INTERVAL_MS) {
        lastFlush = now;
        if (batchCount > 0 && sink.ready()) {
            sink.publish(batch, batchCount);
        }
        batchCount = 0;    // start a fresh coalescing window
    }

    if (now - lastStats >= 30000) {
        lastStats = now;
        Serial.print("[wbrg1] adverts seen: ");
        Serial.print(seenTotal);
        Serial.print("  queue drops: ");
        Serial.print(queue.dropped());
        Serial.print("  reconnects: ");
        Serial.print(sink.reconnects());
        Serial.print("  link: ");
        Serial.println(sink.ready() ? "up" : "down");

        // Diagnostics to HA (auto-discovered sensors).
        sink.publishTelemetry(WiFi.RSSI(), (uint32_t)xPortGetFreeHeapSize(),
                              (uint32_t)(now / 1000), seenTotal,
                              sink.reconnects(), queue.dropped());
    }

    delay(5);
}
