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
#include <WiFi.h>
#include <OTA.h>   // http_update_ota(), ota_platform_reset()
#include <WDT.h>   // hardware watchdog: a hard fault now reboots in <=8 s
#include <GTimer.h>
#include "gpio_api.h"
#include "PinNames.h"

#include "advert.h"
#include "config.h"
#include "esphome_api.h"

static AdvertQueue queue;

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

// ---- freeze spy: watchdog in IRQ mode -----------------------------------
// When the firmware stops kicking the watchdog (= the BLE-connect freeze), the
// WDG interrupt fires at NVIC priority 0 and this raw exception handler dumps
// the interrupted context (PC, exception number, NVIC active/pending) over the
// LOG UART with the ROM's polling printf, then forces a reset. If even this
// stays silent, the KM4 clock itself is gated.
#define WDG_REG  (*(volatile uint32_t *)0x40002800)
static volatile bool spyArmed = false;
extern "C" void spyWdgIsr(void *) __attribute__((used));
extern "C" void spyWdgIsr(void *) {
    uint32_t excret, msp, psp;
    __asm volatile("mov %0, lr" : "=r"(excret));
    __asm volatile("mrs %0, msp" : "=r"(msp));
    __asm volatile("mrs %0, psp" : "=r"(psp));
    WDG_REG |= (1u << 31);   // WDG_BIT_ISR_CLEAR
    uint32_t *frame = (uint32_t *)((excret & 4) ? psp : msp);
    volatile uint32_t *nvic_iabr = (volatile uint32_t *)0xE000E300;
    volatile uint32_t *nvic_ispr = (volatile uint32_t *)0xE000E200;
    volatile uint32_t *nvic_iser = (volatile uint32_t *)0xE000E100;
    volatile uint32_t *scb_icsr  = (volatile uint32_t *)0xE000ED04;
    DiagPrintf("\r\nSPY: excret=%08x pc=%08x lr=%08x xpsr=%08x ipsr=%u icsr=%08x\r\n",
               excret, frame[6], frame[5], frame[7], frame[7] & 0x1FF, *scb_icsr);
    DiagPrintf("SPY: iabr=%08x %08x ispr=%08x %08x iser=%08x %08x armed=%d\r\n",
               nvic_iabr[0], nvic_iabr[1], nvic_ispr[0], nvic_ispr[1], nvic_iser[0], nvic_iser[1], (int)spyArmed);
    DiagPrintf("SPY: stack: %08x %08x %08x %08x %08x %08x %08x %08x\r\n",
               frame[0], frame[1], frame[2], frame[3], frame[4], frame[5], frame[6], frame[7]);
    for (volatile int i = 0; i < 200000; i++) { }   // let the UART drain
    *(volatile uint32_t *)0xE000ED0C = 0x05FA0004;   // SCB AIRCR SYSRESETREQ
    for (;;) { }
}
static void wdtCbUnused(uint32_t) { }

// ---- flight recorder --------------------------------------------------
// 10 kHz sampler: distinct thread PCs go into a ring in a spare SRAM window
// just below __sram_end__ (0x1007C000; heap ends at 0x1007BE40). SRAM keeps
// its contents across the watchdog reset, so the boot marker can print the
// last PCs seen before the freeze.
#define REC_BASE   ((volatile uint32_t *)0x1007BE80)
#define REC_MAGIC  0x5AFEC0DEu
#define REC_N      56
static void recSample(uint32_t) {
    if (!spyArmed) return;
    uint32_t psp; __asm volatile("mrs %0, psp" : "=r"(psp));
    uint32_t pc = ((uint32_t *)psp)[6], lr = ((uint32_t *)psp)[5];
    volatile uint32_t *r = REC_BASE;
    if (r[0] != REC_MAGIC) { r[0] = REC_MAGIC; r[1] = 0; }
    uint32_t idx = r[1];
    if (r[2 + 2 * (idx % (REC_N / 2))] != pc) {
        uint32_t k = 2 + 2 * ((idx + 1) % (REC_N / 2));
        r[k] = pc; r[k + 1] = lr; r[1] = idx + 1;
    }
}
// ---- FTL flash backend disabled -----------------------------------------
// FLASH_Write_Lock (cpsid i + IPC to KM0 + masked spin on 0x48000204) can
// deadlock against KM0's WiFi IPC traffic when the BT stack persists bond
// data right after a connection. A proxy needs no persistent bonds, so the
// FTL flash primitives (weakened in lib_arduino.a(ftl.o)) become no-ops:
// FTL still works in RAM for the session, nothing touches flash.
static volatile uint32_t ftlSkipped = 0;
extern "C" uint32_t ftl_flash_write(uint32_t addr, void *data, uint32_t len) {
    (void)addr; (void)data; (void)len; ftlSkipped++; return 0;
}
extern "C" uint32_t ftl_flash_erase_sector(uint32_t addr) {
    (void)addr; ftlSkipped++; return 0;
}

