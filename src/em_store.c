/*
 * Copyright (c) 2026 The zmk-editable-macros contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <string.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/hid.h>
#include <zmk/keys.h>

#include <zmk_editable_macros/em.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

struct em_slot {
    uint16_t count;
    uint16_t generation;
    char name[EM_NAME_MAX_LEN + 1]; /* display name, always NUL-terminated and non-empty */
    uint32_t steps[EM_MAX_STEPS];
};

/*
 * Two locks, taken in this order when both are needed: stage_lock, then table_lock.
 *
 * table_lock guards the slots that playback reads. It is only ever held for a short
 * copy, never across a flash write, because the player takes it from the system work
 * queue.
 *
 * stage_lock guards the staging buffer and serialises writers (the RPC thread, and the
 * settings loader at boot). It is held across the flash write in a commit; the player
 * never takes it.
 */
static K_MUTEX_DEFINE(table_lock);
static K_MUTEX_DEFINE(stage_lock);

static struct em_slot slots[EM_SLOT_COUNT];

static struct em_record stage;
static int stage_slot = -1;
static uint16_t stage_count;

size_t em_format_u8(char *dst, uint8_t value) {
    char digits[3];
    size_t n = 0;

    do {
        digits[n++] = '0' + (value % 10);
        value /= 10;
    } while (value > 0);

    for (size_t i = 0; i < n; i++) {
        dst[i] = digits[n - 1 - i];
    }
    return n;
}

/* A slot without a stored name is shown as "Macro N", counting from 1. */
static void set_default_name(uint8_t slot) {
    static const char prefix[] = "Macro ";
    const size_t prefix_len = sizeof(prefix) - 1;
    char *name = slots[slot].name;

    memcpy(name, prefix, prefix_len);
    name[prefix_len + em_format_u8(&name[prefix_len], slot + 1)] = '\0';
}

BUILD_ASSERT(sizeof("Macro 255") <= EM_NAME_MAX_LEN + 1, "default name must fit the name buffer");

static int em_store_init(void) {
    for (uint8_t i = 0; i < EM_SLOT_COUNT; i++) {
        set_default_name(i);
    }
    return 0;
}

SYS_INIT(em_store_init, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);

bool em_step_is_valid(uint32_t step) {
    uint16_t page = ZMK_HID_USAGE_PAGE(step);
    uint16_t id = ZMK_HID_USAGE_ID(step);

    switch (page) {
    case HID_USAGE_KEY:
        return (id >= HID_USAGE_KEY_KEYBOARD_A && id <= ZMK_HID_KEYBOARD_MAX_USAGE) ||
               (id >= HID_USAGE_KEY_KEYBOARD_LEFTCONTROL && id <= HID_USAGE_KEY_KEYBOARD_RIGHT_GUI);
    case HID_USAGE_CONSUMER:
        return id >= 1 && id <= ZMK_HID_CONSUMER_MAX_USAGE;
    default:
        return false;
    }
}

static bool name_is_valid(const char *name, uint8_t name_len) {
    if (name_len > EM_NAME_MAX_LEN || (name_len > 0 && name == NULL)) {
        return false;
    }
    for (uint8_t i = 0; i < name_len; i++) {
        unsigned char c = (unsigned char)name[i];
        /* No control characters; bytes >= 0x80 are allowed so UTF-8 names work. */
        if (c < 0x20 || c == 0x7F) {
            return false;
        }
    }
    return true;
}

/* A record is acceptable only if every field is consistent with its length. */
static bool record_is_valid(const struct em_record *rec, size_t len) {
    if (len < EM_RECORD_HEADER_SIZE || rec->version != EM_RECORD_VERSION) {
        return false;
    }
    if (rec->step_count > EM_MAX_STEPS || len != EM_RECORD_SIZE(rec->step_count)) {
        return false;
    }
    if (!name_is_valid(rec->name, rec->name_len)) {
        return false;
    }
    for (uint16_t i = 0; i < rec->step_count; i++) {
        if (!em_step_is_valid(rec->steps[i])) {
            return false;
        }
    }
    return true;
}

