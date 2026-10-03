#include "client_watch.h"

void client_watch_start(client_watch_t *w, int64_t probe_after_us, int64_t answer_within_us,
                        uint32_t rx, int64_t now_us)
{
    w->probe_after_us = probe_after_us;
    w->answer_within_us = answer_within_us;
    w->rx_seen = rx;
    w->rx_at = now_us;
    w->probe_at = 0;
}

int client_watch_step(client_watch_t *w, uint32_t rx, int64_t now_us)
{
    if (rx != w->rx_seen) {
        w->rx_seen = rx;
        w->rx_at = now_us;
        w->probe_at = 0;
        return 0;
    }
    if (now_us - w->rx_at < w->probe_after_us) {
        return 0;
    }
    int64_t since = w->probe_at ? w->probe_at : w->rx_at + w->probe_after_us;
    if (now_us - since >= w->answer_within_us) {
        w->rx_at = now_us;
        w->probe_at = 0;
        return CLIENT_WATCH_DROP;
    }
    return w->probe_at ? 0 : CLIENT_WATCH_PROBE;
}

void client_watch_probed(client_watch_t *w, int64_t now_us)
{
    w->probe_at = now_us;
}
