/* The C3's RFC2217 server (vendored rfc2217-server + main/client_watch.c) on
 * lwIP from ESP-IDF (unix port, tap interface), driven by run.py. Mirrors
 * bridge.c: a "UART" thread sends the target's log with blocking sends (as
 * uart_rx_task does), RTS set starts a boot log, and a watch thread runs the
 * client check once a second. Prints timestamped events on stdout. */
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "lwip/init.h"
#include "lwip/tcpip.h"
#include "lwip/netif.h"
#include "lwip/netifapi.h"
#include "netif/tapif.h"
#include "client_watch.h"
#include "rfc2217_server.h"

static rfc2217_server_t s_server;
static volatile bool s_client;
static volatile bool s_boot_log;
static client_watch_t s_watch;
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

static int64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void event(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("%.3f ", now_us() / 1e6);
    vprintf(fmt, ap);
    printf("\n");
    fflush(stdout);
    va_end(ap);
}

static void on_connected(void *ctx)
{
    pthread_mutex_lock(&s_lock);
    client_watch_start(&s_watch, 10000000, 5000000, rfc2217_server_rx_count(s_server), now_us());
    s_client = true;
    pthread_mutex_unlock(&s_lock);
    event("connected");
}

static void on_disconnected(void *ctx)
{
    s_client = false;
    s_boot_log = false;
    event("disconnected");
}

static unsigned on_baudrate(void *ctx, unsigned b) { return b; }
static rfc2217_purge_t on_purge(void *ctx, rfc2217_purge_t p) { return p; }
static void on_data(void *ctx, const uint8_t *d, size_t n) {}

static rfc2217_control_t on_control(void *ctx, rfc2217_control_t r)
{
    if (r == RFC2217_CONTROL_SET_RTS) {
        event("rts");
        s_boot_log = true;
    }
    return r;
}

/* The target's UART, read like uart_rx_task does at 115200 baud: ~58 bytes
 * every 5 ms, each sent with a blocking send. On connect, PRE_KB of output
 * (like esptool's read_flash: the window grows); after RTS, a 20 KB boot log,
 * then a short line a second. */
static volatile bool s_pre_done;
static void *uart_thread(void *arg)
{
    static uint8_t buf[1024];
    memset(buf, 'x', sizeof(buf));
    const int pre = getenv("PRE_KB") ? atoi(getenv("PRE_KB")) * 1024 : 0;
    while (true) {
        if (!s_client) {
            s_pre_done = false;
            usleep(10000);
            continue;
        }
        if (!s_pre_done) {
            for (int sent = 0; sent < pre && s_client; sent += sizeof(buf)) {
                rfc2217_server_send_data(s_server, buf, sizeof(buf));
            }
            s_pre_done = true;
            event("pre-transfer sent");
        }
        if (!s_boot_log) {
            usleep(10000);
            continue;
        }
        usleep(300000);                         /* EN released, the target boots */
        for (int sent = 0; sent < 20 * 1024 && s_client; sent += 58) {
            rfc2217_server_send_data(s_server, buf, 58);
            usleep(5000);
        }
        while (s_client) {
            rfc2217_server_send_data(s_server, buf, 64);
            usleep(1000000);
        }
    }
    return NULL;
}

static void *watch_thread(void *arg)
{
    while (true) {
        usleep(1000000);
        if (!s_client) {
            continue;
        }
        pthread_mutex_lock(&s_lock);
        int64_t now = now_us();
        int act = client_watch_step(&s_watch, rfc2217_server_rx_count(s_server), now);
        if ((act & CLIENT_WATCH_PROBE) && rfc2217_server_probe(s_server) == 0) {
            client_watch_probed(&s_watch, now);
            event("probe");
        }
        pthread_mutex_unlock(&s_lock);
        if (act & CLIENT_WATCH_DROP) {
            event("drop");
            int64_t t = now_us();
            rfc2217_server_disconnect(s_server);
            event("disconnect returned after %lld ms", (long long)((now_us() - t) / 1000));
        }
    }
    return NULL;
}

int main(void)
{
    static struct netif netif;
    ip4_addr_t ip, mask, gw;
    tcpip_init(NULL, NULL);
    IP4_ADDR(&ip, 192, 168, 99, 2);
    IP4_ADDR(&mask, 255, 255, 255, 0);
    IP4_ADDR(&gw, 192, 168, 99, 1);
    netifapi_netif_add(&netif, &ip, &mask, &gw, NULL, tapif_init, tcpip_input);
    netifapi_netif_set_default(&netif);
    netifapi_netif_set_up(&netif);
    netifapi_netif_set_link_up(&netif);

    const rfc2217_server_config_t cfg = {
        .on_client_connected = on_connected,
        .on_client_disconnected = on_disconnected,
        .on_baudrate = on_baudrate,
        .on_control = on_control,
        .on_purge = on_purge,
        .on_data_received = on_data,
        .port = 4000,
        /* as bridge.c with the default 15 s; KEEPALIVE=0 turns it off */
        .keepalive_idle_s = getenv("KEEPALIVE") && !atoi(getenv("KEEPALIVE")) ? 0 : 5,
        .keepalive_interval_s = 2,
        .keepalive_count = 5,
    };
    rfc2217_server_create(&cfg, &s_server);
    rfc2217_server_start(s_server);
    pthread_t t1, t2;
    pthread_create(&t1, NULL, uart_thread, NULL);
    pthread_create(&t2, NULL, watch_thread, NULL);
    event("ready");
    pause();
    return 0;
}
