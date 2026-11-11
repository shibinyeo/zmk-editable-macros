/*
 * Copyright (c) 2026 The zmk-editable-macros contributors
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * Test support: a stand-in Studio transport.
 *
 * It plays the part of the USB or Bluetooth link. Request frames listed in the
 * `rpc-frames` property of the `zmk,editable-macro-test` node are put into ZMK's RPC
 * receive buffer one at a time, byte for byte as a client would send them, and every
 * reply frame ZMK produces is logged in hex. So a test drives ZMK's real framing,
 * decoding, dispatch, locking and encoding; only the wire is simulated.
 */

#define DT_DRV_COMPAT zmk_editable_macro_test

#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/ring_buffer.h>

#include <zmk/endpoints.h>
#include <zmk/studio/rpc.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#define FRAME_SOF 0xAB
#define FRAME_ESC 0xAC
#define FRAME_EOF 0xAD

/* ---- Replies: drain ZMK's transmit buffer and log each complete frame ------------- */

#define HEX_BYTES_PER_LINE 24

static uint8_t reply[768];
static size_t reply_len;

/* No '>' in the text: ZMK's test runner strips each log line up to the last "> ". */
static void log_reply(void) {
    static const char hex_digits[] = "0123456789abcdef";
    char line[3 * HEX_BYTES_PER_LINE + 1];

    LOG_DBG("emtest: reply, %u bytes", (unsigned)reply_len);
    for (size_t off = 0; off < reply_len; off += HEX_BYTES_PER_LINE) {
        size_t n = MIN(HEX_BYTES_PER_LINE, reply_len - off);
        for (size_t i = 0; i < n; i++) {
            line[3 * i] = hex_digits[reply[off + i] >> 4];
            line[3 * i + 1] = hex_digits[reply[off + i] & 0x0F];
            line[3 * i + 2] = ' ';
        }
        line[3 * n - 1] = '\0';
        LOG_DBG("emtest:   %s", line);
    }
}

/* Called by ZMK on the sending thread each time it adds bytes; the buffer is only 64
 * bytes, so it has to be emptied every time or the sender would wait forever. */
static void test_tx_notify(struct ring_buf *tx_buf, size_t added, bool message_done,
                           void *user_data) {
    ARG_UNUSED(added);
    ARG_UNUSED(user_data);

    uint8_t *chunk;
    uint32_t len;

    while ((len = ring_buf_get_claim(tx_buf, &chunk, ring_buf_capacity_get(tx_buf))) > 0) {
        size_t take = MIN(len, sizeof(reply) - reply_len);
        memcpy(&reply[reply_len], chunk, take);
        reply_len += take;
        ring_buf_get_finish(tx_buf, len);
    }

    if (message_done) {
        log_reply();
        reply_len = 0;
    }
}

static int test_rx_start(void) { return 0; }
static int test_rx_stop(void) { return 0; }

/* native_posix has neither USB nor Bluetooth, and ZMK then reports USB as the selected
 * output, so that is the transport this one claims to be. */
ZMK_RPC_TRANSPORT(emtest, ZMK_TRANSPORT_USB, test_rx_start, test_rx_stop, NULL, test_tx_notify);

/* ---- Requests: feed the listed frames in, one per interval ------------------------- */

#if DT_INST_NODE_HAS_PROP(0, rpc_frames)

static const uint8_t frames[] = DT_INST_PROP(0, rpc_frames);
static size_t next_frame_at;
static unsigned int frame_number;

static void send_next_frame(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(frame_work, send_next_frame);

/* Length of the frame starting at @p start: up to and including the first 0xAD that is
 * not escaped. */
static size_t frame_length(size_t start) {
    bool escaped = false;

    for (size_t i = start + 1; i < sizeof(frames); i++) {
        if (escaped) {
            escaped = false;
        } else if (frames[i] == FRAME_ESC) {
            escaped = true;
        } else if (frames[i] == FRAME_EOF) {
            return i - start + 1;
        }
    }
    return sizeof(frames) - start;
}

static void send_next_frame(struct k_work *work) {
    ARG_UNUSED(work);

    if (next_frame_at >= sizeof(frames)) {
        return;
    }

    size_t len = frame_length(next_frame_at);
    uint32_t put = ring_buf_put(zmk_rpc_get_rx_buf(), &frames[next_frame_at], len);

    LOG_DBG("emtest: request %u, %u bytes", frame_number, (unsigned)len);
    if (put != len) {
        LOG_ERR("emtest: receive buffer took only %u of %u bytes", put, (unsigned)len);
    }
    zmk_rpc_rx_notify();

    frame_number++;
    next_frame_at += len;
    k_work_schedule(&frame_work, K_MSEC(DT_INST_PROP(0, rpc_frame_interval_ms)));
}

void em_test_rpc_start(void) {
    k_work_schedule(&frame_work, K_MSEC(DT_INST_PROP(0, rpc_frame_interval_ms)));
}

#else

void em_test_rpc_start(void) {}

#endif // DT_INST_NODE_HAS_PROP(0, rpc_frames)

#endif // DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)
