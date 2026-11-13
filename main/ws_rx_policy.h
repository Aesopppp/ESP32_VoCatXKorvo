#ifndef WS_RX_POLICY_H
#define WS_RX_POLICY_H

#include <stdbool.h>
#include <stddef.h>

/* Up to about four minutes of 60 ms Opus frames, with a separate byte cap.
 * Control messages share FIFO ordering but audio may never consume all slots. */
#define WS_RX_FRAMES 4096u
#define WS_RX_CONTROL_RESERVE 32u
#define WS_RX_AUDIO_BYTES (1024u * 1024u)
#define WS_RX_CONTROL_BYTES (128u * 1024u)

static inline bool ws_rx_audio_fits(size_t queued, size_t bytes, size_t incoming)
{
    return queued < WS_RX_FRAMES - WS_RX_CONTROL_RESERVE &&
           bytes <= WS_RX_AUDIO_BYTES && incoming <= WS_RX_AUDIO_BYTES - bytes;
}

#endif
