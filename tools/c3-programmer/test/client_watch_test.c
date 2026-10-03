/* Host test of client_watch.c.
 * cc -Wall -Wextra -fsanitize=address,undefined -I../main -o /tmp/cwt client_watch_test.c ../main/client_watch.c && /tmp/cwt */
#include <assert.h>
#include <stdio.h>
#include "client_watch.h"

#define S 1000000LL

int main(void)
{
    client_watch_t w;

    /* a client that answers the probe stays */
    client_watch_start(&w, 10 * S, 5 * S, 7, 100 * S);
    for (int t = 101; t < 110; t++) assert(client_watch_step(&w, 7, t * S) == 0);
    assert(client_watch_step(&w, 7, 110 * S) == CLIENT_WATCH_PROBE);
    client_watch_probed(&w, 110 * S);
    assert(client_watch_step(&w, 7, 111 * S) == 0);               /* asked once */
    assert(client_watch_step(&w, 8, 112 * S) == 0);               /* the answer */
    for (int t = 113; t < 122; t++) assert(client_watch_step(&w, 8, t * S) == 0);
    assert(client_watch_step(&w, 8, 122 * S) == CLIENT_WATCH_PROBE);

    /* no answer: dropped 5 s after the probe, once */
    client_watch_start(&w, 10 * S, 5 * S, 0, 0);
    assert(client_watch_step(&w, 0, 10 * S) == CLIENT_WATCH_PROBE);
    client_watch_probed(&w, 10 * S);
    for (int t = 11; t < 15; t++) assert(client_watch_step(&w, 0, t * S) == 0);
    assert(client_watch_step(&w, 0, 15 * S) == CLIENT_WATCH_DROP);
    assert(client_watch_step(&w, 0, 16 * S) == 0);

    /* the probe cannot be queued (a send is stuck): asked again each step,
     * dropped 5 s after probing became due */
    client_watch_start(&w, 10 * S, 5 * S, 0, 0);
    for (int t = 10; t < 15; t++) assert(client_watch_step(&w, 0, t * S) == CLIENT_WATCH_PROBE);
    assert(client_watch_step(&w, 0, 15 * S) == CLIENT_WATCH_DROP);

    /* a probe queued after failed attempts gets its full 5 s */
    client_watch_start(&w, 10 * S, 5 * S, 0, 0);
    assert(client_watch_step(&w, 0, 10 * S) == CLIENT_WATCH_PROBE);   /* not queued */
    assert(client_watch_step(&w, 0, 12 * S) == CLIENT_WATCH_PROBE);
    client_watch_probed(&w, 12 * S);
    assert(client_watch_step(&w, 0, 16 * S) == 0);
    assert(client_watch_step(&w, 0, 17 * S) == CLIENT_WATCH_DROP);

    puts("client_watch: ok");
    return 0;
}
