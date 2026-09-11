#include <Arduino.h>
#include <Esp.h>

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <NetworkEvents.h>
#include <LittleFS.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>

#include <ESPAsyncWebServer.h>
#include "MiniShell.h"
#include <PubSubClient.h>
#include <SPI.h>
#include <SD.h>

#include "config.h"
#include "parse.h"
#include "stats.h"
#include "sysinfo.h"
#include "its5_parser.h"

#include "version.h"

// A broker "slot": every field needed to independently connect, reconnect
// (with its own backoff) and publish to one MQTT server. `prefix` is the
// config key prefix (e.g. "mqtt" -> mqtt_broker_host, mqtt_user, ...).
// A broker is considered enabled when its "<prefix>_broker_host" config
// value is non-empty; leaving it empty (the default) disables that slot.
struct mqtt_broker_t {
    const char *prefix;
    PubSubClient client;
    WiFiClient plainClient;
    WiFiClientSecure secureClient;
    uint32_t next_connect;
    uint32_t connect_delay;
};

// Deliberately left with no initializer list here (see mqtt_broker_init()
// in setup()): PubSubClient/WiFiClient/WiFiClientSecure members get their
// normal default constructors either way, this just avoids depending on
// those library types being copy-constructible from a temporary.
static mqtt_broker_t broker1;
static mqtt_broker_t broker2;
static mqtt_broker_t *brokers[] = { &broker1, &broker2 };

static void mqtt_broker_init(mqtt_broker_t &b, const char *prefix)
{
    b.prefix = prefix;
    b.next_connect = 0;
    b.connect_delay = 1000;
}

static AsyncWebServer server(80);
static MiniShell shell(&Serial);
static WiFiEvent_t lastWifiEvent = ARDUINO_EVENT_NONE;
static WiFiEvent_t wifiEvent = ARDUINO_EVENT_NONE;
static String rootCA;
static char esp_mac[24];        // e.g. "aa:bb:cc:dd:ee:ff"
static char esp_id[16];
static uint8_t packet[2500];
static StaticJsonDocument < 1024 > infoDoc;

static char mqtt_info[256];
static char mqtt_status_topic[128];
static char mqtt_info_topic[128];
static char mqtt_stats_topic[128];
static char mqtt_packet_topic[128];
static its5_frame_t its5_frame;

// Wire format for ITS5_TYPE_STATS frames, must match the layout the
// esp32-c5-sniffer sends (see sniffer_stats_t in esp32-c5-sniffer.ino).
typedef struct __attribute__((packed)) {
    uint32_t uptimeMs;
    uint32_t sentPackets;
    uint32_t droppedPackets;
    uint16_t queued;
    uint16_t queueSize;
    int8_t rssi;
    uint8_t haveRssi;
    float tempC;
    uint8_t haveTemp;
} sniffer_stats_t;

static sniffer_stats_t sniffer_stats;
static bool sniffer_stats_valid = false;
static uint32_t sniffer_stats_received_ms = 0;
static const char *wifi_ap_password = "itsg5setup";
// Board: AZ-Delivery "D1 Mini ESP32" (classic dual-core ESP32, no native
// USB -- console is via an external CP2102/CH340 bridge on GPIO1/3). Pin
// choices below follow this board's own silkscreen labels; see SD_CARD.md
// for the full wiring rationale and diagram.
//
// U2RX/U2TX (this board's labeled default UART2 pins) for the sniffer
// link, freeing the VSPI bus (GPIO18/19/23/5, also silkscreen-labeled on
// this board) for the SD card below.
static constexpr int packet_rx_pin = 16;
static constexpr int packet_tx_pin = 17;
// VSPI SS/CS pin (see SD_CARD.md); SCK/MOSI/MISO come from the board's
// default SPI pin mapping used automatically by SD.begin().
static constexpr int sd_cs_pin = SS;
static constexpr const char *sd_log_dir = "/logs";
// Upper bound on how many Serial1 bytes loop() drains in one go. Without
// this, a jammed/noisy sniffer board that keeps streaming non-frame
// garbage forever can make the drain loop below monopolize the CPU,
// since Serial1.available() never actually reaches 0 -- starving
// shell.process()/ArduinoOTA.handle()/MQTT upkeep indefinitely. Capping
// it just spreads draining a large backlog across a few extra loop()
// iterations instead of blocking in one.
static constexpr uint32_t max_serial_bytes_per_loop = 4096;
static its5_frame_t pending_frame;
static bool pending_frame_valid = false;
static String device_hostname  = "its-bridge";
static constexpr const char *ota_hostname = "its-bridge";

static bool mqtt_broker_enabled(mqtt_broker_t &b)
{
    return config_get_value(String(b.prefix) + "_broker_host").length() > 0;
}

static void mqtt_schedule_reconnect(mqtt_broker_t &b)
{
    b.next_connect = millis() + b.connect_delay;
    if (b.connect_delay < 60000) {
        b.connect_delay *= 2;
        if (b.connect_delay > 60000) {
            b.connect_delay = 60000;
        }
    }
}

static void handleGetWifi(AsyncWebServerRequest *request)
{
    request->send(LittleFS, "/wifi.html", "text/html");
}

static void handlePostWifi(AsyncWebServerRequest *request)
{
    if (!request->hasParam("ssid", true)) {
        request->send(400, "text/plain", "SSID is required");
        return;
    }

    String ssid = request->getParam("ssid", true)->value();
    String password = request->hasParam("password", true)
        ? request->getParam("password", true)->value()
        : "";

    WiFi.disconnect(true, true);
    delay(200);
    WiFi.begin(ssid.c_str(), password.c_str());
    request->send(200, "text/html",
        "<html><body><h1>WiFi settings saved</h1>"
        "<p>The bridge is connecting. Reconnect to the setup network or "
        "open its new station IP after it connects.</p></body></html>");
}

