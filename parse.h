#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint8_t source_mac[6];
    uint16_t sequence_ctrl;
} ieee80211_t;

size_t parse_ieee80211(const uint8_t *data, size_t len, ieee80211_t *info);
