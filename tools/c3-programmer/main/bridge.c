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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "rfc2217_server.h"
#include "sdkconfig.h"
#include "target_ctl.h"

#define UART_NUM CONFIG_C3PROG_UART_NUM
#define BUF_SIZE 1024

/* RFC2217 "request" values for SET-CONTROL (RFC 2217, section 3). */
#define CONTROL_REQUEST_FLOW 0
#define CONTROL_REQUEST_DTR  7
#define CONTROL_REQUEST_RTS  10

static const char *TAG = "bridge";

static rfc2217_server_t s_server;
static volatile bool s_client;
static volatile uint32_t s_activity;

bool bridge_client_connected(void)
{
    return s_client;
}

uint32_t bridge_activity(void)
{
    return s_activity;
}

static void on_connected(void *ctx)
{
    ESP_LOGI(TAG, "RFC2217 client connected");
    s_client = true;
}

static void on_disconnected(void *ctx)
{
    ESP_LOGI(TAG, "RFC2217 client disconnected");
    s_client = false;
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
    if (requested == RFC2217_PURGE_RECEIVE || requested == RFC2217_PURGE_BOTH) {
        uart_flush_input(UART_NUM);
    }
    if (requested == RFC2217_PURGE_TRANSMIT || requested == RFC2217_PURGE_BOTH) {
        uart_wait_tx_done(UART_NUM, pdMS_TO_TICKS(1000));
    }
    return requested;
}

static void on_data_received(void *ctx, const uint8_t *data, size_t len)
{
    uart_write_bytes(UART_NUM, data, len);
    s_activity += len;
}

/* Target -> network and USB. */
static void uart_rx_task(void *arg)
{
    static uint8_t buf[BUF_SIZE];
    while (true) {
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

    usb_serial_jtag_driver_config_t usb_cfg = {
        .rx_buffer_size = BUF_SIZE,
        .tx_buffer_size = BUF_SIZE,
    };
    ESP_RETURN_ON_ERROR(usb_serial_jtag_driver_install(&usb_cfg), TAG, "usb install");

    xTaskCreate(uart_rx_task, "uart_rx", 3072, NULL, 10, NULL);
    xTaskCreate(usb_rx_task, "usb_rx", 3072, NULL, 9, NULL);

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
        .task_priority = 8,
        .task_core_id = 0,
    };
    ESP_RETURN_ON_ERROR(rfc2217_server_create(&cfg, &s_server), TAG, "rfc2217 create");
    ESP_RETURN_ON_ERROR(rfc2217_server_start(s_server), TAG, "rfc2217 start");
    ESP_LOGI(TAG, "RFC2217 server on port %d", CONFIG_C3PROG_RFC2217_PORT);
    return ESP_OK;
}
