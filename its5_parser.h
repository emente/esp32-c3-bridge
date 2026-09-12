#include <stdbool.h>
#include <stdint.h>

#define ITS5_HEADER_LEN 15
// Must be >= esp32-c5-sniffer.ino's MAX_PACKET_LEN (currently 2400, its own
// enqueue()-enforced cap on captured frame size -- see its comment there for
// why that particular value). A real over-the-air broadcast frame can
// legitimately be that large; anything smaller here just means its_parse()
// silently rejects/resyncs on frames the sniffer already accepted and sent,
// which is exactly what a too-small 1500 here used to do.
#define ITS5_MAX_PAYLOAD 2400

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

// One parser's working state (in-progress frame buffer + position). Each
// independent byte stream being parsed (the live Serial1 stream, an SD
// replay file) needs its own instance -- sharing one across two streams
// that can be serviced in the same call stack (e.g. live bytes drained
// from inside a replay loop) would interleave their bytes into a single
// in-progress frame and corrupt both. Zero-initialize (e.g. `= {}`) to get
// a fresh "waiting for a new frame" state.
typedef struct {
    uint8_t buffer[ITS5_HEADER_LEN + ITS5_MAX_PAYLOAD];
    uint16_t frame_len;
    int position;
} its5_parser_state_t;

// parse a single byte of ITS5 data using `state`, returns true if a
// complete frame was parsed (written into `*frame`)
bool its5_parse(its5_parser_state_t *state, uint8_t c, its5_frame_t *frame);

// reset a parser state back to "waiting for a new frame"
void its5_reset(its5_parser_state_t *state);
