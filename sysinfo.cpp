#include <string.h>             // for strcpy

#include <Arduino.h>
#include <ArduinoJson.h>
#include "LittleFS.h"
#include <WiFi.h>

#include "version.h"
#include "sysinfo.h"

static const char *node_id;

static void handleGetRequest(AsyncWebServerRequest *request)
{
    AsyncResponseStream *response = request->beginResponseStream("application/json");

    StaticJsonDocument < 1024 > doc;
    doc["id"] = node_id;
    doc["version"] = GIT_VERSION;
    doc["datetime"] = time(nullptr);
    doc["uptime"] = millis() / 1000;

    JsonObject cpu = doc["cpu"].to < JsonObject > ();
    cpu["chipmodel"] = ESP.getChipModel();
    char temp[10];
    snprintf(temp, sizeof(temp), "%.1f", temperatureRead());
    cpu["frequency"] = ESP.getCpuFreqMHz();
    cpu["temperature"] = temp;

    JsonObject heap = doc["heap"].to < JsonObject > ();
    heap["total"] = ESP.getHeapSize();
    heap["free"] = ESP.getFreeHeap();

    JsonObject wifi = doc["wifi"].to < JsonObject > ();
    wifi["ssid"] = WiFi.SSID();
    wifi["bssid"] = WiFi.BSSIDstr();
    wifi["channel"] = WiFi.channel();
    wifi["rssi"] = WiFi.RSSI();

    serializeJson(doc, *response);
    request->send(response);
}

void sysinfo_begin(const char *id)
{
    node_id = id;
}

void sysinfo_serve(AsyncWebServer & server, const char *path)
{
    // register ourselves with the server
    server.on(path, HTTP_GET, handleGetRequest);
}
