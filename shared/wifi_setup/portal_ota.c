/* Firmware update through the web page. See portal_ota.h. */
#include "portal_ota.h"

#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"
#include "setup_portal.h"

#define CHUNK 4096
#define HEADER_LEN (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t))
#define RECV_RETRIES 5

static const char *TAG = "ota";

static volatile bool s_busy;
static volatile bool s_pending_verify;
static esp_timer_handle_t s_confirm_timer;

static void confirm_cb(void *arg)
{
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
        s_pending_verify = false;
        ESP_LOGI(TAG, "new firmware confirmed after %d s", PORTAL_OTA_CONFIRM_S);
    }
}

void portal_ota_init(void)
{
    esp_ota_img_states_t state;
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (run && esp_ota_get_state_partition(run, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
        s_pending_verify = true;
        const esp_timer_create_args_t args = { .callback = confirm_cb, .name = "ota_confirm" };
        if (esp_timer_create(&args, &s_confirm_timer) == ESP_OK) {
            esp_timer_start_once(s_confirm_timer, (int64_t)PORTAL_OTA_CONFIRM_S * 1000 * 1000);
        }
        ESP_LOGW(TAG, "running new firmware from %s on probation for %d s", run->label, PORTAL_OTA_CONFIRM_S);
    }
}

size_t portal_ota_status(char *buf, size_t pos, size_t cap)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    return setup_portal_appendf(buf, pos, cap,
                                ",\"ota\":{\"supported\":%s,\"running\":\"%s\",\"maxSize\":%lu,\"probation\":%s}",
                                next ? "true" : "false", run ? run->label : "",
                                next ? (unsigned long)next->size : 0UL, s_pending_verify ? "true" : "false");
}

/* Why the first bytes are not an update for this device (key NULL if fine). */
static portal_msg_t check_header(const uint8_t *buf)
{
    const esp_image_header_t *hdr = (const esp_image_header_t *)buf;
    const esp_app_desc_t *desc =
        (const esp_app_desc_t *)(buf + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t));
    if (hdr->magic != ESP_IMAGE_HEADER_MAGIC || desc->magic_word != ESP_APP_DESC_MAGIC_WORD) {
        return PORTAL_MSG("err.thisIsNotAnUpdate", "This is not an update file. Use the \"…-ota.bin\" image (the full image for address 0x0 is for flashing over a cable only).");
    }
    if (hdr->chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID) {
        return PORTAL_MSG("err.theImageIsForA", "The image is for a different chip");
    }
    const esp_app_desc_t *running = esp_app_get_description();
    if (strncmp(desc->project_name, running->project_name, sizeof(desc->project_name)) != 0) {
        return PORTAL_MSG("err.theImageIsADifferent", "The image is a different firmware (not for this device)");
    }
    return (portal_msg_t){ 0 };
}

/* Fill buf with up to len bytes; returns bytes read or -1. */
static int recv_some(httpd_req_t *req, uint8_t *buf, size_t len)
{
    for (int tries = 0; tries < RECV_RETRIES; tries++) {
        int r = httpd_req_recv(req, (char *)buf, len);
        if (r > 0) {
            return r;
        }
        if (r != HTTPD_SOCK_ERR_TIMEOUT) {
            return -1;
        }
    }
    return -1;
}

static esp_err_t ota_fail(httpd_req_t *req, esp_ota_handle_t h, uint8_t *buf, portal_msg_t message)
{
    if (h) {
        esp_ota_abort(h);
    }
    free(buf);
    s_busy = false;
    ESP_LOGW(TAG, "update rejected: %s", message.en);
    return setup_portal_send_msg(req, message);
}

esp_err_t portal_ota_post(httpd_req_t *req)
{
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        return setup_portal_send_error_key(req, "err.thisFirmwareHasNoUpdate", "This firmware has no update partitions: flash it over a cable");
    }
    if (req->content_len < HEADER_LEN || req->content_len > part->size) {
        return setup_portal_send_error_key(req, "err.theFileSizeDoesNot", "The file size does not fit an update");
    }
    if (s_busy) {
        return setup_portal_send_error_key(req, "err.anUpdateIsAlreadyIn", "An update is already in progress");
    }
    s_busy = true;

    uint8_t *buf = malloc(CHUNK);
    if (!buf) {
        s_busy = false;
        return httpd_resp_send_500(req);
    }
    esp_ota_handle_t h = 0;
    size_t total = req->content_len, done = 0, have = 0;

    /* Validate the header before touching the flash. */
    while (have < HEADER_LEN) {
        int r = recv_some(req, buf + have, CHUNK - have);
        if (r < 0) {
            return ota_fail(req, 0, buf, PORTAL_MSG("err.theTransferWasInterrupted", "The transfer was interrupted"));
        }
        have += r;
    }
    portal_msg_t why = check_header(buf);
    if (why.key) {
        return ota_fail(req, 0, buf, why);
    }
    char version[33];
    strlcpy(version, ((const esp_app_desc_t *)(buf + sizeof(esp_image_header_t) +
                                                sizeof(esp_image_segment_header_t)))->version, sizeof(version));

    ESP_LOGI(TAG, "writing %u bytes, version %s, to %s", (unsigned)total, version, part->label);
    if (esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &h) != ESP_OK) {
        return ota_fail(req, 0, buf, PORTAL_MSG("err.couldNotStartWriting", "Could not start writing"));
    }
    while (true) {
        if (esp_ota_write(h, buf, have) != ESP_OK) {
            return ota_fail(req, h, buf, PORTAL_MSG("err.flashWriteError", "Flash write error"));
        }
        done += have;
        if (done >= total) {
            break;
        }
        size_t want = total - done < CHUNK ? total - done : CHUNK;
        int r = recv_some(req, buf, want);
        if (r < 0) {
            return ota_fail(req, h, buf, PORTAL_MSG("err.theTransferWasInterrupted", "The transfer was interrupted"));
        }
        have = r;
    }
    free(buf);

    esp_err_t err = esp_ota_end(h);
    if (err != ESP_OK) {
        s_busy = false;
        return setup_portal_send_msg(req, err == ESP_ERR_OTA_VALIDATE_FAILED
                                            ? PORTAL_MSG("err.theImageIsCorruptedVerification", "The image is corrupted (verification failed)")
                                            : PORTAL_MSG("err.couldNotFinishWriting", "Could not finish writing"));
    }
    if (esp_ota_set_boot_partition(part) != ESP_OK) {
        s_busy = false;
        return setup_portal_send_error_key(req, "err.couldNotSelectTheNew", "Could not select the new firmware to boot");
    }
    ESP_LOGI(TAG, "update to %s written, restarting", version);

    char resp[96];
    size_t pos = setup_portal_appendf(resp, 0, sizeof(resp), "{\"ok\":true,\"version\":");
    pos = setup_portal_json_str(resp, pos, sizeof(resp) - 2, version);
    setup_portal_appendf(resp, pos, sizeof(resp), "}");
    setup_portal_restart_later();
    return setup_portal_send_json(req, resp);
}