// ---- osif_lock spy ------------------------------------------------------
// The library's osif_lock/osif_unlock are weakened (objcopy); these strong
// versions record the caller of every critical-section entry in retained
// SRAM, so after the watchdog reset we know exactly who never unlocked.
#define LCK_BASE ((volatile uint32_t *)0x1007BF80)   // [magic, caller, depth]
#define LCK_MAGIC 0x10CC10CCu
extern "C" void real_vPortEnterCritical(void);
extern "C" void real_vPortExitCritical(void);
extern "C" void real_FLASH_Write_Lock(void);
extern "C" void real_FLASH_Write_Unlock(void);
// flash-lock spy: [magic, caller, state 1=waiting 2=held 0=released]
#define FLK_BASE ((volatile uint32_t *)0x1007BFA0)
#define FLK_MAGIC 0xF1A5F1A5u
extern "C" void FLASH_Write_Lock(void) {
    FLK_BASE[0] = FLK_MAGIC; FLK_BASE[1] = (uint32_t)__builtin_return_address(0); FLK_BASE[2] = 1;
    real_FLASH_Write_Lock();
    FLK_BASE[2] = 2;
}
extern "C" void FLASH_Write_Unlock(void) {
    if (FLK_BASE[0] == FLK_MAGIC) FLK_BASE[2] = 0;
    real_FLASH_Write_Unlock();
}
// ---- generic IRQ spy ----------------------------------------------------
// Every enabled NVIC vector is hooked with one trampoline that records the
// active vector number + an in-flag in retained SRAM. After a watchdog reset
// the boot marker names the ISR the chip died in (or rules out ISR context).
#define GIRQ_BASE ((volatile uint32_t *)0x1007BFB0)  // [magic, vec, inflag, entries]
// scanfix ladder verdict, retained across the reboot the ladder may end in:
// [magic, rung1: stop_rc<<8|start_rc, moved1<<16|rung2_start_rc<<8|moved2, final_rung]
#define SFX_BASE ((volatile uint32_t *)0x1007BFC0)
#define SFX_MAGIC 0x5CAFF1Du
#define GIRQ_MAGIC 0xB7B7B7B7u
typedef void (*isr_fn_t)(void);
static isr_fn_t girqOrig[80];
extern "C" void girqSpy(void) {
    uint32_t vec = (*(volatile uint32_t *)0xE000ED04) & 0x1FF;   // ICSR.VECTACTIVE
    uint32_t prevVec = GIRQ_BASE[1], prevIn = GIRQ_BASE[2];
    GIRQ_BASE[0] = GIRQ_MAGIC; GIRQ_BASE[1] = vec; GIRQ_BASE[2] = 1; GIRQ_BASE[3]++;
    if (vec < 80 && girqOrig[vec]) girqOrig[vec]();
    GIRQ_BASE[1] = prevVec; GIRQ_BASE[2] = prevIn;   // restore for nesting
}
static void hookAllIrqs() {
    volatile uint32_t *vtor = (volatile uint32_t *)(*(volatile uint32_t *)0xE000ED08);
    for (int v = 16; v < 80; v++) {
        uint32_t h = vtor[v];
        if (h == 0 || h == ((uint32_t)&girqSpy | 1)) continue;
        girqOrig[v] = (isr_fn_t)h;
        vtor[v] = (uint32_t)&girqSpy | 1;
    }
}
// kernel-critical spy: [magic, caller, nesting]
#define CRT_BASE ((volatile uint32_t *)0x1007BF90)
#define CRT_MAGIC 0xC217C217u
extern "C" void vPortEnterCritical(void) {
    uint32_t caller = (uint32_t)__builtin_return_address(0);
    real_vPortEnterCritical();
    CRT_BASE[0] = CRT_MAGIC; CRT_BASE[1] = caller; CRT_BASE[2]++;
}
extern "C" void vPortExitCritical(void) {
    if (CRT_BASE[0] == CRT_MAGIC && CRT_BASE[2]) CRT_BASE[2]--;
    real_vPortExitCritical();
}

