#include <stdbool.h>
#include <stdint.h>

#define ITS5_HEADER_LEN 15
#define ITS5_MAX_PAYLOAD 1500

// Frame type, must match ITS5_TYPE_* in the esp32-c5-sniffer .ino
#define ITS5_TYPE_PACKET 0
#define ITS5_TYPE_STATS 1

typedef struct {
    uint8_t type;
    uint32_t sec;
    uint32_t usec;
    uint16_t len;
    uint8_t payload[ITS5_MAX_PAYLOAD];
} its5_frame_t;

// parse a single byte of ITS5 data, returns true if a complete frame was parsed
bool its5_parse(uint8_t c, its5_frame_t *frame);

// reset parser state back to "waiting for a new frame". Used between SD log
// files during replay so a truncated/corrupt file can't leave the parser
// stuck mid-frame for the next live Serial1 byte.
void its5_reset(void);
