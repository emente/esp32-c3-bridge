#include <stdbool.h>
#include <ESPAsyncWebServer.h>

void sysinfo_begin(const char *id);
void sysinfo_serve(AsyncWebServer &server, const char *path);