extern "C" uint32_t osif_lock(void) {
    uint32_t ipsr; __asm volatile("mrs %0, ipsr" : "=r"(ipsr));
    if (ipsr) return 0;
    uint32_t caller = (uint32_t)__builtin_return_address(0);
    vPortEnterCritical();
    LCK_BASE[0] = LCK_MAGIC; LCK_BASE[1] = caller; LCK_BASE[2]++;
    return 0;
}
extern "C" void osif_unlock(uint32_t flags) {
    (void)flags;
    uint32_t ipsr; __asm volatile("mrs %0, ipsr" : "=r"(ipsr));
    if (ipsr) return;
    if (LCK_BASE[0] == LCK_MAGIC && LCK_BASE[2]) LCK_BASE[2]--;
    vPortExitCritical();
}
static void recDump() {
    volatile uint32_t *r = REC_BASE;
    char m[200]; int o = 0;
    if (CRT_BASE[0] == CRT_MAGIC) {
        snprintf(m, sizeof(m), "crt: last-caller=%08x nest=%u", (unsigned)CRT_BASE[1], (unsigned)CRT_BASE[2]);
        logLine(m); CRT_BASE[0] = 0; CRT_BASE[1] = 0; CRT_BASE[2] = 0;
    }
    if (GIRQ_BASE[0] == GIRQ_MAGIC) {
        snprintf(m, sizeof(m), "girq: vec=%u inflag=%u entries=%u", (unsigned)GIRQ_BASE[1], (unsigned)GIRQ_BASE[2], (unsigned)GIRQ_BASE[3]);
        logLine(m); GIRQ_BASE[0] = 0; GIRQ_BASE[1] = 0; GIRQ_BASE[2] = 0; GIRQ_BASE[3] = 0;
    }
    if (FLK_BASE[0] == FLK_MAGIC) {
        snprintf(m, sizeof(m), "flk: last-caller=%08x state=%u", (unsigned)FLK_BASE[1], (unsigned)FLK_BASE[2]);
        logLine(m); FLK_BASE[0] = 0; FLK_BASE[1] = 0; FLK_BASE[2] = 0;
    }
    if (LCK_BASE[0] == LCK_MAGIC) {
        snprintf(m, sizeof(m), "lck: last-caller=%08x depth=%u", (unsigned)LCK_BASE[1], (unsigned)LCK_BASE[2]);
        logLine(m); LCK_BASE[0] = 0; LCK_BASE[1] = 0; LCK_BASE[2] = 0;
    }
    if (r[0] != REC_MAGIC) { logLine("rec: no data"); return; }
    uint32_t idx = r[1];
    o += snprintf(m + o, sizeof(m) - o, "rec(n=%u) pc/lr:", (unsigned)idx);
    for (int i = 7; i >= 0 && o < (int)sizeof(m) - 20; i--) {
        uint32_t k = 2 + 2 * ((idx - i) % (REC_N / 2));
        o += snprintf(m + o, sizeof(m) - o, " %x/%x", (unsigned)r[k], (unsigned)r[k + 1]);
    }
    logLine(m);
    r[0] = 0;
    if (SFX_BASE[0] == SFX_MAGIC) {
        char sm[110];
        snprintf(sm, sizeof(sm),
                 "scanfix-prev: rung1 stop=%d start=%d moved=%d | rung2 start=%d moved=%d | final=%d",
                 (int)((SFX_BASE[1] >> 8) & 0xFF), (int)(SFX_BASE[1] & 0xFF),
                 (int)((SFX_BASE[2] >> 16) & 0xFF), (int)((SFX_BASE[2] >> 8) & 0xFF),
                 (int)(SFX_BASE[2] & 0xFF), (int)SFX_BASE[3]);
        logLine(sm);
        SFX_BASE[0] = 0;
    }
}

// Hardware watchdog. Kicked every loop pass and inside the long BLE waits.
static WDT wdt;
void wdtKick() { wdt.RefreshWatchdog(); }

// Diagnostic log line. Serial (UART) only now that MQTT is gone; live device
// state is in HA via the native API sensors, and commands go over :6054.
// Ring of recent log lines, readable over the ctrl socket ("log" command).
#define LOGRING_N 10
#define LOGRING_W 120
static char logRing[LOGRING_N][LOGRING_W];
static volatile uint16_t logSeq = 0;
static void logLine(const char *msg) {
    Serial.print("[log] "); Serial.println(msg);
    snprintf(logRing[logSeq % LOGRING_N], LOGRING_W, "%u %s", (unsigned)logSeq, msg);
    logSeq++;
}
void apiLogLine(const char *msg) { logLine(msg); }
static bool bootAnnounced = false;

