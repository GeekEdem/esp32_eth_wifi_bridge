/* Target UART <-> RFC2217 server and USB-Serial/JTAG.
 *
 * Network path: RFC2217 carries baud-rate changes and DTR/RTS, so esptool and
 * idf.py work unmodified (rfc2217://<host>:<port>). RTS drives EN, DTR drives
 * IO0, as on the usual USB-UART auto-reset circuit.
 *
 * USB path: the C3's USB-Serial/JTAG gives firmware neither the host's baud
 * rate nor DTR/RTS, so it runs at a fixed baud and the bootloader is entered
 * with the button (see main.c).
 *
 * Target output is mirrored to both paths.
 */
#include "bridge.h"

#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_pthread.h"
#include "esp_timer.h"
#include "client_watch.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "rfc2217_server.h"
#include "sdkconfig.h"
#include "target_ctl.h"

#define UART_NUM CONFIG_C3PROG_UART_NUM
#define BUF_SIZE 1024

/* RFC 2217 PURGE-DATA values as they come off the wire. rfc2217-server (v0.4.0)
 * casts the raw byte to rfc2217_purge_t, whose constants are 0/1/2, so they must
 * not be compared with RFC2217_PURGE_*; pyserial expects the same value echoed
 * back, so on_purge() returns it unchanged. */
#define PURGE_RECEIVE  1    /* data from the target not yet sent to the client */
#define PURGE_TRANSMIT 2    /* data from the client not yet sent to the target */
#define PURGE_BOTH     3

/* The RFC2217 server threads run above the UART/USB pumps. The receive purge
 * itself is done by uart_rx_task (see on_purge()): uart_read_bytes() holds the
 * UART RX lock for its whole timeout, so uart_flush_input() from another thread
 * could wait for it forever on the single-core C3. */
#define SERVER_PRIORITY 8
#define PUMP_PRIORITY   5

/* RFC2217 "request" values for SET-CONTROL (RFC 2217, section 3). */
#define CONTROL_REQUEST_FLOW 0
#define CONTROL_REQUEST_DTR  7
#define CONTROL_REQUEST_RTS  10

static const char *TAG = "bridge";

static rfc2217_server_t s_server;
static volatile bool s_purge_rx;           /* on_purge() -> uart_rx_task */
static SemaphoreHandle_t s_purge_done;     /* uart_rx_task -> on_purge() */
static volatile bool s_client;
static volatile uint32_t s_activity;

/* Client liveness (client_watch.c, run by client_watch_task): a client that
 * stopped answering is dropped, so the next one can connect (one at a time).
 * The probe also covers a client whose TCP is alive but whose program is
 * stuck. The drop is a hard close (SO_LINGER 0: RST, nothing queued behind
 * the unacknowledged data), done by its own task, never by an esp_timer
 * callback: a socket call there could hold up every timer, IO0 release
 * included. */
#if !CONFIG_LWIP_SO_LINGER
#error "rfc2217_server_disconnect() needs CONFIG_LWIP_SO_LINGER (sdkconfig.defaults)"
#endif
#define PROBE_AFTER_US   ((int64_t)CONFIG_C3PROG_CLIENT_KEEPALIVE_S * 2 / 3 * 1000000)
#define ANSWER_WITHIN_US ((int64_t)CONFIG_C3PROG_CLIENT_KEEPALIVE_S * 1000000 - PROBE_AFTER_US)
static portMUX_TYPE s_watch_lock = portMUX_INITIALIZER_UNLOCKED;
static client_watch_t s_watch;           /* under s_watch_lock */
static int64_t s_connected_us;           /* current client negotiated; 0 = none */
static int64_t s_dropped_us;             /* last drop by the watch; 0 = never */
static uint32_t s_drops;

bool bridge_client_connected(void)
{
    return s_client;
}

void bridge_client_info(bridge_client_info_t *info)
{
    int64_t now = esp_timer_get_time();
    int64_t conn = s_connected_us, drop = s_dropped_us;
    info->connected_s = s_client && conn ? (int32_t)((now - conn) / 1000000) : -1;
    info->dropped_s = drop ? (int32_t)((now - drop) / 1000000) : -1;
    info->drops = s_drops;
}

uint32_t bridge_activity(void)
{
    return s_activity;
}

static void on_connected(void *ctx)
{
    int64_t now = esp_timer_get_time();
    ESP_LOGI(TAG, "RFC2217 client connected");
    taskENTER_CRITICAL(&s_watch_lock);
    client_watch_start(&s_watch, PROBE_AFTER_US, ANSWER_WITHIN_US, rfc2217_server_rx_count(s_server), now);
    s_connected_us = now;
    taskEXIT_CRITICAL(&s_watch_lock);
    s_client = true;
}

static void on_disconnected(void *ctx)
{
    ESP_LOGI(TAG, "RFC2217 client disconnected");
    s_client = false;
    s_connected_us = 0;
    /* Never leave the target held in reset or with IO0 low. */
    target_release();
    uart_set_baudrate(UART_NUM, CONFIG_C3PROG_UART_BAUD);
}