static void blue_led(int on)
{
    static int last_on = -1;
    if (on != last_on) {
        last_on = on;
        digitalWrite(LED_BUILTIN, on ? LOW : HIGH);
    }
}

// Forward declaration: defined below, but sd_replay_file() (further down
// this section) needs to call it before that point in the file.
static bool mqtt_publish(const char *topic, const uint8_t *payload, size_t length);


// ---------------------------------------------------------------------------
// SD card packet logging (see SD_CARD.md for wiring)
//
// Every captured packet is appended to one growing log file per boot,
// encoded exactly as the ITS5 wire format (see its5_parser.h): "ITS5" magic
// + type + sec + usec + len + payload. That makes a log file directly
// replayable through the same its5_parse() state machine used for the live
// Serial1 stream (see sd_replay_file()), with no separate on-disk format to
// maintain.
//
// File naming: /logs/logNNNNN.its5, NNNNN chosen once at boot as
// (highest existing counter) + 1, so a fresh log file is started every
// power-cycle and no existing file is ever overwritten or appended to
// across reboots.
// ---------------------------------------------------------------------------

static bool sd_available = false;
static File sd_log_file;
// Total packets written to the SD card since boot (not reset by "sddelete"
// starting a fresh file -- that's a new file, not a new power-up).
static uint32_t sd_packets_written = 0;
// Current working directory for the ls/cd/dir/rm/format commands below.
// Reset to "/" on boot; a card swap mid-session isn't detected, so a stale
// path just fails to open on the next command like any other SD I/O error.
static String sd_cwd = "/";

// openNextFile()'s File::name() returns a bare filename on some core
// versions and a full "/logs/xxx" path on others; normalize both to a
// full path so callers don't have to care which.
static String sd_full_path(const char *dir, const char *name)
{
    if (name[0] == '/') {
        return String(name);
    }
    return String(dir) + "/" + name;
}

static const char *sd_basename(const char *name)
{
    const char *slash = strrchr(name, '/');
    return slash ? slash + 1 : name;
}

static uint32_t sd_next_log_counter(void)
{
    uint32_t max_n = 0;
    File dir = SD.open(sd_log_dir);
    if (!dir) {
        return 1;
    }
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        if (!f.isDirectory()) {
            uint32_t n = 0;
            if (sscanf(sd_basename(f.name()), "log%05lu.its5", (unsigned long *) &n) == 1 && n > max_n) {
                max_n = n;
            }
        }
        f.close();
    }
    dir.close();
    return max_n + 1;
}

static void sd_log_begin(void)
{
    if (!SD.begin(sd_cs_pin)) {
        printf("SD card: not found / init failed (logging disabled)\n");
        sd_available = false;
        return;
    }
    if (!SD.exists(sd_log_dir) && !SD.mkdir(sd_log_dir)) {
        printf("SD card: failed to create %s (logging disabled)\n", sd_log_dir);
        sd_available = false;
        return;
    }
    char filename[48];
    snprintf(filename, sizeof(filename), "%s/log%05lu.its5", sd_log_dir, (unsigned long) sd_next_log_counter());
    sd_log_file = SD.open(filename, FILE_WRITE);
    if (!sd_log_file) {
        printf("SD card: failed to open %s for writing (logging disabled)\n", filename);
        sd_available = false;
        return;
    }
    sd_available = true;
    printf("SD card: logging packets to %s\n", filename);
}

// Re-encodes `frame` exactly as the ITS5 wire format and appends it to the
// current log file. Flushes every packet (not just on close) so a power
// loss loses at most the in-flight write, not the whole session -- SD
// writes are infrequent enough (packet rate, not byte rate) that this
// isn't a meaningful wear/performance concern here.
static void sd_log_packet(const its5_frame_t &frame)
{
    if (!sd_available || !sd_log_file) {
        return;
    }
    uint8_t header[ITS5_HEADER_LEN];
    memcpy(header, "ITS5", 4);
    header[4] = frame.type;
    header[5] = frame.sec & 0xFF;
    header[6] = (frame.sec >> 8) & 0xFF;
    header[7] = (frame.sec >> 16) & 0xFF;
    header[8] = (frame.sec >> 24) & 0xFF;
    header[9] = frame.usec & 0xFF;
    header[10] = (frame.usec >> 8) & 0xFF;
    header[11] = (frame.usec >> 16) & 0xFF;
    header[12] = (frame.usec >> 24) & 0xFF;
    header[13] = frame.len & 0xFF;
    header[14] = (frame.len >> 8) & 0xFF;

    sd_log_file.write(header, sizeof(header));
    sd_log_file.write(frame.payload, frame.len);
    sd_log_file.flush();
    sd_packets_written++;
}

