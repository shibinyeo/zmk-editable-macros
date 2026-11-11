/*
 * Copyright (c) 2026 The zmk-editable-macros contributors
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * Studio requests for editable macros.
 *
 * ZMK has no way for a module to add its own RPC subsystem: the set of subsystems is a
 * closed list in the protobuf definitions. What it does allow is more handlers on an
 * existing subsystem, because handlers are collected from a linker section. So the
 * macro requests are extra members of `core` (see core.proto in the messages fork), and
 * the handlers below register themselves with ZMK's own macro. ZMK is not patched.
 *
 * Everything here runs on ZMK's RPC thread.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <pb_encode.h>
#include <zmk/studio/rpc.h>

#include <zmk_editable_macros/em.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#ifndef zmk_core_Request_macro_get_info_tag
#error "The Studio messages in this build have no macro requests. Use the zmk-studio-messages \
fork that adds them, or set CONFIG_ZMK_EDITABLE_MACRO_STUDIO_RPC=n."
#endif

/*
 * ZMK v0.3 copes badly with a request that does not fit its receive buffer: over USB
 * the extra bytes are dropped, over Bluetooth the receive callback spins forever. The
 * largest request here is a 16-step write, about 112 bytes on the wire once framed and
 * escaped. ZMK's default buffer is 30 bytes, so the build has to raise it.
 */
BUILD_ASSERT(CONFIG_ZMK_STUDIO_RPC_RX_BUF_SIZE >= 256,
             "Editable macros need CONFIG_ZMK_STUDIO_RPC_RX_BUF_SIZE=256 or more");

/* The wire status is the store's status; keep the two enums identical. */
BUILD_ASSERT((int)EM_OK == (int)zmk_core_MacroStatus_MACRO_STATUS_OK &&
                 (int)EM_ERR_INVALID_SLOT == (int)zmk_core_MacroStatus_MACRO_STATUS_ERR_INVALID_SLOT &&
                 (int)EM_ERR_BAD_SEQUENCE == (int)zmk_core_MacroStatus_MACRO_STATUS_ERR_BAD_SEQUENCE &&
                 (int)EM_ERR_TOO_LONG == (int)zmk_core_MacroStatus_MACRO_STATUS_ERR_TOO_LONG &&
                 (int)EM_ERR_INVALID_STEP == (int)zmk_core_MacroStatus_MACRO_STATUS_ERR_INVALID_STEP &&
                 (int)EM_ERR_INVALID_NAME == (int)zmk_core_MacroStatus_MACRO_STATUS_ERR_INVALID_NAME &&
                 (int)EM_ERR_NO_SPACE == (int)zmk_core_MacroStatus_MACRO_STATUS_ERR_NO_SPACE &&
                 (int)EM_ERR_STORAGE == (int)zmk_core_MacroStatus_MACRO_STATUS_ERR_STORAGE,
             "enum em_status and zmk.core.MacroStatus have drifted apart");

BUILD_ASSERT(ARRAY_SIZE(((zmk_core_MacroCommitRequest *)0)->name) >= EM_NAME_MAX_LEN,
             "core.options.in must allow a full-length name in MacroCommitRequest");

#define EM_RPC_PROTOCOL_VERSION 1
#define EM_RPC_MAX_WRITE_STEPS ARRAY_SIZE(((zmk_core_MacroWriteRequest *)0)->steps)
#define EM_RPC_MAX_READ_STEPS 32

#define MACRO_RESPONSE(type, ...) ZMK_RPC_RESPONSE(core, type, __VA_ARGS__)
#define MACRO_RESULT(type, status_)                                                                \
    MACRO_RESPONSE(type, ((zmk_core_MacroResult){.status = (zmk_core_MacroStatus)(status_)}))

/*
 * Replies are encoded after the handler has returned, so anything an encode callback
 * needs has to outlive the handler. One request is handled at a time, so one static
 * buffer is enough.
 */
static struct {
    uint16_t count;
    uint32_t steps[EM_RPC_MAX_READ_STEPS];
} read_reply;

static bool encode_read_steps(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
    ARG_UNUSED(arg);

    for (uint16_t i = 0; i < read_reply.count; i++) {
        if (!pb_encode_tag_for_field(stream, field) ||
            !pb_encode_varint(stream, read_reply.steps[i])) {
            return false;
        }
    }
    return true;
}

static bool encode_slot_name(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
    const char *name = *arg;
    size_t len = strlen(name);

    if (len == 0) {
        return true;
    }
    return pb_encode_tag_for_field(stream, field) &&
           pb_encode_string(stream, (const pb_byte_t *)name, len);
}

/* Unlocked on purpose: it is how a client finds out the feature exists, and it gives
 * away only sizes. Everything that touches macro contents needs Studio unlocked. */
