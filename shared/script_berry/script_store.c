/* The script in the "storage" partition:
 *   [header: magic, length, crc32][source text]
 * The text is written first and the header last, so an interrupted save
 * leaves no valid header rather than a half-written script. The script task
 * compiles through a flash mapping (no copy in RAM); only that task maps
 * and writes, the web page reads chunks with esp_partition_read(). */
#include <string.h>

#include "esp_log.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "script.h"
#include "script_internal.h"

#define MAGIC   0x31435342u     /* "BSC1" */
#define HDR     16
#define SECTOR  4096

static const char *TAG = "script_store";

typedef struct {
    uint32_t magic;
    uint32_t len;
    uint32_t crc;
    uint32_t reserved;
} hdr_t;

static const esp_partition_t *s_part;
static const void *s_map;
static esp_partition_mmap_handle_t s_map_handle;

static void unmap(void)
{
    if (s_map) {
        esp_partition_munmap(s_map_handle);
        s_map = NULL;
    }
}

esp_err_t script_store_init(void)
{
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "storage");
    return s_part ? ESP_OK : ESP_ERR_NOT_FOUND;
}

size_t script_max_len(void)
{
    size_t max = s_part ? s_part->size - HDR : 0;
    return max > 32 * 1024 ? 32 * 1024 : max;      /* the compiler needs RAM too */
}

static bool read_hdr(hdr_t *h)
{
    return s_part && esp_partition_read(s_part, 0, h, sizeof(*h)) == ESP_OK && h->magic == MAGIC &&
           h->len > 0 && h->len <= s_part->size - HDR;
}

size_t script_saved_len(void)
{
    hdr_t h;
    return read_hdr(&h) ? h.len : 0;
}

esp_err_t script_read(size_t offset, void *buf, size_t len)
{
    return s_part ? esp_partition_read(s_part, HDR + offset, buf, len) : ESP_ERR_NOT_FOUND;
}

const char *script_store_map(size_t *len)
{
    *len = 0;
    hdr_t h;
    if (!read_hdr(&h)) {
        return NULL;
    }
    if (!s_map && esp_partition_mmap(s_part, 0, s_part->size, ESP_PARTITION_MMAP_DATA,
                                     &s_map, &s_map_handle) != ESP_OK) {
        s_map = NULL;
        return NULL;
    }
    const char *src = (const char *)s_map + HDR;
    if (esp_rom_crc32_le(0, (const uint8_t *)src, h.len) != h.crc) {
        ESP_LOGW(TAG, "stored script fails its checksum");
        return NULL;
    }
    *len = h.len;
    return src;
}

esp_err_t script_store_write(const char *src, size_t len)
{
    if (!s_part) {
        return ESP_ERR_NOT_FOUND;
    }
    if (len > script_max_len()) {
        return ESP_ERR_INVALID_SIZE;
    }
    unmap();                                    /* the mapping would show stale data */
    size_t erase = (HDR + len + SECTOR - 1) / SECTOR * SECTOR;
    esp_err_t err = esp_partition_erase_range(s_part, 0, erase ? erase : SECTOR);
    if (err == ESP_OK && len) {
        err = esp_partition_write(s_part, HDR, src, len);
    }
    if (err == ESP_OK && len) {
        hdr_t h = {
            .magic = MAGIC,
            .len = len,
            .crc = esp_rom_crc32_le(0, (const uint8_t *)src, len),
        };
        err = esp_partition_write(s_part, 0, &h, sizeof(h));
    }
    return err;
}
