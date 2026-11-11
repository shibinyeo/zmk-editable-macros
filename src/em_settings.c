/*
 * Copyright (c) 2026 The zmk-editable-macros contributors
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * Persistence for editable macros, through the Zephyr settings subsystem only.
 *
 * One value per slot, key "emacro/s/<slot>", holding a struct em_record truncated to
 * the steps in use. A single value means a save is atomic: after a reset the slot holds
 * either the old macro or the new one, never a mixture.
 *
 * Nothing here touches the flash driver directly. The settings partition sits right
 * below the bootloader on the XIAO nRF52840, and going through settings/NVS keeps every
 * write bounds-checked against that partition.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#if IS_ENABLED(CONFIG_SETTINGS_NVS)
#include <zephyr/fs/nvs.h>
#endif

#include <zmk_editable_macros/em.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define EM_SETTINGS_SUBTREE "emacro"
#define EM_SETTINGS_KEY_PREFIX EM_SETTINGS_SUBTREE "/s/"
#define EM_SETTINGS_KEY_MAX sizeof(EM_SETTINGS_KEY_PREFIX "255")

static void slot_key(char key[EM_SETTINGS_KEY_MAX], uint8_t slot) {
    const size_t prefix_len = sizeof(EM_SETTINGS_KEY_PREFIX) - 1;

    memcpy(key, EM_SETTINGS_KEY_PREFIX, prefix_len);
    key[prefix_len + em_format_u8(&key[prefix_len], slot)] = '\0';
}

static int em_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    const char *next;

    if (IS_ENABLED(CONFIG_ZMK_EDITABLE_MACRO_SETTINGS_IGNORE)) {
        return 0;
    }

    /* Unknown keys under our subtree are skipped, never treated as an error: a failure
     * here would be reported for every boot and gain nothing. */
    if (!settings_name_steq(name, "s", &next) || !next) {
        return 0;
    }

    char *end;
    unsigned long slot = strtoul(next, &end, 10);
    if (end == next || *end != '\0' || slot > UINT8_MAX) {
        LOG_WRN("emacro: ignoring stored key with bad slot '%s'", next);
        return 0;
    }

    em_store_load((uint8_t)slot, len, read_cb, cb_arg);
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(emacro, EM_SETTINGS_SUBTREE, NULL, em_settings_set, NULL, NULL);

#if IS_ENABLED(CONFIG_SETTINGS_NVS)

/* Refuse a save that would eat into the space kept free for bonds and keymap changes. */
static int check_free_space(size_t len) {
    void *storage;
    int err = settings_storage_get(&storage);
    if (err < 0) {
        return err;
    }

    ssize_t free_bytes = nvs_calc_free_space((struct nvs_fs *)storage);
    if (free_bytes < 0) {
        return (int)free_bytes;
    }

    /* A stored value costs its data plus two 8-byte allocation entries and the key name. */
    size_t needed = len + EM_SETTINGS_KEY_MAX + 16 + CONFIG_ZMK_EDITABLE_MACRO_STORAGE_RESERVE;
    if ((size_t)free_bytes < needed) {
        LOG_WRN("emacro: %d bytes free in settings, %u needed", (int)free_bytes, (unsigned)needed);
        return -ENOSPC;
    }
    return 0;
}

#else

static int check_free_space(size_t len) { return 0; }

#endif

int em_settings_save(uint8_t slot, const struct em_record *record, size_t len) {
    char key[EM_SETTINGS_KEY_MAX];

    int err = check_free_space(len);
    if (err < 0) {
        return err;
    }

    slot_key(key, slot);
    return settings_save_one(key, record, len);
}

int em_settings_delete(uint8_t slot) {
    char key[EM_SETTINGS_KEY_MAX];

    slot_key(key, slot);
    return settings_delete(key);
}