static zmk_studio_Response macro_get_info(const zmk_studio_Request *req) {
    ARG_UNUSED(req);

    zmk_core_MacroInfo info = {
        .protocol_version = EM_RPC_PROTOCOL_VERSION,
        .slot_count = EM_SLOT_COUNT,
        .max_steps = EM_MAX_STEPS,
        .max_name_len = EM_NAME_MAX_LEN,
        .max_write_steps = EM_RPC_MAX_WRITE_STEPS,
        .max_read_steps = EM_RPC_MAX_READ_STEPS,
        .total_steps_budget = CONFIG_ZMK_EDITABLE_MACRO_TOTAL_STEPS,
        .total_steps_used = em_store_total_steps(),
    };

    return MACRO_RESPONSE(macro_get_info, info);
}

static zmk_studio_Response macro_get_slot(const zmk_studio_Request *req) {
    uint32_t slot = req->subsystem.core.request_type.macro_get_slot.slot;

    if (slot >= EM_SLOT_COUNT) {
        return ZMK_RPC_SIMPLE_ERR(GENERIC);
    }

    zmk_core_MacroSlotInfo info = {
        .slot = slot,
        .step_count = em_store_step_count(slot),
        .name = {.funcs.encode = encode_slot_name, .arg = (void *)em_store_custom_name(slot)},
    };

    return MACRO_RESPONSE(macro_get_slot, info);
}

static zmk_studio_Response macro_read(const zmk_studio_Request *req) {
    const zmk_core_MacroReadRequest *r = &req->subsystem.core.request_type.macro_read;

    if (r->slot >= EM_SLOT_COUNT) {
        return ZMK_RPC_SIMPLE_ERR(GENERIC);
    }

    /* An offset at or past the end is not an error; it reads as no steps. */
    read_reply.count = (r->offset > UINT16_MAX)
                           ? 0
                           : em_store_read(r->slot, r->offset, read_reply.steps,
                                           ARRAY_SIZE(read_reply.steps));

    zmk_core_MacroReadResponse resp = {
        .slot = r->slot,
        .offset = r->offset,
        .steps = {.funcs.encode = encode_read_steps},
    };

    return MACRO_RESPONSE(macro_read, resp);
}

static zmk_studio_Response macro_write(const zmk_studio_Request *req) {
    const zmk_core_MacroWriteRequest *w = &req->subsystem.core.request_type.macro_write;

    if (w->slot >= EM_SLOT_COUNT) {
        return MACRO_RESULT(macro_write, EM_ERR_INVALID_SLOT);
    }
    if (w->offset > UINT16_MAX) {
        return MACRO_RESULT(macro_write, EM_ERR_BAD_SEQUENCE);
    }

    return MACRO_RESULT(macro_write, em_stage_write(w->slot, w->offset, w->steps, w->steps_count));
}

static zmk_studio_Response macro_commit(const zmk_studio_Request *req) {
    const zmk_core_MacroCommitRequest *c = &req->subsystem.core.request_type.macro_commit;
    char name[EM_NAME_MAX_LEN];

    if (c->slot >= EM_SLOT_COUNT) {
        return MACRO_RESULT(macro_commit, EM_ERR_INVALID_SLOT);
    }
    if (c->step_count > UINT16_MAX) {
        return MACRO_RESULT(macro_commit, EM_ERR_BAD_SEQUENCE);
    }
    if (c->name_count > EM_NAME_MAX_LEN) {
        return MACRO_RESULT(macro_commit, EM_ERR_INVALID_NAME);
    }

    /* The name arrives as one varint per byte; see the note on requests in core.proto. */
    for (pb_size_t i = 0; i < c->name_count; i++) {
        if (c->name[i] > UINT8_MAX) {
            return MACRO_RESULT(macro_commit, EM_ERR_INVALID_NAME);
        }
        name[i] = (char)c->name[i];
    }

    return MACRO_RESULT(macro_commit, em_stage_commit(c->slot, c->step_count, name, c->name_count));
}

static zmk_studio_Response macro_clear(const zmk_studio_Request *req) {
    uint32_t slot = req->subsystem.core.request_type.macro_clear.slot;

    if (slot >= EM_SLOT_COUNT) {
        return MACRO_RESULT(macro_clear, EM_ERR_INVALID_SLOT);
    }

    return MACRO_RESULT(macro_clear, em_store_clear(slot));
}

ZMK_RPC_SUBSYSTEM_HANDLER(core, macro_get_info, ZMK_STUDIO_RPC_HANDLER_UNSECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(core, macro_get_slot, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(core, macro_read, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(core, macro_write, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(core, macro_commit, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(core, macro_clear, ZMK_STUDIO_RPC_HANDLER_SECURED);