// Feeds one SD log file through the same its5_parse() state machine used
// for live Serial1 data, republishing every captured packet frame to MQTT
// exactly like a live one. Keeps both brokers' connections alive with
// periodic loop() calls, since a big replay can take a while and nothing
// else services them while this runs (shell commands run to completion
// before control returns to the main loop()).
//
// Paced with a small delay() after every published packet. Two reasons:
// unlike live capture (naturally rate-limited by actual RF arrival + the
// sniffer's UART throughput), a replay is bound only by SD read speed and
// how fast mqtt_publish() accepts calls -- easily 10-100x any realistic
// live rate with nothing slowing it down, which could burst-flood the
// broker. And delay() is what actually yields to the RTOS/feeds the task
// watchdog here -- the mqtt client loop() calls alone don't reliably do
// either, so a large enough file replayed in one unbroken call could trip
// the watchdog without this.
static uint32_t sd_replay_file(File &f)
{
    its5_frame_t frame;
    uint32_t packets = 0;
    while (f.available() > 0) {
        int c = f.read();
        if (c < 0) {
            break;
        }
        if (its5_parse((uint8_t) c, &frame)) {
            if (frame.type == ITS5_TYPE_PACKET && frame.len > 0) {
                mqtt_publish(mqtt_packet_topic, frame.payload, frame.len);
                packets++;
                delay(5);
            }
            for (mqtt_broker_t *b : brokers) {
                b->client.loop();
            }
        }
    }
    its5_reset();
    printf(" %lu packet(s) replayed\n", (unsigned long) packets);
    return packets;
}


// Publish to every connected+enabled broker (non-retained, QoS 0). Returns
// true if the caller should consider this message "delivered" and move on:
// either it reached at least one broker, or every enabled broker happens to
// be down right now (in which case waiting won't help this instant, and the
// per-broker reconnect logic in loop() will keep trying independently). A
// single broker being briefly disconnected never blocks delivery to a
// healthy second one.
static bool mqtt_publish(const char *topic, const uint8_t *payload, size_t length)
{
    bool published_any = false;
    bool have_disconnected_enabled_broker = false;

    for (mqtt_broker_t *b : brokers) {
        if (!mqtt_broker_enabled(*b)) {
            continue;
        }
        if (!b->client.connected()) {
            have_disconnected_enabled_broker = true;
            continue;
        }
        if (b->client.publish(topic, payload, length)) {
            published_any = true;
        }
    }
    if (published_any) {
        stats_count(1);
    }
    return published_any || !have_disconnected_enabled_broker;
}

static bool mqtt_connect(mqtt_broker_t &b)
{
    if (b.client.connected()) {
        // already connected
        return true;
    }
    char proto[16];
    char host[128];
    char user[64];
    char pass[64];
    strlcpy(proto, config_get_value(String(b.prefix) + "_protocol").c_str(), sizeof(proto));
    strlcpy(host, config_get_value(String(b.prefix) + "_broker_host").c_str(), sizeof(host));
    strlcpy(user, config_get_value(String(b.prefix) + "_user").c_str(), sizeof(user));
    strlcpy(pass, config_get_value(String(b.prefix) + "_pass").c_str(), sizeof(pass));
    int port = config_get_value(String(b.prefix) + "_broker_port").toInt();
    if (strlen(host) == 0) {
        // this broker slot is not configured, do not attempt to connect
        return false;
    }
    if (strcmp(proto, "mqtts") == 0) {
        if (strcmp(config_get_value(String(b.prefix) + "_insecure").c_str(), "true") == 0) {
            b.secureClient.setInsecure();
        } else {
            b.secureClient.setCACert(rootCA.c_str());
        }
        b.client.setClient(b.secureClient);
    } else {
        b.client.setClient(b.plainClient);
    }
    b.client.setServer(host, port);
    b.client.setBufferSize(2500);
    bool result;
    char *userp = NULL;
    char *passp = NULL;;
    if (strlen(user) > 0) {
        userp = user;
        passp = pass;
    }
    // Each broker connects with its own client ID: the primary (&"mqtt")
    // keeps the plain esp_id for backwards compatibility with existing
    // deployments, the secondary gets a suffix so pointing both slots at
    // the same broker by mistake doesn't make them fight over one client
    // ID (which would just get the older connection kicked repeatedly).
    String clientId = String(esp_id) + (strcmp(b.prefix, "mqtt") == 0 ? "" : "-2");
    printf("Connecting to %s://%s:%d (%s) ...", proto, host, port, b.prefix);
    uint32_t t0 = millis();
    result = b.client.connect(clientId.c_str(), userp, passp, mqtt_status_topic, 0, true, "offline", true);
    uint32_t duration = millis() - t0;
    printf(" %d ms...", duration);
    if (result) {
        printf("connected!\n");
        b.client.publish(mqtt_status_topic, "online", true);
        b.client.publish(mqtt_info_topic, mqtt_info);
        b.connect_delay = 1000;
        b.next_connect = 0;
    } else {
        printf("failed to connect, rc=%d\n", b.client.state());
        mqtt_schedule_reconnect(b);
    }
    return result;
}

static void handleWifiEvent(WiFiEvent_t event)
{
    wifiEvent = event;
}

static int do_wifi(int argc, char *argv[])
{
    if (argc > 1) {
        printf("Disconnecting...\n");
        WiFi.disconnect(true, true);
        delay(2000);
        char *ssid = argv[1];
        const char *pass = (argc > 2) ? argv[2] : "";
        printf("Starting WiFi %s with password '%s'...", ssid, pass);
        WiFi.begin(ssid, pass);
        printf("done\n");
    }
    printf("SSID:    %s\n", WiFi.SSID().c_str());
    return WiFi.status();
}

