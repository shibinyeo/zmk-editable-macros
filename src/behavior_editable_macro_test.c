/*
 * Copyright (c) 2026 The zmk-editable-macros contributors
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * Test-only behavior `&emtest <action>`: performs a store operation when pressed, so a
 * test can edit macros part-way through a scripted key sequence and log the outcome.
 */

#define DT_DRV_COMPAT zmk_behavior_editable_macro_test

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <drivers/behavior.h>
#include <dt-bindings/zmk/keys.h>
#include <zmk/behavior.h>

#include <zmk_editable_macros/em.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

enum {
    EMTEST_COMMIT_ALT = 0, /* replace slot 0 with X, Y and name it "alt" */
    EMTEST_VALIDATION = 1, /* attempt every kind of invalid write and commit */
    EMTEST_CLEAR = 2,      /* clear slot 0 */
    EMTEST_REPORT = 3,     /* log what the slots hold */
};

/* No '>' in any message here: ZMK's test runner strips each log line up to the last "> ". */
static void report(const char *what, enum em_status status) {
    LOG_DBG("emtest: %s: status %d", what, status);
}

/* Slots 0-2 are always listed; later slots only if they hold something. */
static void log_slots(void) {
    for (uint8_t slot = 0; slot < EM_SLOT_COUNT; slot++) {
        if (slot < 3 || em_store_step_count(slot) > 0) {
            LOG_DBG("emtest: slot %u holds %u steps, named '%s'", slot, em_store_step_count(slot),
                    em_store_name(slot));
        }
    }
    LOG_DBG("emtest: %u steps in total", em_store_total_steps());
}

/*
 * Expects slot 0 to hold 3 steps and the build to use MAX_STEPS=8 and TOTAL_STEPS=4,
 * so the length and budget limits are reachable with a handful of steps.
 */
static void run_validation(void) {
    static const uint32_t good[] = {A, B, C, D, E, F, G};
    static const uint32_t not_a_key[] = {0x00010030}; /* HID page 0x01 is not keyboard/consumer */
    static const uint32_t no_usage[] = {0x00070000};  /* keyboard page, usage id 0 */

    report("write to a slot that does not exist", em_stage_write(EM_SLOT_COUNT, 0, good, 2));
    report("write at offset 5 with nothing staged", em_stage_write(1, 5, good, 2));
    report("start a sequence in slot 1", em_stage_write(1, 0, good, 2));
    report("write that leaves a gap", em_stage_write(1, 3, good, 2));
    report("continue the sequence from another slot", em_stage_write(2, 2, good, 2));
    report("write a step on an unsupported HID page", em_stage_write(1, 2, not_a_key, 1));
    report("write a step with usage id 0", em_stage_write(1, 2, no_usage, 1));
    report("write past the per-macro limit", em_stage_write(1, 2, good, 7));
    report("commit with the wrong step count", em_stage_commit(1, 5, NULL, 0));
    report("commit steps to a slot with nothing staged", em_stage_commit(3, 2, NULL, 0));
    report("commit with a 17-byte name", em_stage_commit(1, 2, "12345678901234567", 17));
    report("commit with a control character in the name", em_stage_commit(1, 2, "a\nb", 3));
    report("commit over the total budget", em_stage_commit(1, 2, NULL, 0));
    report("clear a slot that does not exist", em_store_clear(EM_SLOT_COUNT));
    log_slots();
}

static int on_test_pressed(struct zmk_behavior_binding *binding,
                           struct zmk_behavior_binding_event event) {
    static const uint32_t alt[] = {X, Y};

    switch (binding->param1) {
    case EMTEST_COMMIT_ALT:
        report("stage alt", em_stage_write(0, 0, alt, ARRAY_SIZE(alt)));
        report("commit alt", em_stage_commit(0, ARRAY_SIZE(alt), "alt", 3));
        break;
    case EMTEST_VALIDATION:
        run_validation();
        break;
    case EMTEST_CLEAR:
        report("clear slot 0", em_store_clear(0));
        break;
    case EMTEST_REPORT:
        log_slots();
        break;
    default:
        LOG_WRN("emtest: unknown action %u", binding->param1);
        break;
    }
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_test_released(struct zmk_behavior_binding *binding,
                            struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_editable_macro_test_driver_api = {
    .binding_pressed = on_test_pressed,
    .binding_released = on_test_released,
};

BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
                        &behavior_editable_macro_test_driver_api);

#endif // DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)
