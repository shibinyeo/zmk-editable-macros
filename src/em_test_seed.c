/*
 * Copyright (c) 2026 The zmk-editable-macros contributors
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * Test support: fill slots from a `zmk,editable-macro-test` devicetree node at start-up.
 * See dts/bindings/zmk,editable-macro-test.yaml. Not for a keyboard in normal use.
 */

#define DT_DRV_COMPAT zmk_editable_macro_test

#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include <zmk_editable_macros/em.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

struct em_seed {
    uint8_t slot;
    const char *name;
    const uint32_t *steps;
    uint16_t step_count;
    const uint8_t *raw;
    uint16_t raw_len;
    const char *raw_key;
};

#define SEED_ARRAYS(node)                                                                          \
    IF_ENABLED(DT_NODE_HAS_PROP(node, steps),                                                      \
               (static const uint32_t _CONCAT(seed_steps_, node)[] = DT_PROP(node, steps);))       \
    IF_ENABLED(DT_NODE_HAS_PROP(node, raw),                                                        \
               (static const uint8_t _CONCAT(seed_raw_, node)[] = DT_PROP(node, raw);))

#define SEED_ENTRY(node)                                                                           \
    {                                                                                              \
        .slot = DT_PROP(node, slot),                                                               \
        .name = DT_PROP_OR(node, macro_name, NULL),                                                \
        .steps = COND_CODE_1(DT_NODE_HAS_PROP(node, steps), (_CONCAT(seed_steps_, node)), (NULL)), \
        .step_count = DT_PROP_LEN_OR(node, steps, 0),                                              \
        .raw = COND_CODE_1(DT_NODE_HAS_PROP(node, raw), (_CONCAT(seed_raw_, node)), (NULL)),       \
        .raw_len = DT_PROP_LEN_OR(node, raw, 0),                                                   \
        .raw_key = DT_PROP_OR(node, raw_key, NULL),                                                \
    },

DT_INST_FOREACH_CHILD(0, SEED_ARRAYS)

static const struct em_seed seeds[] = {DT_INST_FOREACH_CHILD(0, SEED_ENTRY)};

/* Deliberately small, so seeding exercises the multi-write path an editor uses. */
#define SEED_CHUNK 16

static void seed_steps(const struct em_seed *s) {
    enum em_status st = em_stage_write(s->slot, 0, NULL, 0);

    for (uint16_t off = 0; st == EM_OK && off < s->step_count; off += SEED_CHUNK) {
        st = em_stage_write(s->slot, off, &s->steps[off], MIN(SEED_CHUNK, s->step_count - off));
    }
    if (st == EM_OK) {
        st = em_stage_commit(s->slot, s->step_count, s->name, s->name ? strlen(s->name) : 0);
    }
    LOG_DBG("emtest: seed slot %u with %u steps: status %d", s->slot, s->step_count, st);
}

static void seed_raw(const struct em_seed *s) {
#if IS_ENABLED(CONFIG_SETTINGS)
    char key[32];

    if (s->raw_key) {
        strncpy(key, s->raw_key, sizeof(key) - 1);
        key[sizeof(key) - 1] = '\0';
    } else {
        snprintk(key, sizeof(key), "emacro/s/%u", s->slot);
    }
    int err = settings_save_one(key, s->raw, s->raw_len);
    LOG_DBG("emtest: stored %u raw bytes at %s: result %d", s->raw_len, key, err);
#else
    LOG_WRN("emtest: raw seed for slot %u needs CONFIG_SETTINGS", s->slot);
#endif
}

static void run_seeds(void) {
    for (size_t i = 0; i < ARRAY_SIZE(seeds); i++) {
        if (seeds[i].raw) {
            seed_raw(&seeds[i]);
        } else {
            seed_steps(&seeds[i]);
        }
    }

#if IS_ENABLED(CONFIG_SETTINGS)
    if (DT_INST_PROP(0, reload)) {
        em_store_reset_ram();
        int err = settings_load_subtree("emacro");
        LOG_DBG("emtest: RAM cleared and settings reloaded: result %d", err);
    }
#endif
}

#if IS_ENABLED(CONFIG_SETTINGS)

/*
 * Seeding saves to settings, so it has to wait until ZMK's main() has initialised and
 * loaded them. A handler's commit callback runs at the end of that load; from there the
 * work is handed to the system work queue, which finishes it before the first key event.
 */
static void seed_work_handler(struct k_work *work) { run_seeds(); }

static K_WORK_DEFINE(seed_work, seed_work_handler);
static bool seeded;

static int seed_after_settings_load(void) {
    if (!seeded) {
        seeded = true;
        k_work_submit(&seed_work);
    }
    return 0;
}

static int seed_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(emacro_test, "emacro_test", NULL, seed_settings_set,
                               seed_after_settings_load, NULL);

#else

static int seed_at_init(void) {
    run_seeds();
    return 0;
}

SYS_INIT(seed_at_init, APPLICATION, 99);

#endif // IS_ENABLED(CONFIG_SETTINGS)

#endif // DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)