static int do_network(int argc, char *argv[])
{
    wl_status_t status = WiFi.status();
    printf("SSID:    %s\n", WiFi.SSID().c_str());
    printf("Status:  %d\n", status);
    printf("RSSI:    %d\n", WiFi.RSSI());
    printf("Inet:    %s\n", WiFi.localIP().toString().c_str());
    printf("Gateway: %s\n", WiFi.gatewayIP().toString().c_str());
    printf("Netmask: %s\n", WiFi.subnetMask().toString().c_str());
    printf("Web url: http://%s\n", WiFi.localIP().toString().c_str());
    return status == WL_CONNECTED ? 0 : status;
}

static int do_reboot(int argc, char *argv[])
{
    ESP.restart();
    return 0;
}

static int do_datetime(int argc, char *argv[])
{
    time_t now = time(NULL);

    struct tm *utc = gmtime(&now);
    printf("UTC  : %4d-%02d-%02d %02d:%02d:%02d\n",
           1900 + utc->tm_year, 1 + utc->tm_mon, utc->tm_mday,
           utc->tm_hour, utc->tm_min, utc->tm_sec);
    struct tm *local = localtime(&now);
    printf("Local: %4d-%02d-%02d %02d:%02d:%02d\n",
           1900 + local->tm_year, 1 + local->tm_mon, local->tm_mday,
           local->tm_hour, local->tm_min, local->tm_sec);
    return 0;
}

static int do_disconnect(int argc, char *argv[])
{
    for (mqtt_broker_t *b : brokers) {
        if (b->client.connected()) {
            b->client.disconnect();
            printf("Disconnected from %s broker\n", b->prefix);
        } else {
            printf("Not connected to %s broker\n", b->prefix);
        }
    }
    return 0;
}

static int do_led(int argc, char *argv[])
{
    bool state = (argc > 1) ? atoi(argv[1]) : !digitalRead(LED_BUILTIN);
    printf("LED %d\n", state);
    digitalWrite(LED_BUILTIN, state ? HIGH : LOW);
    return 0;
}

static size_t create_info(char *info, size_t size)
{
    infoDoc["emac"] = esp_mac;
    infoDoc["ver"] = "github.com/emente/its-g5-receiver@" GIT_VERSION;
    infoDoc["hwv"] = "xiao-esp32-c5";
    return serializeJson(infoDoc, info, size);
}

static int do_mqtt(int argc, char *argv[])
{
    for (mqtt_broker_t *b : brokers) {
        printf("--- %s ---\n", b->prefix);
        printf("enabled: %s\n", mqtt_broker_enabled(*b) ? "yes" : "no");
        printf("host: %s:%s\n", config_get_value(String(b->prefix) + "_broker_host").c_str(),
               config_get_value(String(b->prefix) + "_broker_port").c_str());
        printf("connection: %s\n", b->client.connected() ? "connected" : "not connected");
    }
    printf("node/clientid: %s\n", esp_id);
    printf("mqtt_status_topic: %s\n", mqtt_status_topic);
    printf("mqtt_info_topic: %s\n", mqtt_info_topic);
    printf("mqtt_stats_topic: %s\n", mqtt_stats_topic);
    printf("mqtt_packet_topic: %s\n", mqtt_packet_topic);
    printf("info: %s\n", mqtt_info);
    return 0;
}

static int do_ota(int argc, char *argv[])
{
    printf("hostname: %s.local\n", ota_hostname);
    printf("password: %s\n", config_get_value("ota_password").length() > 0 ? "set" : "NOT SET (insecure)");
    return 0;
}

static int do_config(int argc, char *argv[])
{
    File f = LittleFS.open("/config.json", "r");
    if (f) {
        Serial.println(f.readString());
        f.close();
    }
    return 0;
}

static int do_sysinfo(int argc, char *argv[])
{
    printf("Chip model: %s (rev 0x%02X)\n", ESP.getChipModel(), ESP.getChipRevision());
    printf("CPU freq: %d MHz\n", ESP.getCpuFreqMHz());
    printf("CPU temp: %.1f C\n", temperatureRead());
    printf("ROM size: %d bytes\n", ESP.getFlashChipSize());
    printf("ROM freq: %d MHz\n", ESP.getFlashFrequencyMHz());
    return 0;
}

static int do_tx(int argc, char *argv[])
{
    if (argc > 1) {
        int tx_power = atoi(argv[1]);
        printf("Setting TX power to %d dBm\n", tx_power);
        WiFi.setTxPower((wifi_power_t) (4 * tx_power));
    }
    printf("Current TX power: %d dBm\n", WiFi.getTxPower() / 4);
    return 0;
}

static int do_stats(int argc, char *argv[])
{
    stats_t stats;
    stats_get(&stats);
    printf("latest: %d\n", stats.latest);
    printf("counts:");
    for (int i = 0; i < 60; i++) {
        printf(" %d", stats.counts[i]);
    }
    printf("\n");
    return 0;
}

static int do_sniffer(int argc, char *argv[])
{
    if (!sniffer_stats_valid) {
        printf("No sniffer statistics received yet\n");
        return -1;
    }
    printf("Last received: %lu ms ago\n", millis() - sniffer_stats_received_ms);
    printf("Sniffer uptime: %lu ms\n", sniffer_stats.uptimeMs);
    printf("Sniffer sent packets: %lu\n", sniffer_stats.sentPackets);
    printf("Sniffer dropped packets: %lu\n", sniffer_stats.droppedPackets);
    printf("Sniffer queue: %u/%u\n", sniffer_stats.queued, sniffer_stats.queueSize);
    if (sniffer_stats.haveRssi) {
        printf("Sniffer RSSI: %d dBm\n", sniffer_stats.rssi);
    } else {
        printf("Sniffer RSSI: unavailable\n");
    }
    if (sniffer_stats.haveTemp) {
        printf("Sniffer temp: %.1f C\n", sniffer_stats.tempC);
    } else {
        printf("Sniffer temp: unavailable\n");
    }
    return 0;
}