static unsigned on_baudrate(void *ctx, unsigned baudrate)
{
    if (uart_set_baudrate(UART_NUM, baudrate) != ESP_OK) {
        return 0;
    }
    return baudrate;
}

static rfc2217_control_t on_control(void *ctx, rfc2217_control_t requested)
{
    target_ctl_touch();                 /* the client is alive: restart the EN watchdog */
    switch ((int)requested) {
    case RFC2217_CONTROL_SET_RTS:
        target_set_en(true);
        break;
    case RFC2217_CONTROL_CLEAR_RTS:
        target_set_en(false);
        break;
    case RFC2217_CONTROL_SET_DTR:
        target_set_boot(true);
        break;
    case RFC2217_CONTROL_CLEAR_DTR:
        target_set_boot(false);
        break;
    case CONTROL_REQUEST_FLOW:
        return RFC2217_CONTROL_SET_NO_FLOW_CONTROL;
    case CONTROL_REQUEST_DTR:
        return target_boot_asserted() ? RFC2217_CONTROL_SET_DTR : RFC2217_CONTROL_CLEAR_DTR;
    case CONTROL_REQUEST_RTS:
        return target_en_asserted() ? RFC2217_CONTROL_SET_RTS : RFC2217_CONTROL_CLEAR_RTS;
    default:
        break;  /* flow control and break: accepted, not implemented */
    }
    return requested;
}

static rfc2217_purge_t on_purge(void *ctx, rfc2217_purge_t requested)
{
    int what = (int)requested;          /* raw RFC 2217 value, see PURGE_* */
    if (what == PURGE_RECEIVE || what == PURGE_BOTH) {
        /* esptool's reset_input_buffer(): uart_rx_task flushes between two reads
         * (within ~5 ms) and signals back, so the acknowledgement goes out after
         * the flush. Bounded wait: never hang the server thread. */
        xSemaphoreTake(s_purge_done, 0);
        s_purge_rx = true;
        if (xSemaphoreTake(s_purge_done, pdMS_TO_TICKS(200)) != pdTRUE) {
            ESP_LOGW(TAG, "purge: UART reader did not answer");
        }
    }
    /* PURGE_TRANSMIT: the UART driver cannot drop bytes already queued for the
     * target, and waiting for them here would stall the server thread. */
    return requested;
}

static void on_data_received(void *ctx, const uint8_t *data, size_t len)
{
    uart_write_bytes(UART_NUM, data, len);
    s_activity += len;
}

#if CONFIG_C3PROG_CLIENT_KEEPALIVE_S > 0
/* Once a second while a client is connected: probe it after PROBE_AFTER_US of
 * silence, drop it when no answer comes (client_watch.h). */
static void client_watch_task(void *arg)
{
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (!s_client) {
            continue;
        }
        int64_t now = esp_timer_get_time();
        taskENTER_CRITICAL(&s_watch_lock);
        int act = client_watch_step(&s_watch, rfc2217_server_rx_count(s_server), now);
        taskEXIT_CRITICAL(&s_watch_lock);
        if ((act & CLIENT_WATCH_PROBE) && rfc2217_server_probe(s_server) == 0) {
            taskENTER_CRITICAL(&s_watch_lock);
            client_watch_probed(&s_watch, now);
            taskEXIT_CRITICAL(&s_watch_lock);
        }
        if (act & CLIENT_WATCH_DROP) {
            ESP_LOGW(TAG, "RFC2217 client not answering for %d s: dropping it",
                     CONFIG_C3PROG_CLIENT_KEEPALIVE_S);
            s_drops++;
            s_dropped_us = now;
            int err = rfc2217_server_disconnect(s_server);
            ESP_LOGI(TAG, "drop: %s after %lld ms", err == 0 ? "done" : "failed",
                     (long long)((esp_timer_get_time() - now) / 1000));
        }
    }
}
#endif

/* Target -> network and USB. */
static void uart_rx_task(void *arg)
{
    static uint8_t buf[BUF_SIZE];
    while (true) {
        if (s_purge_rx) {
            uart_flush_input(UART_NUM);
            s_purge_rx = false;
            xSemaphoreGive(s_purge_done);
        }
        int len = uart_read_bytes(UART_NUM, buf, sizeof(buf), pdMS_TO_TICKS(5));
        if (len <= 0) {
            continue;
        }
        s_activity += len;
        if (s_client) {
            rfc2217_server_send_data(s_server, buf, len);
        }
        /* Non-blocking: if no USB host is reading, the data is dropped. */
        usb_serial_jtag_write_bytes(buf, len, 0);
    }
}

