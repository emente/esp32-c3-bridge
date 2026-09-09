#include <stdio.h>  // for printf
#include <string.h> // for memcpy

#include "its5_parser.h"

static const uint8_t ITS5_HEADER[4] = {'I', 'T', 'S', '5'};
static uint8_t buffer[ITS5_HEADER_LEN + ITS5_MAX_PAYLOAD];
static uint16_t frame_len = 0;
static int position = 0;

bool its5_parse(uint8_t b, its5_frame_t *frame)
{
    switch (position) {
    case 0:
    case 1:
    case 2:
    case 3:
        // header magic
        if (b != ITS5_HEADER[position]) {
            if (position > 0) {
                position = 0;
                return its5_parse(b, frame);
            }
            // position == 0 and byte doesn't match 'I': not a frame start,
            // discard it outright instead of falling through to store it
            // (previously this byte was stored/advanced regardless, so
            // garbage could spuriously "sync" almost anywhere).
            return false;
        }
        break;
    case 14:
        // second byte of length field
        frame_len = buffer[13] + (b << 8);
        printf("frame_len=%d\n", frame_len);
        if ((ITS5_HEADER_LEN + frame_len) > sizeof(buffer)) {
            // won't fit, reset and try again
            position = 0;
            return its5_parse(b, frame);
        }
        break;
    default:
        break;
    }

    // store bytes while there is room in the buffer
    if (position < sizeof(buffer)) {
        buffer[position++] = b;
    }

    // full frame?
    if (position == (ITS5_HEADER_LEN + frame_len)) {
        // end of packet, decode into frame
        frame->type = buffer[4];
        frame->sec = buffer[5] + (buffer[6] << 8) + (buffer[7] << 16) + (buffer[8] << 24);
        frame->usec = buffer[9] + (buffer[10] << 8) + (buffer[11] << 16) + (buffer[12] << 24);
        frame->len = frame_len;
        memcpy(frame->payload, &buffer[ITS5_HEADER_LEN], frame_len);
        // reset index and indicate success
        position = 0;
        return true;
    }
    return false;
}