static int do_sd(int argc, char *argv[])
{
    printf("available: %s\n", sd_available ? "yes" : "no");
    printf("packets written since boot: %lu\n", (unsigned long) sd_packets_written);
    if (!sd_available) {
        return -1;
    }
    printf("card size: %llu MB\n", SD.cardSize() / (1024ULL * 1024ULL));
    printf("used:      %llu MB\n", SD.usedBytes() / (1024ULL * 1024ULL));

    uint32_t count = 0;
    File dir = SD.open(sd_log_dir);
    if (dir) {
        for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
            if (!f.isDirectory()) {
                count++;
            }
            f.close();
        }
        dir.close();
    }
    printf("log files: %lu\n", (unsigned long) count);
    if (sd_log_file) {
        printf("current log: %u bytes written this session\n", (unsigned) sd_log_file.size());
    }
    return 0;
}

static int do_sdreplay(int argc, char *argv[])
{
    if (!sd_available) {
        printf("SD card not available\n");
        return -1;
    }
    // "delete" deletes a file once it's been FED to mqtt_publish() for
    // every packet in it -- same "attempted, not confirmed-delivered"
    // semantics mqtt_publish() already has for live packets (it returns
    // success even when every broker is down, rather than blocking; see
    // its own doc comment), so this doesn't invent a stronger guarantee
    // replay never had. The file currently being logged to this session is
    // never deleted regardless, even if asked -- SD.remove() on a file
    // still open for writing elsewhere is undefined behaviour here, not
    // just unwanted.
    bool delete_after = (argc > 1 && strcmp(argv[1], "delete") == 0);
    if (argc > 1 && !delete_after) {
        printf("Unknown option '%s' (only \"delete\" is supported)\n", argv[1]);
        return -1;
    }

    File dir = SD.open(sd_log_dir);
    if (!dir) {
        printf("Cannot open %s\n", sd_log_dir);
        return -1;
    }
    uint32_t files = 0;
    uint32_t deleted = 0;
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        if (f.isDirectory()) {
            f.close();
            continue;
        }
        String path = sd_full_path(sd_log_dir, f.name());
        bool is_current = sd_log_file && strcmp(sd_basename(path.c_str()), sd_basename(sd_log_file.name())) == 0;
        printf("Replaying %s (%u bytes)...", f.name(), (unsigned) f.size());
        sd_replay_file(f);
        f.close();
        files++;
        if (delete_after) {
            if (is_current) {
                printf("Not deleting %s: still this session's active log file\n", path.c_str());
            } else if (SD.remove(path)) {
                deleted++;
            } else {
                printf("Failed to delete %s\n", path.c_str());
            }
        }
    }
    dir.close();
    printf("Replay complete: %lu file(s)", (unsigned long) files);
    if (delete_after) {
        printf(", %lu deleted", (unsigned long) deleted);
    }
    printf("\n");
    return 0;
}

static int do_sddelete(int argc, char *argv[])
{
    if (!sd_available) {
        printf("SD card not available\n");
        return -1;
    }
    if (argc < 2 || strcmp(argv[1], "yes") != 0) {
        printf("This deletes ALL log files on the SD card. Re-run as: sddelete yes\n");
        return -1;
    }

    // Close (and stop writing to) the current session's log file before
    // possibly deleting it out from under an open handle.
    if (sd_log_file) {
        sd_log_file.close();
    }

    uint32_t deleted = 0;
    File dir = SD.open(sd_log_dir);
    if (dir) {
        for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
            bool is_dir = f.isDirectory();
            String path = sd_full_path(sd_log_dir, f.name());
            f.close();
            if (!is_dir && SD.remove(path)) {
                deleted++;
            }
        }
        dir.close();
    }
    printf("Deleted %lu log file(s)\n", (unsigned long) deleted);

    // Resume logging with a fresh file (counter restarts at 1 since none
    // remain, which is fine -- there's nothing left for it to collide with).
    sd_log_begin();
    return 0;
}

// Resolves an ls/cd/rm argument against sd_cwd: absolute paths pass
// through, ".." goes up one level, anything else is relative, and a
// missing argument means "the current directory itself".
static String sd_resolve_path(const char *arg)
{
    if (!arg || arg[0] == '\0') {
        return sd_cwd;
    }
    if (arg[0] == '/') {
        return String(arg);
    }
    if (strcmp(arg, "..") == 0) {
        int slash = sd_cwd.lastIndexOf('/');
        return slash <= 0 ? String("/") : sd_cwd.substring(0, slash);
    }
    return sd_cwd == "/" ? ("/" + String(arg)) : (sd_cwd + "/" + String(arg));
}

// "dir" is a plain alias for this -- same command, two names, so either
// habit (Unix or Windows) works at this prompt.
static int do_sdls(int argc, char *argv[])
{
    if (!sd_available) {
        printf("SD card not available\n");
        return -1;
    }
    String path = sd_resolve_path(argc > 1 ? argv[1] : nullptr);
    File dir = SD.open(path);
    if (!dir || !dir.isDirectory()) {
        printf("Not a directory: %s\n", path.c_str());
        return -1;
    }
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        if (f.isDirectory()) {
            printf("%10s  %s/\n", "<DIR>", sd_basename(f.name()));
        } else {
            printf("%10u  %s\n", (unsigned) f.size(), sd_basename(f.name()));
        }
        f.close();
    }
    dir.close();
    printf("%s\n", path.c_str());
    return 0;
}