/* USB -> target. */
static void usb_rx_task(void *arg)
{
    static uint8_t buf[BUF_SIZE];
    while (true) {
        int len = usb_serial_jtag_read_bytes(buf, sizeof(buf), pdMS_TO_TICKS(20));
        if (len > 0) {
            uart_write_bytes(UART_NUM, buf, len);
            s_activity += len;
        }
    }
}

static esp_err_t uart_init(void)
{
    const uart_config_t cfg = {
        .baud_rate = CONFIG_C3PROG_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_RETURN_ON_ERROR(uart_driver_install(UART_NUM, 4096, 4096, 0, NULL, 0), TAG, "uart install");
    ESP_RETURN_ON_ERROR(uart_param_config(UART_NUM, &cfg), TAG, "uart config");
    ESP_RETURN_ON_ERROR(uart_set_pin(UART_NUM, CONFIG_C3PROG_UART_TX_GPIO, CONFIG_C3PROG_UART_RX_GPIO,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE), TAG, "uart pins");
    /* Keep RX idle-high while the target is unpowered. */
    return gpio_pullup_en(CONFIG_C3PROG_UART_RX_GPIO);
}

esp_err_t bridge_start(void)
{
    ESP_RETURN_ON_ERROR(uart_init(), TAG, "uart");
    s_purge_done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_purge_done, ESP_ERR_NO_MEM, TAG, "purge semaphore");

    usb_serial_jtag_driver_config_t usb_cfg = {
        .rx_buffer_size = BUF_SIZE,
        .tx_buffer_size = BUF_SIZE,
    };
    ESP_RETURN_ON_ERROR(usb_serial_jtag_driver_install(&usb_cfg), TAG, "usb install");

    xTaskCreate(uart_rx_task, "uart_rx", 3072, NULL, PUMP_PRIORITY, NULL);
    xTaskCreate(usb_rx_task, "usb_rx", 3072, NULL, PUMP_PRIORITY, NULL);

    /* rfc2217-server v0.4.0 ignores task_priority/task_stack_size below: its
     * threads are pthreads, created with the calling task's pthread config
     * (the connection thread inherits it from the server thread). */
    esp_pthread_cfg_t pt = esp_pthread_get_default_config();
    pt.prio = SERVER_PRIORITY;
    pt.stack_size = 4096;
    pt.inherit_cfg = true;
    pt.thread_name = "rfc2217";
    ESP_RETURN_ON_ERROR(esp_pthread_set_cfg(&pt), TAG, "pthread cfg");

    /* Listens on every interface, so the setup AP works for flashing too. */
    const rfc2217_server_config_t cfg = {
        .ctx = NULL,
        .on_client_connected = on_connected,
        .on_client_disconnected = on_disconnected,
        .on_baudrate = on_baudrate,
        .on_control = on_control,
        .on_purge = on_purge,
        .on_data_received = on_data_received,
        .port = CONFIG_C3PROG_RFC2217_PORT,
        .task_stack_size = 4096,
        .task_priority = SERVER_PRIORITY,
        .task_core_id = 0,
        /* A client that vanished is dropped, so on_disconnected() releases the
         * target and the next client can connect (patched component). */
        .keepalive_idle_s = CONFIG_C3PROG_CLIENT_KEEPALIVE_S ? (CONFIG_C3PROG_CLIENT_KEEPALIVE_S + 2) / 3 : 0,
        .keepalive_interval_s = 2,
        .keepalive_count = CONFIG_C3PROG_CLIENT_KEEPALIVE_S > 3 ? (CONFIG_C3PROG_CLIENT_KEEPALIVE_S * 2 / 3) / 2 : 1,
        /* With room for one pending connection only, a client that gave up while
         * the server was busy (a retried esptool while a dead client is still
         * held) can leave a half-open connection in that slot: the C3's SYN-ACK
         * finds the client's socket closed, Windows' firewall sends no RST, and
         * lwIP keeps it in SYN-RCVD for ~18 s, refusing every new client; the
         * next impatient retry can do the same again (seen on hardware: no new
         * client for 59 s). A few slots keep the queue usable. */
        .listen_backlog = 4,
    };
    ESP_RETURN_ON_ERROR(rfc2217_server_create(&cfg, &s_server), TAG, "rfc2217 create");
    esp_err_t err = rfc2217_server_start(s_server) == 0 ? ESP_OK : ESP_FAIL;
    const esp_pthread_cfg_t def = esp_pthread_get_default_config();
    esp_pthread_set_cfg(&def);          /* later pthreads of this task: defaults again */
    ESP_RETURN_ON_ERROR(err, TAG, "rfc2217 start");
#if CONFIG_C3PROG_CLIENT_KEEPALIVE_S > 0
    /* Below the server threads, so a probe never waits on them for long. */
    xTaskCreate(client_watch_task, "client_watch", 3072, NULL, PUMP_PRIORITY, NULL);
#endif
    ESP_LOGI(TAG, "RFC2217 server on port %d", CONFIG_C3PROG_RFC2217_PORT);
    return ESP_OK;
}
