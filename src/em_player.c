/*
 * Copyright (c) 2026 The zmk-editable-macros contributors
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * Playback of an editable macro.
 *
 * Stock ZMK macros queue two items per tap into a shared 64-entry queue before the
 * first key is sent, and silently drop what does not fit. That caps a macro at about 32
 * taps. This player keeps one small state record instead and walks the stored steps one
 * event at a time from a delayable work item, so length is limited only by the store.
 *
 * Each step is sent exactly as `&kp` sends it: a zmk_keycode_state_changed press, then
 * a release tap-ms later, then the next step wait-ms after that. All playback runs on
 * the system work queue, the same context the stock behavior queue uses.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/events/keycode_state_changed.h>

#include <zmk_editable_macros/em.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

struct em_request {
    uint32_t tap_ms;
    uint32_t wait_ms;
    uint8_t slot;
};

K_MSGQ_DEFINE(em_requests, sizeof(struct em_request), CONFIG_ZMK_EDITABLE_MACRO_QUEUE_SIZE, 4);

static void em_player_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(em_player_work, em_player_work_handler);

/* `active`, `slot` and `cancel` are read by em_player_trigger(), which may be called
 * from another thread; the spinlock covers those three. Everything else is touched only
 * by the work handler. */
static struct k_spinlock state_lock;

static struct {
    bool active;
    bool cancel;
    uint8_t slot;

    uint32_t tap_ms;
    uint32_t wait_ms;
    uint16_t generation;
    uint16_t index;
    uint16_t taps;
    bool key_down;
    uint32_t down_step;
    int64_t next_due;
} st;

static void schedule_next(uint32_t delay_ms) {
    st.next_due = k_uptime_get() + delay_ms;
    k_work_reschedule(&em_player_work, K_MSEC(delay_ms));
}

/* A key this player pressed is always released by this player, whatever ends the macro. */
static void release_held_key(void) {
    if (st.key_down) {
        raise_zmk_keycode_state_changed_from_encoded(st.down_step, false, k_uptime_get());
        st.key_down = false;
    }
}

static void finish(void) {
    k_spinlock_key_t key = k_spin_lock(&state_lock);
    st.active = false;
    st.cancel = false;
    k_spin_unlock(&state_lock, key);
}

static bool start_next_request(void) {
    struct em_request req;

    if (k_msgq_get(&em_requests, &req, K_NO_WAIT) != 0) {
        return false;
    }

    st.tap_ms = req.tap_ms;
    st.wait_ms = req.wait_ms;
    st.generation = em_store_generation(req.slot);
    st.index = 0;
    st.taps = 0;
    st.key_down = false;
    st.next_due = 0;

    k_spinlock_key_t key = k_spin_lock(&state_lock);
    st.slot = req.slot;
    st.cancel = false;
    st.active = true;
    k_spin_unlock(&state_lock, key);

    LOG_DBG("emacro: slot %u start, %u steps", req.slot, em_store_step_count(req.slot));
    return true;
}

static void em_player_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    for (;;) {
        k_spinlock_key_t key = k_spin_lock(&state_lock);
        bool active = st.active;
        bool cancel = st.cancel;
        k_spin_unlock(&state_lock, key);

        if (!active) {
            if (!start_next_request()) {
                return;
            }
            continue;
        }

        if (cancel) {
            release_held_key();
            LOG_DBG("emacro: slot %u cancelled after %u taps", st.slot, st.taps);
            finish();
            continue;
        }

        /* Ran before the current delay was up (a trigger from another thread can cause
         * that). Go back to waiting so a tap is never cut short. */
        int64_t now = k_uptime_get();
        if (now < st.next_due) {
            k_work_reschedule(&em_player_work, K_MSEC(st.next_due - now));
            return;
        }

        if (st.key_down) {
            release_held_key();
            st.index++;
            schedule_next(st.wait_ms);
            return;
        }

        uint32_t step;
        switch (em_store_fetch(st.slot, st.generation, st.index, &step)) {
        case EM_FETCH_OK:
            raise_zmk_keycode_state_changed_from_encoded(step, true, now);
            st.key_down = true;
            st.down_step = step;
            st.taps++;
            schedule_next(st.tap_ms);
            return;
        case EM_FETCH_STALE:
            /* The slot was saved or cleared mid-playback. Stop rather than splice the
             * old macro onto the new one. No key is down at this point. */
            LOG_DBG("emacro: slot %u changed during playback, stopped after %u taps", st.slot,
                    st.taps);
            finish();
            continue;
        case EM_FETCH_END:
        default:
            LOG_DBG("emacro: slot %u done, %u taps", st.slot, st.taps);
            finish();
            continue;
        }
    }
}

int em_player_trigger(uint8_t slot, uint32_t tap_ms, uint32_t wait_ms) {
    if (slot >= EM_SLOT_COUNT) {
        return -EINVAL;
    }

    k_spinlock_key_t key = k_spin_lock(&state_lock);
    bool active = st.active;
    bool is_playing = active && st.slot == slot;
    if (is_playing) {
        st.cancel = true;
    }
    k_spin_unlock(&state_lock, key);

    if (is_playing) {
        /* Pressing the key of the macro that is playing stops it. */
        k_work_reschedule(&em_player_work, K_NO_WAIT);
        return 0;
    }

    struct em_request req = {.slot = slot, .tap_ms = tap_ms, .wait_ms = wait_ms};
    if (k_msgq_put(&em_requests, &req, K_NO_WAIT) != 0) {
        LOG_WRN("emacro: slot %u not queued, %d macros already waiting", slot,
                CONFIG_ZMK_EDITABLE_MACRO_QUEUE_SIZE);
        return -ENOMEM;
    }

    if (!active) {
        k_work_schedule(&em_player_work, K_NO_WAIT);
    }
    return 0;
}