static int do_sdcd(int argc, char *argv[])
{
    if (!sd_available) {
        printf("SD card not available\n");
        return -1;
    }
    String path = argc > 1 ? sd_resolve_path(argv[1]) : String("/");
    File dir = SD.open(path);
    if (!dir || !dir.isDirectory()) {
        printf("No such directory: %s\n", path.c_str());
        return -1;
    }
    dir.close();
    sd_cwd = path;
    printf("%s\n", sd_cwd.c_str());
    return 0;
}

static int do_sdrm(int argc, char *argv[])
{
    if (!sd_available) {
        printf("SD card not available\n");
        return -1;
    }
    if (argc < 2) {
        printf("Usage: rm <filename>\n");
        return -1;
    }
    String path = sd_resolve_path(argv[1]);
    // Refuse to remove whatever this session is still actively logging to
    // -- SD.remove() on a file that's also open for writing elsewhere is
    // undefined behaviour here, not just an inconvenience.
    if (sd_log_file && strcmp(sd_basename(path.c_str()), sd_basename(sd_log_file.name())) == 0) {
        printf("Refusing to remove %s: still this session's active log file\n", path.c_str());
        return -1;
    }
    if (!SD.remove(path)) {
        printf("Failed to remove %s (not found, or it's a directory -- rm only removes files)\n", path.c_str());
        return -1;
    }
    printf("Removed %s\n", path.c_str());
    return 0;
}

// Depth-first delete of everything under `path` (files and, once emptied,
// the directories themselves) -- see do_sdformat's own comment for why
// this recursive delete stands in for a real format.
static uint32_t sd_delete_recursive(const char *path)
{
    uint32_t deleted = 0;
    File dir = SD.open(path);
    if (!dir) {
        return 0;
    }
    if (!dir.isDirectory()) {
        dir.close();
        return 0;
    }
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        bool is_dir = f.isDirectory();
        String child = sd_full_path(path, f.name());
        f.close();
        if (is_dir) {
            deleted += sd_delete_recursive(child.c_str());
            SD.rmdir(child);
        } else if (SD.remove(child)) {
            deleted++;
        }
    }
    dir.close();
    return deleted;
}

static int do_sdformat(int argc, char *argv[])
{
    if (!sd_available) {
        printf("SD card not available\n");
        return -1;
    }
    if (argc < 2 || strcmp(argv[1], "yes") != 0) {
        printf("This deletes EVERYTHING reachable on the SD card, not just log files.\n"
               "(Not a real low-level FAT format -- the SD library used here doesn't\n"
               "expose one -- just a recursive delete of every file/directory it can see.)\n"
               "Re-run as: format yes\n");
        return -1;
    }
    if (sd_log_file) {
        sd_log_file.close();
    }
    uint32_t deleted = sd_delete_recursive("/");
    sd_cwd = "/";
    printf("Deleted %lu file(s)\n", (unsigned long) deleted);

    if (!SD.exists(sd_log_dir)) {
        SD.mkdir(sd_log_dir);
    }
    sd_log_begin();
    return 0;
}

int do_cpu(int argc, char *argv[])
{
    if (argc > 1) {
        int mhz = atoi(argv[1]);
        printf("Setting CPU speed to %d MHz\n", mhz);
        setCpuFrequencyMhz(mhz);
    }
    printf("Current CPU speed: %d MHz\n", ESP.getCpuFreqMHz());
    return 0;
}

int do_ls(int argc, char *argv[])
{
    File root = LittleFS.open("/");
    if (!root) {
        printf("Failed to open root directory\n");
        return -1;
    }
    if (!root.isDirectory()) {
        printf("Root is not a directory\n");
        return -1;
    }
    File file = root.openNextFile();
    while (file) {
        printf("%6u %s\n", file.size(), file.name());
        file = root.openNextFile();
    }
    return 0;
}

static const cmd_t commands[] = {
    { "wifi", do_wifi, "[<ssid> [password]] Configure WIFi" },
    { "network", do_network, "Show network status" },
    { "reboot", do_reboot, "Reboot" },
    { "datetime", do_datetime, "Display date and time" },
    { "disconnect", do_disconnect, "Disconnect from MQTT" },
    { "led", do_led, "[state]Toggle LED" },
    { "mqtt", do_mqtt, "Show mqtt information" },
    { "ota", do_ota, "Show OTA update information" },
    { "config", do_config, "Show configuration" },
    { "sysinfo", do_sysinfo, "Show system information" },
    { "tx", do_tx, "Set WiFi tx power" },
    { "stats", do_stats, "Show statistic internals" },
    { "sniffer", do_sniffer, "Show last received sniffer statistics" },
    { "sd", do_sd, "Show SD card logging status" },
    { "sdreplay", do_sdreplay, "[delete] Republish all SD log files to MQTT (and delete each afterwards)" },
    { "sddelete", do_sddelete, "<yes> Delete all SD log files" },
    { "cpu", do_cpu, "<MHz> Set CPU speed" },
    { "lsfs", do_ls, "List files on the internal (LittleFS) filesystem" },
    { "ls", do_sdls, "[<dir>] List an SD card directory (alias: dir)" },
    { "dir", do_sdls, "[<dir>] List an SD card directory (alias: ls)" },
    { "cd", do_sdcd, "[<dir>] Change SD card directory (no arg = root)" },
    { "rm", do_sdrm, "<file> Delete one file from the SD card" },
    { "format", do_sdformat, "<yes> Recursively delete EVERYTHING on the SD card" },
    { NULL, NULL, NULL }
};