// Diagnostic BLE step commands, run from loop():
//   scan off | scan on | conn <12-hex-mac> [addrtype] | disc
static volatile int diagOp = 0;   // 1 scan off, 2 scan on, 3 conn, 4 disc
static uint8_t diagAddr[6];
static uint8_t diagType = 0;
static int8_t  diagConnId = -1;
static bool    diagWifiOff = false;   // connw: drop WiFi before connecting (coex test)
// Boot safety net: a counter in a backup register (survives watchdog/soft
// resets, not power loss). Incremented before BT init, cleared once BT is up.
// Two consecutive failures -> boot WITHOUT BLE so OTA over MQTT stays possible.
extern "C" void BKUP_Write(uint32_t idx, uint32_t val);
extern "C" uint32_t BKUP_Read(uint32_t idx);
extern "C" void BKUP_Set(uint32_t idx, uint32_t mask);
static const uint32_t BOOT_REG = 5, BOOT_MAGIC = 0x5AFE0000u;
static bool safeMode = false;   // true: BLE never initialised this boot
static bool bleUp = false;
static bool wifiPsOff = false;   // WiFi power-save disabled after (re)association
extern "C" int wifi_disable_powersave(void);
extern "C" void wifi_btcoex_set_bt_off(void);
extern "C" void pmu_acquire_wakelock(uint32_t nDeviceId);
extern "C" uint32_t SYSCFG_ICVersion(void);
#define PMU_OS_ID 0
#define PMU_DEV_USER_BASE_ID 16

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
// Transport-agnostic command handler (fed by MQTT and the control socket).
static void handleCommand(char *buf) {
    if (strncmp(buf, "stat", 4) == 0) {
        char m[140];
        snprintf(m, sizeof(m), "stat: api_client=%d bt_sub=%d state_sub=%d adv_sent=%u seen=%u wifi=%d",
                 (int)espApi.clientConnected(), (int)espApi.btSubscribed(), (int)espApi.stateSubscribed(),
                 (unsigned)espApi.advertsSent(), (unsigned)seenTotal, (int)(WiFi.status() == WL_CONNECTED));
        logLine(m);
        return;
    }
    if (strncmp(buf, "scan off", 8) == 0) { diagOp = 1; return; }
    if (strncmp(buf, "scan on", 7) == 0)  { diagOp = 2; return; }
    if (strncmp(buf, "disc", 4) == 0)     { diagOp = 4; return; }
    if (strncmp(buf, "ps off", 6) == 0)   { diagOp = 6; return; }
    if (strncmp(buf, "reboot", 6) == 0)   { diagOp = 7; return; }
    if (strncmp(buf, "coex off", 8) == 0) { diagOp = 8; return; }
    if (strncmp(buf, "hang", 4) == 0)     { diagOp = 9; return; }   // spy self-test: spin in loop()
    if (strncmp(buf, "uartburn", 8) == 0) { diagOp = 10; return; }  // reboot into ROM UART download mode
    if (strncmp(buf, "flashid", 7) == 0)  { diagOp = 11; return; }  // JEDEC id + SPIC params
    if (strncmp(buf, "flashbench", 10) == 0) { diagOp = 14; return; }  // time a 16KB user-mode read
    if (strncmp(buf, "scanstate", 9) == 0) { diagOp = 15; return; }  // dump GAP dev state
    if (strncmp(buf, "scanfix", 7) == 0)  { diagOp = 16; return; }  // run the scan recovery ladder
    if (strncmp(buf, "connw ", 6) == 0)   { diagWifiOff = true; memmove(buf + 4, buf + 5, strlen(buf + 5) + 1); }  // "connw x" -> "conn x"
    if (strncmp(buf, "conn ", 5) == 0) {
        // parse 12 hex digits by hand (newlib-nano sscanf has no %llx)
        const char *h = buf + 5; uint8_t b[6]; int n = 0;
        while (n < 6 && isxdigit((unsigned char)h[0]) && isxdigit((unsigned char)h[1])) {
            char tmp[3] = {h[0], h[1], 0};
            b[n++] = (uint8_t)strtoul(tmp, NULL, 16); h += 2;
            if (*h == ':') h++;
        }
        if (n == 6) {
            for (int i = 0; i < 6; i++) diagAddr[i] = b[5 - i];   // SDK wants little-endian
            diagType = (uint8_t)atoi(h);
            diagOp = 3;
        }
        return;
    }
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

// MQTT command callback: copy payload and dispatch to the shared handler.
void onMqttCommand(char *topic, uint8_t *payload, unsigned int len) {
    (void)topic;
    char buf[168];
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    memcpy(buf, payload, len);
    buf[len] = 0;
    handleCommand(buf);
}

// ---- control socket (port 6054): reliable out-of-band command/OTA channel on
// the core's ard_socket.h layer (same proven code as the ESPHome API server,
// unlike the flaky PubSubClient). Line-based; replies "ok\n".
extern "C" {
int start_server(unsigned short port, unsigned char protMode);
int sock_listen(int sock, int max);
int get_available(int sock);
int recv_data(int sock, const unsigned char *data, unsigned short len, int flag);
int send_data(int sock, const unsigned char *data, unsigned short len, int flag);
int set_sock_recv_timeout(int sock, int timeout);
void close_socket(int sock);
}
static void ctrlTaskFn(void *) {
    int srv = start_server(6054, 0);
    if (srv < 0) { vTaskDelete(NULL); return; }
    if (sock_listen(srv, 1) < 0) { vTaskDelete(NULL); return; }
    char line[168]; size_t ll = 0;
    for (;;) {
        int cli = get_available(srv);
        if (cli < 0) { vTaskDelay(100); continue; }
        set_sock_recv_timeout(cli, 200);
        const char *greet = "wbrg1 ctrl ready\n";
        send_data(cli, (const unsigned char *)greet, (uint16_t)strlen(greet), 0);
        // One command per connection: read a line, handle, reply, close. This
        // core's recv_data doesn't reliably return 0 on peer close, so we also
        // cap idle time (~5 s) to never wedge the accept loop on a dead socket.
        ll = 0;
        int idle = 0;
        for (;;) {
            unsigned char c;
            int n = recv_data(cli, &c, 1, 0);
            if (n == 0) break;                          // peer closed
            if (n < 0) { if (++idle > 25) break; vTaskDelay(10); continue; }
            idle = 0;
            if (c == '\n' || c == '\r') {
                if (ll > 0) {
                    line[ll] = 0;
                    if (strncmp(line, "log", 3) == 0) {
                        uint16_t s0 = (logSeq > LOGRING_N) ? (uint16_t)(logSeq - LOGRING_N) : 0;
                        for (uint16_t sq = s0; sq != logSeq; sq++) {
                            const char *l = logRing[sq % LOGRING_N];
                            send_data(cli, (const unsigned char *)l, (uint16_t)strlen(l), 0);
                            send_data(cli, (const unsigned char *)"\n", 1, 0);
                        }
                        const char *ok2 = "ok\n";
                        send_data(cli, (const unsigned char *)ok2, 3, 0);
                    } else if (strncmp(line, "stat", 4) == 0) {
                        char sm[176];
                        int sl = snprintf(sm, sizeof(sm),
                            "api_client=%d bt_sub=%d state_sub=%d adv_sent=%u seen=%u wifi=%d\n",
                            (int)espApi.clientConnected(), (int)espApi.btSubscribed(),
                            (int)espApi.stateSubscribed(), (unsigned)espApi.advertsSent(),
                            (unsigned)seenTotal, (int)(WiFi.status() == WL_CONNECTED));
                        send_data(cli, (const unsigned char *)sm, (uint16_t)sl, 0);
                    } else {
                        handleCommand(line);
                        const char *ok = "ok\n";
                        send_data(cli, (const unsigned char *)ok, 3, 0);
                    }
                }
                break;                                  // done with this client
            } else if (ll < sizeof(line) - 1) {
                line[ll++] = (char)c;
            }
        }
        close_socket(cli);
    }
}
static bool ctrlStarted = false;

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
#if MQTT_PUBLISH_ADVERTS
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
#endif
        // Keep draining even once the batch is full, so the queue does not
        // stall and back up into the BLE callback.
    }
}