/* Caller holds stage_lock (the record being applied is usually the staging buffer). */
static void apply_record(uint8_t slot, const struct em_record *rec) {
    struct em_slot *s = &slots[slot];

    k_mutex_lock(&table_lock, K_FOREVER);
    s->count = rec->step_count;
    memcpy(s->steps, rec->steps, 4 * (size_t)rec->step_count);
    if (rec->name_len > 0) {
        memcpy(s->name, rec->name, rec->name_len);
        s->name[rec->name_len] = '\0';
    } else {
        set_default_name(slot);
    }
    s->generation++;
    k_mutex_unlock(&table_lock);
}

uint16_t em_store_step_count(uint8_t slot) {
    if (slot >= EM_SLOT_COUNT) {
        return 0;
    }
    k_mutex_lock(&table_lock, K_FOREVER);
    uint16_t count = slots[slot].count;
    k_mutex_unlock(&table_lock);
    return count;
}

uint32_t em_store_total_steps(void) {
    uint32_t total = 0;

    k_mutex_lock(&table_lock, K_FOREVER);
    for (uint8_t i = 0; i < EM_SLOT_COUNT; i++) {
        total += slots[i].count;
    }
    k_mutex_unlock(&table_lock);
    return total;
}

const char *em_store_name(uint8_t slot) {
    if (slot >= EM_SLOT_COUNT) {
        return "?";
    }
    return slots[slot].name;
}

uint16_t em_store_generation(uint8_t slot) {
    if (slot >= EM_SLOT_COUNT) {
        return 0;
    }
    k_mutex_lock(&table_lock, K_FOREVER);
    uint16_t generation = slots[slot].generation;
    k_mutex_unlock(&table_lock);
    return generation;
}

enum em_fetch em_store_fetch(uint8_t slot, uint16_t generation, uint16_t index, uint32_t *step) {
    enum em_fetch ret;

    if (slot >= EM_SLOT_COUNT) {
        return EM_FETCH_END;
    }

    k_mutex_lock(&table_lock, K_FOREVER);
    const struct em_slot *s = &slots[slot];
    if (s->generation != generation) {
        ret = EM_FETCH_STALE;
    } else if (index >= s->count) {
        ret = EM_FETCH_END;
    } else {
        *step = s->steps[index];
        ret = EM_FETCH_OK;
    }
    k_mutex_unlock(&table_lock);
    return ret;
}

uint16_t em_store_read(uint8_t slot, uint16_t offset, uint32_t *out, uint16_t max) {
    uint16_t n = 0;

    if (slot >= EM_SLOT_COUNT) {
        return 0;
    }

    k_mutex_lock(&table_lock, K_FOREVER);
    const struct em_slot *s = &slots[slot];
    if (offset < s->count) {
        n = MIN(max, s->count - offset);
        memcpy(out, &s->steps[offset], 4 * (size_t)n);
    }
    k_mutex_unlock(&table_lock);
    return n;
}

enum em_status em_stage_write(uint8_t slot, uint16_t offset, const uint32_t *steps,
                              uint16_t count) {
    enum em_status ret = EM_OK;

    if (slot >= EM_SLOT_COUNT) {
        return EM_ERR_INVALID_SLOT;
    }

    k_mutex_lock(&stage_lock, K_FOREVER);

    /* Offset 0 starts over; anything else has to continue the sequence already staged. */
    uint16_t base = (offset == 0) ? 0 : stage_count;
    if (offset != 0 && (stage_slot != slot || offset != stage_count)) {
        ret = EM_ERR_BAD_SEQUENCE;
        goto out;
    }
    if ((uint32_t)base + count > EM_MAX_STEPS) {
        ret = EM_ERR_TOO_LONG;
        goto out;
    }
    for (uint16_t i = 0; i < count; i++) {
        if (!em_step_is_valid(steps[i])) {
            ret = EM_ERR_INVALID_STEP;
            goto out;
        }
    }

    if (count > 0) {
        memcpy(&stage.steps[base], steps, 4 * (size_t)count);
    }
    stage_slot = slot;
    stage_count = base + count;

out:
    k_mutex_unlock(&stage_lock);
    return ret;
}

