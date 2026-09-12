#include <stdio.h>  // for printf
#include <string.h> // for memcpy

#include "its5_parser.h"

static const uint8_t ITS5_HEADER[4] = {'I', 'T', 'S', '5'};

bool its5_parse(its5_parser_state_t *state, uint8_t b, its5_frame_t *frame)
{
    switch (state->position) {
    case 0:
    case 1:
    case 2:
    case 3:
        // header magic
        if (b != ITS5_HEADER[state->position]) {
            if (state->position > 0) {
                state->position = 0;
                return its5_parse(state, b, frame);
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
        state->frame_len = state->buffer[13] + (b << 8);
        //printf("frame_len=%d\n", state->frame_len);
        if ((ITS5_HEADER_LEN + state->frame_len) > sizeof(state->buffer)) {
            // won't fit, reset and try again
            state->position = 0;
            return its5_parse(state, b, frame);
        }
        break;
    default:
        break;
    }

    // store bytes while there is room in the buffer
    if (state->position < (int)sizeof(state->buffer)) {
        state->buffer[state->position++] = b;
    }

    // full frame?
    if (state->position == (ITS5_HEADER_LEN + state->frame_len)) {
        // end of packet, decode into frame
        frame->type = state->buffer[4];
        frame->sec = state->buffer[5] + (state->buffer[6] << 8) + (state->buffer[7] << 16) + (state->buffer[8] << 24);
        frame->usec = state->buffer[9] + (state->buffer[10] << 8) + (state->buffer[11] << 16) + (state->buffer[12] << 24);
        frame->len = state->frame_len;
        memcpy(frame->payload, &state->buffer[ITS5_HEADER_LEN], state->frame_len);
        // reset index and indicate success
        state->position = 0;
        return true;
    }
    return false;
}

void its5_reset(its5_parser_state_t *state)
{
    state->position = 0;
}
