/*
 * Copyright (c) 2026 The zmk-editable-macros contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/settings/settings.h>
#include <zephyr/sys/util.h>
#include <zephyr/toolchain.h>

/*
 * Editable macros: a fixed set of slots, each holding a list of key taps.
 *
 * A step is the 32-bit value `&kp` takes as its parameter: implicit modifiers in bits
 * 24-31, HID usage page in bits 16-23 and usage id in bits 0-15. So `&kp LS(A)` and a
 * step of LS(A) are the same number, and neither the firmware nor an editor translates.
 */

#define EM_SLOT_COUNT CONFIG_ZMK_EDITABLE_MACRO_SLOTS
#define EM_MAX_STEPS CONFIG_ZMK_EDITABLE_MACRO_MAX_STEPS
#define EM_NAME_MAX_LEN 16

/* Wire/API status codes. The numeric values are part of the RPC protocol; do not renumber. */
enum em_status {
    EM_OK = 0,
    EM_ERR_INVALID_SLOT = 1,
    /* A write did not continue the staged sequence, or a commit's count did not match it. */
    EM_ERR_BAD_SEQUENCE = 2,
    EM_ERR_TOO_LONG = 3,
    EM_ERR_INVALID_STEP = 4,
    EM_ERR_INVALID_NAME = 5,
    /* Over the total-steps budget, or the settings partition is too full. */
    EM_ERR_NO_SPACE = 6,
    EM_ERR_STORAGE = 7,
};

/*
 * The stored form of one slot, and also the layout of the staging buffer.
 * Little-endian, no padding: 20 header bytes followed by step_count 32-bit steps.
 */
#define EM_RECORD_VERSION 1
#define EM_RECORD_HEADER_SIZE 20

struct em_record {
    uint8_t version;
    uint8_t name_len;
    uint16_t step_count;
    char name[EM_NAME_MAX_LEN]; /* not NUL-terminated; unused bytes are zero */
    uint32_t steps[EM_MAX_STEPS];
};

BUILD_ASSERT(offsetof(struct em_record, steps) == EM_RECORD_HEADER_SIZE,
             "em_record must have no padding before steps");
BUILD_ASSERT(sizeof(struct em_record) == EM_RECORD_HEADER_SIZE + 4 * EM_MAX_STEPS,
             "em_record must have no trailing padding");

#define EM_RECORD_SIZE(step_count) (EM_RECORD_HEADER_SIZE + 4 * (size_t)(step_count))

/**
 * Write @p value in decimal at @p dst, without a terminator. Returns the digit count
 * (1 to 3). Used instead of a printf-family call for the two short strings this module
 * builds, because one of them is built during the settings load at boot, which runs on
 * ZMK's 1 KB main stack.
 */
size_t em_format_u8(char *dst, uint8_t value);

/* ---- Store ------------------------------------------------------------------------- */

/** True if @p step is a key this firmware can send (keyboard or consumer page, in range). */
bool em_step_is_valid(uint32_t step);

/** Number of steps in @p slot; 0 for an empty or out-of-range slot. */
uint16_t em_store_step_count(uint8_t slot);

/** Steps held across all slots. */
uint32_t em_store_total_steps(void);

/**
 * Display name of @p slot. Never NULL and never empty: a slot without a stored name is
 * called "Macro N". The pointer stays valid for the life of the program; the text
 * changes only when the slot is committed, cleared or loaded.
 */
const char *em_store_name(uint8_t slot);

/**
 * The name stored for @p slot, or "" if it has none (em_store_name() then says
 * "Macro N"). Same lifetime as em_store_name().
 */
const char *em_store_custom_name(uint8_t slot);

/** Changes every time the contents of @p slot change. Lets a reader detect an edit. */
uint16_t em_store_generation(uint8_t slot);

enum em_fetch {
    EM_FETCH_OK,
    EM_FETCH_END,   /* index is past the last step */
    EM_FETCH_STALE, /* the slot changed since @p generation was read */
};

/** Fetch one step for playback, failing if the slot was edited since it started. */
enum em_fetch em_store_fetch(uint8_t slot, uint16_t generation, uint16_t index, uint32_t *step);

/** Copy up to @p max steps starting at @p offset. Returns how many were copied. */
uint16_t em_store_read(uint8_t slot, uint16_t offset, uint32_t *out, uint16_t max);

/**
 * Stage steps for @p slot. A write at offset 0 starts a new sequence and discards any
 * earlier staged one; every later write must start exactly where the previous one ended.
 * Nothing is visible to playback until em_stage_commit(). A rejected write changes nothing.
 */
enum em_status em_stage_write(uint8_t slot, uint16_t offset, const uint32_t *steps, uint16_t count);

/**
 * Make the staged sequence the contents of @p slot and persist it.
 *
 * @p step_count must equal the number of steps staged for this slot. A commit of zero
 * steps with nothing staged is allowed and leaves the slot empty with the given name.
 * Storage is written first; the slot changes only if that succeeds, so a failed commit
 * leaves the previous macro in place both in RAM and in flash.
 */
enum em_status em_stage_commit(uint8_t slot, uint16_t step_count, const char *name,
                               uint8_t name_len);

/** Empty @p slot, drop its name and remove its stored record. */
enum em_status em_store_clear(uint8_t slot);

/**
 * Load one stored record into @p slot, reading @p len bytes through @p read_cb.
 * Anything that does not validate leaves the slot empty. Never fails the caller.
 */
void em_store_load(uint8_t slot, size_t len, settings_read_cb read_cb, void *cb_arg);

/** Forget everything held in RAM (stored records are untouched). */
void em_store_reset_ram(void);

/* ---- Persistence (em_settings.c; no-ops when CONFIG_SETTINGS is off) ---------------- */

int em_settings_save(uint8_t slot, const struct em_record *record, size_t len);
int em_settings_delete(uint8_t slot);

/* ---- Player ------------------------------------------------------------------------ */

/**
 * Start playing @p slot, or queue it behind the macro that is playing.
 * If @p slot is the one playing, this cancels it instead.
 */
int em_player_trigger(uint8_t slot, uint32_t tap_ms, uint32_t wait_ms);
