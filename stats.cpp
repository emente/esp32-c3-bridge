#include <ArduinoJson.h>
#include "stats.h"

static int last_minute = -1;
static stats_t stats;

static int get_minute(void)
{
    time_t now = time(nullptr);
    struct tm info;
    gmtime_r(&now, &info);
    return info.tm_min;
}

static void handleStatsRequest(AsyncWebServerRequest *request)
{
    AsyncResponseStream *response = request->beginResponseStream("application/json");

    StaticJsonDocument < 1024 > doc;
    doc["timestamp"] = time(nullptr);
    doc["latest"] = stats.latest;
    JsonArray counts = doc["counts"].to < JsonArray > ();
    for (int i = 0; i < 60; i++) {
        int min = (get_minute() + 60 - i) % 60;
        counts.add(stats.counts[min]);
    }
    serializeJson(doc, *response);
    request->send(response);
}

void stats_begin(void)
{
    memset(&stats.counts, 0, sizeof(stats.counts));
    stats.latest = 0;
    last_minute = -1;
}

void stats_serve(AsyncWebServer & server, const char *path)
{
    // register ourselves with the server
    server.on(path, HTTP_GET, handleStatsRequest);
}

void stats_count(int num)
{
    int minute = get_minute();
    stats.counts[minute] += num;
    stats.latest = time(nullptr);
}

// keeps stats updated even when no events happen, call this regularly, at least once per minute
bool stats_update(void)
{
    int minute = get_minute();
    if (minute != last_minute) {
        last_minute = minute;
        // reset count for the new minute
        stats.counts[minute] = 0;
        return true;
    }
    return false;
}

void stats_get(stats_t *statistics)
{
    statistics->latest = stats.latest;
    for (int i = 0; i < 60; i++) {
        int min = (get_minute() + 60 - i) % 60;
        statistics->counts[i] = stats.counts[min];
    }
}
