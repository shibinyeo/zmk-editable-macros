/*
 * Copyright (c) 2026 The zmk-editable-macros contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_editable_macro

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>

#include <zmk_editable_macros/em.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

struct behavior_editable_macro_config {
    uint32_t tap_ms;
    uint32_t wait_ms;
};

static int on_editable_macro_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    const struct device *dev = zmk_behavior_get_binding(binding->behavior_dev);
    const struct behavior_editable_macro_config *cfg = dev->config;

    if (binding->param1 >= EM_SLOT_COUNT) {
        LOG_WRN("emacro: slot %u does not exist (%d slots)", binding->param1, EM_SLOT_COUNT);
        return ZMK_BEHAVIOR_OPAQUE;
    }

    em_player_trigger((uint8_t)binding->param1, cfg->tap_ms, cfg->wait_ms);
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_editable_macro_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    /* The whole macro plays from the press; releasing the key does nothing. */
    return ZMK_BEHAVIOR_OPAQUE;
}

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)

/*
 * Tell ZMK Studio that the parameter is one of the slots, each labelled with the
 * macro's current name, so its ordinary key picker shows a dropdown of macro names.
 * The names are read when Studio asks, so a rename shows up the next time it fetches
 * behavior details.
 */
static struct behavior_parameter_value_metadata slot_values[EM_SLOT_COUNT];

static const struct behavior_parameter_metadata_set slot_set = {
    .param1_values = slot_values,
    .param1_values_len = ARRAY_SIZE(slot_values),
};

static int get_editable_macro_parameter_metadata(const struct device *dev,
                                                 struct behavior_parameter_metadata *metadata) {
    ARG_UNUSED(dev);

    for (uint8_t i = 0; i < EM_SLOT_COUNT; i++) {
        slot_values[i] = (struct behavior_parameter_value_metadata){
            .display_name = (char *)em_store_name(i),
            .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
            .value = i,
        };
    }

    metadata->sets_len = 1;
    metadata->sets = &slot_set;
    return 0;
}

#endif // IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)

static const struct behavior_driver_api behavior_editable_macro_driver_api = {
    .binding_pressed = on_editable_macro_pressed,
    .binding_released = on_editable_macro_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .get_parameter_metadata = get_editable_macro_parameter_metadata,
#endif // IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
};

#define EM_INST(n)                                                                                 \
    static const struct behavior_editable_macro_config behavior_editable_macro_config_##n = {      \
        .tap_ms = DT_INST_PROP_OR(n, tap_ms, CONFIG_ZMK_MACRO_DEFAULT_TAP_MS),                     \
        .wait_ms = DT_INST_PROP_OR(n, wait_ms, CONFIG_ZMK_MACRO_DEFAULT_WAIT_MS),                  \
    };                                                                                             \
    BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, NULL, &behavior_editable_macro_config_##n, POST_KERNEL, \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,                                   \
                            &behavior_editable_macro_driver_api);

DT_INST_FOREACH_STATUS_OKAY(EM_INST)