enum em_status em_stage_commit(uint8_t slot, uint16_t step_count, const char *name,
                               uint8_t name_len) {
    enum em_status ret = EM_OK;

    if (slot >= EM_SLOT_COUNT) {
        return EM_ERR_INVALID_SLOT;
    }

    k_mutex_lock(&stage_lock, K_FOREVER);

    if (stage_slot == slot) {
        if (step_count != stage_count) {
            ret = EM_ERR_BAD_SEQUENCE;
            goto out;
        }
    } else if (step_count != 0) {
        /* Steps were announced but never staged for this slot. */
        ret = EM_ERR_BAD_SEQUENCE;
        goto out;
    }

    if (!name_is_valid(name, name_len)) {
        ret = EM_ERR_INVALID_NAME;
        goto out;
    }

    uint32_t others = em_store_total_steps() - em_store_step_count(slot);
    if (others + step_count > CONFIG_ZMK_EDITABLE_MACRO_TOTAL_STEPS) {
        ret = EM_ERR_NO_SPACE;
        goto out;
    }

    stage.version = EM_RECORD_VERSION;
    stage.name_len = name_len;
    stage.step_count = step_count;
    memset(stage.name, 0, sizeof(stage.name));
    if (name_len > 0) {
        memcpy(stage.name, name, name_len);
    }

    /* Flash first. If it fails, RAM and flash both still hold the previous macro. */
    int err = (step_count == 0 && name_len == 0)
                  ? em_settings_delete(slot)
                  : em_settings_save(slot, &stage, EM_RECORD_SIZE(step_count));
    if (err < 0) {
        LOG_WRN("emacro: slot %u not saved (%d)", slot, err);
        ret = (err == -ENOSPC) ? EM_ERR_NO_SPACE : EM_ERR_STORAGE;
        goto out;
    }

    apply_record(slot, &stage);
    stage_slot = -1;
    stage_count = 0;

out:
    k_mutex_unlock(&stage_lock);
    return ret;
}

enum em_status em_store_clear(uint8_t slot) {
    if (slot >= EM_SLOT_COUNT) {
        return EM_ERR_INVALID_SLOT;
    }

    /* An empty sequence with no name is a clear: it deletes the stored record. */
    enum em_status ret = em_stage_write(slot, 0, NULL, 0);
    if (ret != EM_OK) {
        return ret;
    }
    return em_stage_commit(slot, 0, NULL, 0);
}

void em_store_load(uint8_t slot, size_t len, settings_read_cb read_cb, void *cb_arg) {
    if (slot >= EM_SLOT_COUNT) {
        LOG_WRN("emacro: ignoring stored macro for slot %u (only %d slots)", slot, EM_SLOT_COUNT);
        return;
    }
    if (len < EM_RECORD_HEADER_SIZE || len > sizeof(stage)) {
        LOG_WRN("emacro: ignoring stored macro %u with impossible size %u", slot, (unsigned)len);
        return;
    }

    k_mutex_lock(&stage_lock, K_FOREVER);

    /* The staging buffer doubles as the read buffer, so any staged sequence is dropped. */
    stage_slot = -1;
    stage_count = 0;

    ssize_t got = read_cb(cb_arg, &stage, len);
    if (got != (ssize_t)len) {
        LOG_WRN("emacro: could not read stored macro %u (%d)", slot, (int)got);
    } else if (!record_is_valid(&stage, len)) {
        LOG_WRN("emacro: ignoring stored macro %u, it does not validate", slot);
    } else {
        apply_record(slot, &stage);
        LOG_DBG("emacro: slot %u loaded, %u steps", slot, stage.step_count);
    }

    k_mutex_unlock(&stage_lock);
}

void em_store_reset_ram(void) {
    k_mutex_lock(&stage_lock, K_FOREVER);
    stage_slot = -1;
    stage_count = 0;

    k_mutex_lock(&table_lock, K_FOREVER);
    for (uint8_t i = 0; i < EM_SLOT_COUNT; i++) {
        slots[i].count = 0;
        slots[i].generation++;
        set_default_name(i);
    }
    k_mutex_unlock(&table_lock);

    k_mutex_unlock(&stage_lock);
}

#if !IS_ENABLED(CONFIG_SETTINGS)

/* Without a settings backend macros live in RAM only. */
int em_settings_save(uint8_t slot, const struct em_record *record, size_t len) { return 0; }
int em_settings_delete(uint8_t slot) { return 0; }

#endif