// Status LEDs (identified by pad sweep 2026-08-24, both active-high):
//   red = PA25 (0x19), blue = PB22 (0x36)  -- these were Tuya's state/net LEDs.
static gpio_t ledRed, ledBlue;
static bool   ledsInit = false;
static void ledInit() {
    gpio_init(&ledRed, PA_25);  gpio_dir(&ledRed, PIN_OUTPUT);  gpio_mode(&ledRed, PullNone);  gpio_write(&ledRed, 0);
    gpio_init(&ledBlue, PB_22); gpio_dir(&ledBlue, PIN_OUTPUT); gpio_mode(&ledBlue, PullNone); gpio_write(&ledBlue, 0);
    ledsInit = true;
}
// blue = heartbeat: a brief pulse showing the firmware is alive (not always-on).
// red  = warnings only (off healthy / blink wifi-down / solid safe-mode).
static void ledService() {
    if (!ledsInit) return;
    bool wifi = (WiFi.status() == WL_CONNECTED);
    uint32_t t = millis() % 60000;                // 60 s heartbeat period
    // "lub-dub": two clear ~70 ms pulses at the top of each minute, else dark
    bool beat = (t < 70) || (t >= 220 && t < 290);
    bool warnBlink = ((millis() >> 8) & 1);        // ~2 Hz for warnings
    gpio_write(&ledBlue, beat ? 1 : 0);
    gpio_write(&ledRed,  safeMode ? 1 : (!wifi ? (warnBlink ? 1 : 0) : 0));
}

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println();
    Serial.println("[wbrg1] BLE advertisement proxy starting");
    Serial.print("[wbrg1] scanner id: ");
    Serial.println(SCANNER_ID);

    // WiFi is brought up by the ESPHome API startup in loop(); commands arrive
    // over the :6054 control socket. No MQTT.

    // EXPERIMENT: hold KM4 awake like Tuya's firmware does while BT is active.
    pmu_acquire_wakelock(PMU_OS_ID);
    pmu_acquire_wakelock(PMU_DEV_USER_BASE_ID);

    // Watchdog first: a hang inside BT init must reboot, not stick.
    wdt.InitWatchdog(8000);
    wdt.StartWatchdog();
    ledInit();
    // Flight recorder: 1 kHz timer samples the interrupted thread's PC/LR into
    // backup registers (survive the WDT reset); printed in the boot marker.
    GTimer.begin(2, 100, recSample);

    uint32_t r = BKUP_Read(BOOT_REG);
    uint32_t fails = ((r & 0xFFFF0000u) == BOOT_MAGIC) ? (r & 0xFFFFu) : 0;
    Serial.print("[wbrg1] previous BT-init failures: "); Serial.println(fails);
    if (fails >= 2) {
        safeMode = true;
        BKUP_Write(BOOT_REG, BOOT_MAGIC | 0);   // next reboot tries BT again
        Serial.println("[wbrg1] SAFE MODE: skipping BLE init (OTA/MQTT only)");
        lastFlush = millis(); lastStats = millis();
        return;
    }
    BKUP_Write(BOOT_REG, BOOT_MAGIC | (fails + 1));

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
    bool gattOk = espApi.initGatt();   // client_init + register our GATT client
    Serial.print("[wbrg1] gatt client registered: "); Serial.println(gattOk ? "yes" : "NO");
    BLE.configScan()->startScan();    // continuous
    bleUp = true;
    hookAllIrqs();
    Serial.println("[wbrg1] all NVIC vectors hooked");
    BKUP_Write(BOOT_REG, BOOT_MAGIC | 0);   // BT came up: clear the failure counter

    Serial.println("[wbrg1] scanning");
    lastFlush = millis();
    lastStats = millis();
}

