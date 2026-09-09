#include <string.h>

#include "parse.h"

size_t parse_ieee80211(const uint8_t *data, size_t len, ieee80211_t *info)
{
    if (len < 24)
        return -1;

    uint16_t fc = data[0] | (data[1] << 8);

    uint8_t type    = (fc >> 2) & 0x3;
    uint8_t subtype = (fc >> 4) & 0xf;

    int to_ds   = (fc >> 8) & 1;
    int from_ds = (fc >> 9) & 1;

    if (type != 2) {
        return -1;
    }

    int hdrlen = 24;

    memcpy(info->source_mac, data + 10, 6);
    info->sequence_ctrl = data[22] | (data[23] << 8);

    if (to_ds && from_ds) {
        hdrlen += 6;
    }

    if (subtype & 0x8) {
        hdrlen += 2;
    }

    return hdrlen;
}