void setup(void)
{
    mqtt_broker_init(broker1, "mqtt");
    mqtt_broker_init(broker2, "mqtt2");

    // configure LED and turn off
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, HIGH);

    blue_led(true);

    Serial.begin(115200);
    Serial.println("Hello from ESP32-C3 bridge!");

    // Keep the USB console on UART0 and use UART2 (U2RX/U2TX on this
    // board's silkscreen) for packet input.
    Serial1.begin(115200, SERIAL_8N1, packet_rx_pin, packet_tx_pin);

    // SD card packet logging, see SD_CARD.md for wiring. Uses the VSPI bus
    // (freed above by moving the sniffer link off it), so must come after
    // Serial1.begin() claimed its own pins, not before.
    sd_log_begin();

    // get unique ESP32-C3 ID
    uint64_t chipid = ESP.getEfuseMac();
    char *pid = esp_id;
    char *pemac = esp_mac;
    for (int i = 0; i < 6; i++) {
        pid += sprintf(pid, "%02x", chipid & 0xFF);
        pemac += sprintf(pemac, (i < 5) ? "%02x:" : "%02x", chipid & 0xFF);
        chipid >>= 8;
    }
    printf("espid = %s\n", esp_id);
    device_hostname = String("its-g5-bridge-") + esp_id;
    sprintf(mqtt_status_topic, "its/%s/status", esp_id);
    sprintf(mqtt_packet_topic, "its/%s/packet", esp_id);
    sprintf(mqtt_info_topic, "its/%s/info", esp_id);
    sprintf(mqtt_stats_topic, "its/%s/stats", esp_id);
    create_info(mqtt_info, sizeof(mqtt_info));

    configTzTime("CET-1CEST,M3.5.0/02,M10.5.0/03", "pool.ntp.org");
    WiFi.mode(WIFI_AP_STA);
    WiFi.setAutoReconnect(true);
    WiFi.onEvent(handleWifiEvent);
    WiFi.begin();
    WiFi.setTxPower(WIFI_POWER_8_5dBm);

    WiFi.softAP(device_hostname.c_str(), wifi_ap_password);
    printf("WiFi setup AP: %s / %s at http://%s/wifi\n",
        device_hostname.c_str(), wifi_ap_password,
        WiFi.softAPIP().toString().c_str());

    // load settings, save defaults if necessary
    LittleFS.begin();
    config_begin(LittleFS, "/config.json");
    if (!config_load()) {
        config_set_value("ntp_server", "pool.ntp.org");
        config_set_value("mqtt_insecure", "true");
        config_set_value("mqtt_protocol", "mqtts");
        config_set_value("mqtt_broker_host", "");
        config_set_value("mqtt_broker_port", "1883");
        config_set_value("mqtt_user", "");
        config_set_value("mqtt_pass", "");
        // second (optional) MQTT broker; leave mqtt2_broker_host empty to disable
        config_set_value("mqtt2_insecure", "true");
        config_set_value("mqtt2_protocol", "mqtts");
        config_set_value("mqtt2_broker_host", "");
        config_set_value("mqtt2_broker_port", "1883");
        config_set_value("mqtt2_user", "");
        config_set_value("mqtt2_pass", "");
        config_set_value("ota_password", "");
        config_set_value("sys_cpu_speed", "160");
        config_save();
    }
    server.on("/wifi", HTTP_GET, handleGetWifi);
    server.on("/wifi", HTTP_POST, handlePostWifi);
    config_serve(server, "/config", "/config.html");
    stats_begin();
    stats_serve(server, "/stats");
    sysinfo_begin(esp_id);
    sysinfo_serve(server, "/sysinfo");

    server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");
    server.begin();

    printf("Reading root CA certificate...");
    File f = LittleFS.open("/isrgrootx1.pem", "r");
    if (f) {
        rootCA = f.readString();
        f.close();
        printf("OK\n");
    } else {
        printf("Failed\n");
    }

    MDNS.begin(device_hostname.c_str());
    MDNS.addService("_http", "_tcp", 80);

    // OTA firmware updates, e.g. `pio run -e supermini_ota -t upload
    // --upload-port <device_hostname>.local`. ArduinoOTA registers its own
    // mDNS service on top of the MDNS.begin() call above.
    String ota_password = config_get_value("ota_password");
    if (ota_password.length() > 0) {
        ArduinoOTA.setPassword(ota_password.c_str());
    } else {
        printf("WARNING: ota_password is not set, OTA updates are unauthenticated\n");
    }
    ArduinoOTA.setHostname(ota_hostname);
    ArduinoOTA.onStart([]() {
        printf("OTA update starting (%s)\n", ArduinoOTA.getCommand() == U_FLASH ? "sketch" : "filesystem");
    });
    ArduinoOTA.onEnd([]() {
        printf("OTA update complete, rebooting\n");
    });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
        printf("OTA progress: %u%%\r", total > 0 ? (progress * 100) / total : 0);
    });
    ArduinoOTA.onError([](ota_error_t error) {
        printf("OTA error [%u]\n", error);
    });
    ArduinoOTA.begin();
    printf("OTA update hostname: %s.local\n", ota_hostname);

    int mhz = config_get_value("sys_cpu_speed").toInt();
    printf("Switching to %d MHz...%s\n", mhz, setCpuFrequencyMhz(mhz) ? "OK" : "FAILED");
}