// A timed-out le_connect leaves a create-connection PENDING in the controller
// forever, which silently blocks ALL scanning (both boards went advert-dark for
// Bermuda until a reboot, 2026-08-24 night). le_disconnect() on a link in
// CONNECTING state issues the create-connection-cancel. Always cancel before
// resuming the scanner after a failed connect.
void cancelPendingConnects() {
    T_GAP_CONN_INFO ci;
    for (uint8_t i = 0; i < 4; i++) {
        if (BLE.configConnection()->getConnInfo(i, &ci) &&
            ci.conn_state == GAP_CONN_STATE_CONNECTING) {
            char cm[48];
            snprintf(cm, sizeof(cm), "conn: cancelling pending connId=%u", i);
            logLine(cm);
            le_disconnect(i);
        }
    }
}

// ---- scan-wedge soft recovery (v7) --------------------------------------
// The scan engine occasionally hard-wedges after connect churn (v6 rebooted
// through it, ~1x/hour under GATT polling). This ladder tries soft recoveries
// first and logs exactly which rung works — the root-cause probe AND the fix.
#include "gap_scan.h"   // le_scan_start/stop/set_param, T_LE_SCAN_PARAM_TYPE
#include "gap_msg.h"    // T_GAP_DEV_STATE

static void logScanState(const char *tag) {
    T_GAP_DEV_STATE ds;
    memset(&ds, 0, sizeof(ds));
    le_get_gap_param(GAP_PARAM_DEV_STATE, &ds);
    char m2[96];
    snprintf(m2, sizeof(m2), "%s: init=%u scan=%u conn=%u adv=%u seen=%lu",
             tag, ds.gap_init_state, ds.gap_scan_state, ds.gap_conn_state,
             ds.gap_adv_state, (unsigned long)seenTotal);
    logLine(m2);
}

// waits ~4 s (kicking the wdt) and reports whether adverts moved
static bool advertsMoving() {
    uint32_t s0 = seenTotal;
    for (int i = 0; i < 40; i++) { delay(100); wdtKick(); }
    return seenTotal != s0;
}

// returns rung that revived scanning: 1=stop/start, 2=param reset, 0=failed
static int scanRecoveryLadder() {
    char m2[80];
    logScanState("scanfix: pre");
    SFX_BASE[0] = SFX_MAGIC; SFX_BASE[1] = 0xFFFF; SFX_BASE[2] = 0xFFFFFF; SFX_BASE[3] = 99;
    cancelPendingConnects();
    // rung 1: GAP-level stop/start with return codes
    T_GAP_CAUSE r1 = le_scan_stop();
    delay(400); wdtKick();
    T_GAP_CAUSE r2 = le_scan_start();
    snprintf(m2, sizeof(m2), "scanfix: rung1 stop=%d start=%d", (int)r1, (int)r2);
    logLine(m2);
    SFX_BASE[1] = ((uint32_t)((uint8_t)r1) << 8) | (uint8_t)r2;
    bool mv1 = advertsMoving();
    SFX_BASE[2] = ((uint32_t)(mv1 ? 1 : 0) << 16) | 0xFFFF;
    if (mv1) { logLine("scanfix: rung1 REVIVED"); SFX_BASE[3] = 1; return 1; }
    // rung 2: full param re-set + start
    le_scan_stop(); delay(400); wdtKick();
    uint8_t mode = SCAN_ACTIVE ? GAP_SCAN_MODE_ACTIVE : GAP_SCAN_MODE_PASSIVE;
    uint16_t interval = (uint16_t)(SCAN_INTERVAL_MS * 1000 / 625);
    uint16_t window   = (uint16_t)(SCAN_WINDOW_MS * 1000 / 625);
    uint8_t filt_pol = GAP_SCAN_FILTER_ANY, filt_dup = 0;
    le_scan_set_param(GAP_PARAM_SCAN_MODE, sizeof(mode), &mode);
    le_scan_set_param(GAP_PARAM_SCAN_INTERVAL, sizeof(interval), &interval);
    le_scan_set_param(GAP_PARAM_SCAN_WINDOW, sizeof(window), &window);
    le_scan_set_param(GAP_PARAM_SCAN_FILTER_POLICY, sizeof(filt_pol), &filt_pol);
    le_scan_set_param(GAP_PARAM_SCAN_FILTER_DUPLICATES, sizeof(filt_dup), &filt_dup);
    T_GAP_CAUSE r3 = le_scan_start();
    snprintf(m2, sizeof(m2), "scanfix: rung2 param-reset start=%d", (int)r3);
    logLine(m2);
    SFX_BASE[2] = (SFX_BASE[2] & 0xFF0000u) | ((uint32_t)((uint8_t)r3) << 8) | 0xFF;
    logScanState("scanfix: post2");
    bool mv2 = advertsMoving();
    SFX_BASE[2] = (SFX_BASE[2] & 0xFFFF00u) | (mv2 ? 1 : 0);
    if (mv2) { logLine("scanfix: rung2 REVIVED"); SFX_BASE[3] = 2; return 2; }
    logLine("scanfix: ladder FAILED");
    SFX_BASE[3] = 0;
    return 0;
}

