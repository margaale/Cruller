// ota.h on the ESP32-S3: an app image (.bin) into the other OTA slot, with the bootloader's rollback.
// A new image boots on trial (ESP_OTA_IMG_PENDING_VERIFY); any reset before ota_confirm_if_trial()
// goes back to the previous one.

#include "ota.h"

#include <stdio.h>

#include "esp_ota_ops.h"
#include "esp_system.h"

#include "health.h"

static struct {
    const esp_partition_t *target;
    esp_ota_handle_t handle;
    bool open, failed;
    const char *error;
    size_t written;
} ota;

static bool fail(const char *why) {
    ota.failed = true;
    ota.error = why;
    printf("ota: %s\n", why);
    return false;
}

void ota_begin(void) {
    ota_abort();
    ota = (typeof(ota)){0};
}

bool ota_feed(const uint8_t *data, size_t len) {
    if (ota.failed) return false;
    if (!len) return true;
    if (!ota.open) {
        ota.target = esp_ota_get_next_update_partition(NULL);
        if (!ota.target) return fail("no partition to update");
        // Erases as it goes; the first write checks the image header (an ESP32-S3 app).
        if (esp_ota_begin(ota.target, OTA_WITH_SEQUENTIAL_WRITES, &ota.handle) != ESP_OK) return fail("could not start the update");
        ota.open = true;
        printf("ota: writing to %s at 0x%06lx\n", ota.target->label, (unsigned long)ota.target->address);
    }
    const esp_err_t err = esp_ota_write(ota.handle, data, len);
    if (err == ESP_ERR_OTA_VALIDATE_FAILED) return fail("not an ESP32-S3 app image (.bin)");
    if (err != ESP_OK) return fail("flash write failed");
    ota.written += len;
    return true;
}

bool ota_finish(void) {
    if (ota.failed) return false;
    if (!ota.open) return fail("empty image");
    ota.open = false;
    const esp_err_t err = esp_ota_end(ota.handle);
    if (err == ESP_ERR_OTA_VALIDATE_FAILED) return fail("image check failed");
    if (err != ESP_OK) return fail("could not finish the update");
    if (esp_ota_set_boot_partition(ota.target) != ESP_OK) return fail("could not select the new image");
    printf("ota: image complete, %u bytes\n", (unsigned)ota.written);
    return true;
}

void ota_abort(void) {
    if (ota.open) esp_ota_abort(ota.handle);
    ota.open = false;
}

const char *ota_error(void) {
    return ota.error ? ota.error : "ok";
}

void ota_reboot_into_update(void) {
    health_stop_feeding();
    esp_restart();
}

bool ota_is_trial_boot(void) {
    esp_ota_img_states_t st;
    return esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) == ESP_OK &&
        st == ESP_OTA_IMG_PENDING_VERIFY;
}

void ota_confirm_if_trial(void) {
    if (!ota_is_trial_boot()) return;
    const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    printf("ota: update confirmed (%s)\n", esp_err_to_name(err));
}

int ota_boot_partition(void) {
    const esp_partition_t *p = esp_ota_get_running_partition();
    return p && p->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_0 && p->subtype <= ESP_PARTITION_SUBTYPE_APP_OTA_MAX
        ? (int)(p->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_0) : -1;
}

// The names rp2 uses: "flash update" is an image on trial.
const char *ota_last_boot_type(void) {
    return ota_is_trial_boot() ? "flash update" : "normal";
}