void loop(void)
{
    static uint32_t last_connect = 0;
    uint32_t t = millis();
    uint32_t now = t / 1000;
    uint32_t ms = t % 1000;

    // network status
    bool online = (WiFi.status() == WL_CONNECTED) && (time(nullptr) > 1700000000L);
    if (lastWifiEvent != wifiEvent) {
        lastWifiEvent = wifiEvent;
        printf("WiFi event: %s\n", NetworkEvents::eventName(wifiEvent));
    }
    bool any_broker_connected = false;
    for (mqtt_broker_t *b : brokers) {
        if (b->client.connected()) {
            any_broker_connected = true;
            break;
        }
    }
    blue_led(ms < 500 ? !online : !any_broker_connected);

    // Keep each MQTT broker connected with its own capped exponential
    // reconnect backoff; one broker being down never blocks the other.
    for (mqtt_broker_t *b : brokers) {
        if (online && mqtt_broker_enabled(*b) && !b->client.connected() &&
                (b->next_connect == 0 || millis() >= b->next_connect)) {
            mqtt_connect(*b);
        }
        b->client.loop();
    }

    // handle any pending OTA update
    ArduinoOTA.handle();

    // watch for incoming packets
    bool have_packet = pending_frame_valid;
    uint32_t serial_bytes = 0;
    uint8_t serial_sample[16];
    size_t serial_sample_size = 0;
    while (!have_packet && Serial1.available() > 0 && serial_bytes < max_serial_bytes_per_loop) {
        int c = Serial1.read();
        if (serial_sample_size < sizeof(serial_sample)) {
            serial_sample[serial_sample_size++] = c & 0xFF;
        }
        serial_bytes++;
        if (its5_parse(c & 0xFF, &its5_frame)) {
            pending_frame = its5_frame;
            pending_frame_valid = true;
            have_packet = true;
        }
    }
    if (have_packet && pending_frame.type == ITS5_TYPE_STATS) {
        // sniffer statistics frame: store it for the "sniffer" CLI command
        // and the periodic MQTT stats publish below, rather than treating
        // it as a captured 802.11 packet.
        if (pending_frame.len == sizeof(sniffer_stats_t)) {
            memcpy(&sniffer_stats, pending_frame.payload, sizeof(sniffer_stats_t));
            sniffer_stats_valid = true;
            sniffer_stats_received_ms = millis();
        } else {
            printf("Ignoring sniffer stats frame with unexpected size %u (expected %u)\n",
                pending_frame.len, (unsigned) sizeof(sniffer_stats_t));
        }
        pending_frame_valid = false;
    } else if (have_packet && pending_frame.len == 0) {
        // zero-length ITS5 frame (e.g. a sniffer heartbeat/keepalive):
        // just show a heartbeat dot, nothing to parse or publish, and
        // deliberately not routed through mqtt_publish()/stats_count() --
        // it's not a real packet, so it shouldn't count as one.
        printf(".");
        pending_frame_valid = false;
    } else if (have_packet) {
        // Log to SD first, independent of MQTT connectivity -- a broker
        // outage should never mean a captured packet is lost, only that
        // it's not been forwarded live yet (see the "sdreplay" command).
        sd_log_packet(pending_frame);

        // send over mqtt
        blue_led(true);
        bool packet_sent = mqtt_publish(
            mqtt_packet_topic, pending_frame.payload, pending_frame.len);
        if (packet_sent) {
            pending_frame_valid = false;
            printf("Got packet %d bytes\n", pending_frame.len);
        }
        blue_led(false);

        // log to console
        ieee80211_t ieee;
        if (packet_sent && parse_ieee80211(packet, pending_frame.len, &ieee) > 0) {
            printf
                ("IEEE 802.11 packet from %02x:%02x:%02x:%02x:%02x:%02x, sequence control: %04x\n",
                    ieee.source_mac[0], ieee.source_mac[1], ieee.source_mac[2], ieee.source_mac[3],
                    ieee.source_mac[4], ieee.source_mac[5], ieee.sequence_ctrl);
        }
    }

    // keep stats up-to-date
    if (stats_update()) {
        StaticJsonDocument < 384 > doc;
        doc["temp"] = temperatureRead();
        doc["rssi"] = WiFi.RSSI();
        if (sniffer_stats_valid) {
            JsonObject sniffer = doc["sniffer"].to < JsonObject > ();
            sniffer["uptime_ms"] = sniffer_stats.uptimeMs;
            sniffer["sent"] = sniffer_stats.sentPackets;
            sniffer["dropped"] = sniffer_stats.droppedPackets;
            sniffer["queued"] = sniffer_stats.queued;
            sniffer["queue_size"] = sniffer_stats.queueSize;
            if (sniffer_stats.haveRssi) {
                sniffer["rssi"] = sniffer_stats.rssi;
            }
            if (sniffer_stats.haveTemp) {
                sniffer["temp_c"] = sniffer_stats.tempC;
            }
            sniffer["age_ms"] = millis() - sniffer_stats_received_ms;
        }
        JsonObject sd = doc["sd"].to < JsonObject > ();
        sd["found"] = sd_available;
        sd["packets_written"] = sd_packets_written;
        uint8_t json[384];
        size_t size = serializeJson(doc, json);
        if ((size > 0) && mqtt_publish(mqtt_stats_topic, json, size)) {
            printf("Published %s: %s\n", mqtt_stats_topic, json);
        }
    }
    // command line processing
    shell.process(">", commands);

    // spend some time in low-power mode
    delay(50);
}