extern "C" void flashDiagInfo(char *out, unsigned n);
extern "C" void flashDiagBench(char *out, unsigned n);
extern "C" void flashClkSafe(void);
static unsigned long lastClkSafe = 0;

// Scan-starvation watchdog: repeated connect cycles can hard-wedge the
// Realtek scan engine (startScan accepted, zero adverts; scan off/on and even
// conn/disc cycles cannot revive it — only a reboot). If scanning should be
// running but no advert arrives for 2 minutes, reboot. Bermuda tolerates the
// ~30 s gap; a deaf proxy forever is far worse.
static uint32_t scanWdLastSeen = 0;
static unsigned long scanWdLastChange = 0;

static void runDiag() {
    int op = diagOp;
    if (op == 0) return;
    diagOp = 0;
    if (!bleUp && op != 7 && op != 6 && op != 11 && op != 14 && op != 15) { logLine("diag: ignored, BLE not up (safe mode)"); return; }
    char m[96];
    if (op == 1) {
        logLine("diag: stopScan...");
        BLE.configScan()->stopScan();
        logLine("diag: stopScan done");
    } else if (op == 2) {
        logLine("diag: startScan...");
        BLE.configScan()->startScan();
        logLine("diag: startScan done");
    } else if (op == 10) {
        // Same as the SDK shell's "reboot uartburn": the ROM sees BIT_UARTBURN_BOOT
        // in BKUP_REG0 and enters UART download mode. Only a power cycle exits it.
        logLine("diag: rebooting into UART download mode (power-cycle to leave)");
        delay(300);
        BKUP_Set(0, 0x200);
        *(volatile uint32_t *)0xE000ED0C = 0x05FA0004;   // SCB AIRCR SYSRESETREQ
        for (;;) { }
    } else if (op == 9) {
        logLine("diag: HANG (spin forever, interrupts enabled) -- spy should fire in ~8 s");
        spyArmed = true;
        for (;;) { }
    } else if (op == 11) {
        flashDiagInfo(m, sizeof(m));
        logLine(m);
    } else if (op == 14) {
        flashDiagBench(m, sizeof(m));
        logLine(m);
    } else if (op == 15) {
        logScanState("scanstate");
    } else if (op == 16) {
        int r = scanRecoveryLadder();
        snprintf(m, sizeof(m), "scanfix: result=%d", r);
        logLine(m);
    } else if (op == 8) {
        wifi_btcoex_set_bt_off();
        logLine("diag: wifi_btcoex_set_bt_off() done");
    } else if (op == 7) {
        logLine("diag: reboot");
        delay(300);
        ota_platform_reset();
    } else if (op == 6) {
        int r = wifi_disable_powersave();
        snprintf(m, sizeof(m), "diag: wifi_disable_powersave -> %d", r);
        logLine(m);
    } else if (op == 3) {
        flashClkSafe();   // slow SPIC before the fragile connect window (see flash_diag.cpp)
        if (diagWifiOff) {
            diagWifiOff = false;
            logLine("diag: WiFi.disconnect() before connect (UART only from here)");
            delay(200);
            WiFi.disconnect();
            delay(1500); wdtKick();
        }
        snprintf(m, sizeof(m), "diag: connect %02x:%02x:%02x:%02x:%02x:%02x type %d ...",
                 diagAddr[5], diagAddr[4], diagAddr[3], diagAddr[2], diagAddr[1], diagAddr[0], diagType);
        logLine(m);
        spyArmed = true;
        BLEAddr addr(diagAddr);
        bool r = BLE.configConnection()->connect(addr, (T_GAP_REMOTE_ADDR_TYPE)diagType, 5000);
        snprintf(m, sizeof(m), "diag: le_connect returned %d, waiting...", (int)r);
        logLine(m);
        diagConnId = -1;
        for (int t = 0; t < 60 && diagConnId < 0; t++) {
            delay(100); wdtKick();
            for (uint8_t i = 0; i < 3; i++) if (BLE.connected(i)) { diagConnId = i; break; }
        }
        snprintf(m, sizeof(m), "diag: connId=%d (%s)", diagConnId, diagConnId >= 0 ? "CONNECTED" : "timeout");
        logLine(m);
        if (diagConnId < 0) {           // failed: cancel the pending connect and rescue the scanner
            cancelPendingConnects();
            delay(150);
            BLE.configScan()->startScan();
            logLine("diag: scanner resumed after failed connect");
        }
    } else if (op == 4) {
        snprintf(m, sizeof(m), "diag: disconnect connId=%d ...", diagConnId);
        logLine(m);
        if (diagConnId >= 0) BLE.configConnection()->disconnect(diagConnId);
        delay(500); wdtKick();
        snprintf(m, sizeof(m), "diag: after disconnect connected=%d", diagConnId >= 0 ? (int)BLE.connected(diagConnId) : -1);
        logLine(m);
        diagConnId = -1;
    }
}

