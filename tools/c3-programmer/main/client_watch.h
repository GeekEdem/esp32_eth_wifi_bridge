/* When to probe and when to drop an RFC2217 client that went quiet. Pure C
 * (no ESP-IDF headers), so the host tests can use it.
 *
 * Called about once a second while a client is connected, with the server's
 * receive counter (rfc2217_server_rx_count()): after probe_after_us without
 * anything from the client it asks for a probe; when no answer has come within
 * answer_within_us of the probe (or since probing became due, if the probe
 * could not be queued because a send is stuck on the connection) it asks for
 * the client to be dropped. */
#pragma once

#include <stdint.h>

typedef struct {
    int64_t probe_after_us;
    int64_t answer_within_us;
    uint32_t rx_seen;       /* receive counter at rx_at */
    int64_t rx_at;          /* last time the client sent anything */
    int64_t probe_at;       /* probe queued, waiting for an answer; else 0 */
} client_watch_t;

enum {
    CLIENT_WATCH_PROBE = 1, /* call rfc2217_server_probe(); on success client_watch_probed() */
    CLIENT_WATCH_DROP = 2,  /* call rfc2217_server_disconnect() */
};

/* A new client: start counting from now. */
void client_watch_start(client_watch_t *w, int64_t probe_after_us, int64_t answer_within_us,
                        uint32_t rx, int64_t now_us);
/* One check; returns CLIENT_WATCH_* flags. After DROP it starts over, so a
 * drop is asked for once per silence. */
int client_watch_step(client_watch_t *w, uint32_t rx, int64_t now_us);
/* The probe asked for by the last step was queued at now_us. */
void client_watch_probed(client_watch_t *w, int64_t now_us);