void loop() {
    wdtKick();
    if (!bootAnnounced && WiFi.status() == WL_CONNECTED) {
        bootAnnounced = true;
        char m[120];
        snprintf(m, sizeof(m), "boot: build " __DATE__ " " __TIME__ " gatt=%d heap=%u%s",
                 (int)espApi.gattReady(), (unsigned)xPortGetFreeHeapSize(), safeMode ? " SAFE-MODE" : "");
        logLine(m);
        recDump();
    }
    ledService();
    runDiag();
    if (millis() - lastClkSafe >= 100) {   // keep SPIC at the safe divider (see flash_diag.cpp)
        flashClkSafe();
        lastClkSafe = millis();
    }
    if (bleUp) {
        bool connActive = false;
        for (uint8_t i = 0; i < 3; i++) if (BLE.connected(i)) { connActive = true; break; }
        if (seenTotal != scanWdLastSeen || connActive) {
            scanWdLastSeen = seenTotal;
            scanWdLastChange = millis();
        } else if (millis() - scanWdLastChange > 120000) {
            logLine("scanwd: no adverts for 120s with no active conn -- trying soft recovery");
            int rung = scanRecoveryLadder();
            if (rung > 0) {
                char wm[48];
                snprintf(wm, sizeof(wm), "scanwd: soft recovery ok (rung %d)", rung);
                logLine(wm);
                scanWdLastSeen = seenTotal;
                scanWdLastChange = millis();
            } else {
                logLine("scanwd: soft recovery failed -- rebooting");
                delay(400);
                ota_platform_reset();
            }
        }
    }

    // Every pass: keep the BLE ring empty. This must not be gated behind the
    // MQTT servicing below, which blocks ~500 ms per call in this core.
    drainQueue();

    // Execute any BLE control op the API task marshalled to us (connect/
    // disconnect must run in this loop task's context, not the socket task).
    if (espApiStarted && bleUp) espApi.serviceBleOp();
    if (bleUp) hookAllIrqs();   // idempotent; catches late-registered IRQs

    // Keep WiFi awake: power-save drops idle TCP sockets (the old advert firehose
    // masked this by keeping the radio busy). Re-apply on every (re)association.
    bool wifiUp = (WiFi.status() == WL_CONNECTED);
    if (wifiUp && !wifiPsOff) { wifi_disable_powersave(); wifiPsOff = true; Serial.println("[wifi] power-save disabled"); }
    if (!wifiUp) wifiPsOff = false;

    // ESPHome native API: start once WiFi is up, then service every pass.
    if (!espApiStarted && WiFi.status() == WL_CONNECTED) {
        espApiStarted = true;
        uint8_t m[6] = {0};
        WiFi.macAddress(m);
        snprintf(espMac, sizeof(espMac), "%02X:%02X:%02X:%02X:%02X:%02X",
                 m[0], m[1], m[2], m[3], m[4], m[5]);
        espApi.begin(6053, SCANNER_ID, espMac, "WBRG1 (RTL8721CSM)");
        Serial.println("[esp-api] server started on :6053");
        if (!ctrlStarted) {
            xTaskCreate(ctrlTaskFn, "ctrl", 4096, NULL, tskIDLE_PRIORITY + 1, NULL);
            ctrlStarted = true;
            Serial.println("[ctrl] command server started on :6054");
        }
    }

    unsigned long now = millis();

    // Keep WiFi associated (non-blocking, throttled). The ESPHome API server
    // and the :6054 control socket run on top of it.
    if (WiFi.status() != WL_CONNECTED && now - lastMqtt >= 5000) {
        lastMqtt = now;
        Serial.println("[wifi] connecting");
        WiFi.begin(WIFI_SSID, WIFI_PASS);
    }

    if (otaPending) {
        otaPending = false;
        Serial.print("[ota] pulling http://");
        Serial.print(otaHost); Serial.print(":"); Serial.print(otaPort);
        Serial.println(otaRes);
        wdt.StopWatchdog();   // the pull blocks loop() for ~15 s
        int ret = http_update_ota(otaHost, otaPort, otaRes);
        wdt.StartWatchdog();
        Serial.print("[ota] result="); Serial.println(ret);
        if (ret == 0) {
            Serial.println("[ota] success -- rebooting");
            delay(200);
            ota_platform_reset();
        } else {
            Serial.println("[ota] failed -- staying on current image");
        }
    }

    if (now - lastStats >= 30000) {
        lastStats = now;
        Serial.print("[wbrg1] adverts seen: ");
        Serial.print(seenTotal);
        Serial.print("  queue drops: ");
        Serial.print(queue.dropped());
        Serial.print("  wifi: ");
        Serial.println(WiFi.status() == WL_CONNECTED ? "up" : "down");

        // Diagnostics to HA as native ESPHome API sensors.
        espApi.setTelemetry(WiFi.RSSI(), (uint32_t)xPortGetFreeHeapSize(),
                            (uint32_t)(now / 1000), seenTotal, queue.dropped());
    }

    delay(5);
}
