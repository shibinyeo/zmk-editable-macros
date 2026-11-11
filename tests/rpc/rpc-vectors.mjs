#!/usr/bin/env node
// Request frames for the RPC tests, and a decoder for their replies.
//
//   node tests/rpc/rpc-vectors.mjs gen                 rewrite tests/rpc-*/frames.dtsi
//   node tests/rpc/rpc-vectors.mjs decode <test-name>  print the replies in that test's
//                                                      snapshot, decoded
//
// Both use the TypeScript Studio client itself (the fork with the macro requests), so
// the tests feed the firmware exactly the bytes Studio would send, and the replies are
// checked with the decoder Studio would use. Build the client first
// (`npm run generate && npm run build` in zmk-studio-ts-client). Its location defaults
// to a sibling directory of this module and can be set with ZMK_STUDIO_TS_CLIENT.

import { register } from "node:module";
import { readFileSync, writeFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const here = dirname(fileURLToPath(import.meta.url));
const testsDir = resolve(here, "..");
const clientDir = resolve(process.env.ZMK_STUDIO_TS_CLIENT ?? resolve(here, "../../../zmk-studio-ts-client"));

// The client is compiled for bundlers, which Node is not. Two things need help: its
// imports have no file extensions, and it imports the CommonJS protobufjs runtime as if
// it had named exports.
register(
  "data:text/javascript," +
    encodeURIComponent(`
      let anchor;
      export async function initialize(data) { anchor = data.anchor; }
      export async function resolve(specifier, context, next) {
        if (specifier === "protobufjs/minimal") {
          const shim = 'import { createRequire } from "node:module";' +
            'const m = createRequire(' + JSON.stringify(anchor) + ')("protobufjs/minimal");' +
            'export default m; export const Writer = m.Writer, Reader = m.Reader, util = m.util, configure = m.configure;';
          return { url: "data:text/javascript," + encodeURIComponent(shim), shortCircuit: true };
        }
        try { return await next(specifier, context); }
        catch (e) {
          if (e.code !== "ERR_MODULE_NOT_FOUND" && e.code !== "ERR_UNSUPPORTED_DIR_IMPORT") throw e;
          return next(specifier + ".js", context);
        }
      }`),
  { parentURL: import.meta.url, data: { anchor: pathToFileURL(resolve(clientDir, "package.json")).href } }
);

const lib = (name) => import(pathToFileURL(resolve(clientDir, "lib", name)).href);
const { Request, Response } = await lib("studio.js");
const { get_encoder, get_decoder } = await lib("framing.js");
const { macroStatusToJSON } = await lib("core.js");
const { errorConditionsToJSON } = await lib("meta.js");

// ---- Keys ---------------------------------------------------------------------------

const KEYBOARD = 0x07;
const CONSUMER = 0x0c;
const MOD = { LCTL: 0x01, LSFT: 0x02, LALT: 0x04, LGUI: 0x08, RCTL: 0x10, RSFT: 0x20, RALT: 0x40, RGUI: 0x80 };
/** A step: the same number `&kp` takes. */
const key = (usage, mods = 0, page = KEYBOARD) => ((mods << 24) | (page << 16) | usage) >>> 0;
const letter = (c, mods = 0) => key(0x04 + c.toLowerCase().charCodeAt(0) - 97, mods);
const K = {
  ENTER: key(0x28), TAB: key(0x2b), SPACE: key(0x2c), MINUS: key(0x2d), COMMA: key(0x36), F12: key(0x45),
  N1: key(0x1e), VOL_UP: key(0xe9, 0, CONSUMER),
};
const utf8 = (s) => [...new TextEncoder().encode(s)];

// ---- Framing with the client's own encoder --------------------------------------------

function frame(payload) {
  const parts = [];
  get_encoder().transform(payload, { enqueue: (b) => parts.push(...b), error: (e) => { throw new Error(e); } });
  return Uint8Array.from(parts);
}

function unframe(bytes) {
  const out = [];
  const decoder = get_decoder();
  decoder.transform(Uint8Array.from(bytes), { enqueue: (b) => out.push(b), error: (e) => { throw new Error(e); } });
  return out;
}

const core = (body) => ({ core: body });

/** A request the generated code cannot express, written out by hand. */
const raw = (description, coreBytes) => ({ raw: Uint8Array.from(coreBytes), description });

function encodeRequest(requestId, req) {
  if (req.raw) {
    // Request { request_id = 1; core = 3 { <raw bytes> } }
    return Uint8Array.from([0x08, requestId, 0x1a, req.raw.length, ...req.raw]);
  }
  return Request.encode({ requestId, ...req }).finish();
}

// ---- The tests ----------------------------------------------------------------------

// Twenty steps chosen to stress the wire format. Space, Tab and Minus encode to varints
// whose first byte is 0xAC, 0xAB and 0xAD, the three framing bytes, so they must be
// escaped. A right-GUI modifier sets bit 31, the longest varint. One consumer key.
const signature = [
  letter("h", MOD.LSFT), letter("e"), letter("l"), letter("l"), letter("o"), K.COMMA, K.SPACE, K.TAB, K.MINUS,
  K.MINUS, K.SPACE, letter("a", MOD.RGUI), letter("z", MOD.LCTL | MOD.LSFT | MOD.LALT | MOD.LGUI), K.N1, K.VOL_UP, K.F12,
  letter("t"), K.TAB, K.SPACE, K.ENTER,
];

// 241 steps written in 30 requests of 1 to 16 steps each. The requests add up to several
// times the firmware's 256-byte receive buffer, so they wrap it at many different
// offsets.
const wrapSteps = Array.from({ length: 241 }, (_, k) =>
  key(0x04 + (k % 50), k % 5 === 0 ? MOD.RGUI : k % 3 === 0 ? MOD.LSFT : 0)
);
const wrapWrites = [];
for (let offset = 0, i = 0; offset < wrapSteps.length; i++) {
  const n = Math.min((i % 16) + 1, wrapSteps.length - offset);
  wrapWrites.push(core({ macroWrite: { slot: 0, offset, steps: wrapSteps.slice(offset, offset + n) } }));
  offset += n;
}

const TESTS = {
  "rpc-roundtrip": [
    core({ macroGetInfo: true }),
    core({ macroGetSlot: { slot: 1 } }),
    core({ macroWrite: { slot: 1, offset: 0, steps: signature.slice(0, 16) } }),
    core({ macroWrite: { slot: 1, offset: 16, steps: signature.slice(16) } }),
    core({ macroCommit: { slot: 1, stepCount: signature.length, name: utf8("Signature") } }),
    core({ macroGetSlot: { slot: 1 } }),
    core({ macroRead: { slot: 1, offset: 0 } }),
    core({ macroRead: { slot: 1, offset: 16 } }),
    core({ macroRead: { slot: 1, offset: 20 } }),
    core({ macroGetInfo: true }),
  ],

  "rpc-errors": [
    core({ macroClear: { slot: 2 } }),
    core({ macroGetSlot: { slot: 2 } }),
    core({ macroWrite: { slot: 99, offset: 0, steps: [letter("a")] } }),
    core({ macroWrite: { slot: 0, offset: 5, steps: [letter("a")] } }),
    core({ macroWrite: { slot: 0, offset: 0, steps: [letter("a"), letter("b")] } }),
    core({ macroWrite: { slot: 0, offset: 2, steps: [0x00010030] } }),
    core({ macroCommit: { slot: 0, stepCount: 5, name: [] } }),
    core({ macroCommit: { slot: 0, stepCount: 2, name: [0x41, 0x100] } }),
    core({ macroCommit: { slot: 0, stepCount: 2, name: [0x0a] } }),
    core({ macroCommit: { slot: 99, stepCount: 0, name: [] } }),
    core({ macroClear: { slot: 99 } }),
    core({ macroGetSlot: { slot: 99 } }),
    core({ macroRead: { slot: 99, offset: 0 } }),
    core({ macroGetSlot: { slot: 0 } }),
    core({ macroCommit: { slot: 0, stepCount: 2, name: utf8("ok") } }),
    core({ macroGetSlot: { slot: 0 } }),
    // What firmware does with a core request it has no handler for. This is also what
    // firmware without the module answers to the macro_get_info probe.
    raw("core request 150 (unknown) = true", [0xb0, 0x09, 0x01]),
    // One step more than a write may carry. The firmware cannot decode it and sends no
    // reply; the request after it must still be answered.
    core({ macroWrite: { slot: 0, offset: 0, steps: Array(17).fill(letter("a")) } }),
    core({ macroGetInfo: true }),
  ],

  "rpc-locked": [
    core({ macroGetInfo: true }),
    core({ macroGetSlot: { slot: 0 } }),
    core({ macroRead: { slot: 0, offset: 0 } }),
    core({ macroWrite: { slot: 0, offset: 0, steps: [letter("x")] } }),
    core({ macroCommit: { slot: 0, stepCount: 1, name: [] } }),
    core({ macroClear: { slot: 0 } }),
    // The unlock key is pressed here (see the keymap).
    core({ macroGetSlot: { slot: 0 } }),
    core({ macroRead: { slot: 0, offset: 0 } }),
  ],

  "rpc-ringwrap": [
    ...wrapWrites,
    core({ macroCommit: { slot: 0, stepCount: wrapSteps.length, name: utf8("wrap") } }),
    core({ macroGetSlot: { slot: 0 } }),
    ...Array.from({ length: 8 }, (_, i) => core({ macroRead: { slot: 0, offset: i * 32 } })),
    core({ macroGetInfo: true }),
  ],
};

const EXPECTED_READBACK = { "rpc-roundtrip": { slot: 1, steps: signature }, "rpc-ringwrap": { slot: 0, steps: wrapSteps } };

// ---- gen -----------------------------------------------------------------------------

const hex = (bytes) => [...bytes].map((b) => b.toString(16).padStart(2, "0")).join(" ");

function describe(req) {
  if (req.raw) return req.description;
  return JSON.stringify(req.core, (k, v) =>
    k === "steps" && v.length > 6 ? `${v.length} steps` : k === "name" && v.length ? `${v.length} bytes` : v
  );
}

function generate() {
  for (const [name, requests] of Object.entries(TESTS)) {
    const frames = requests.map((req, id) => frame(encodeRequest(id, req)));
    const lines = [
      "/*",
      " * Generated by tests/rpc/rpc-vectors.mjs with the TypeScript Studio client's own",
      " * encoder and framing. Do not edit; change the script and run `gen` again.",
      " *",
      ...requests.map((req, id) => ` * request ${id}: ${describe(req)}`),
      " */",
      "",
      "&emacro_test {",
      "    rpc-frames = [",
      ...frames.map((f) => `        ${hex(f)}`),
      "    ];",
      "};",
      "",
    ];
    writeFileSync(resolve(testsDir, name, "frames.dtsi"), lines.join("\n"));
    const longest = Math.max(...frames.map((f) => f.length));
    const total = frames.reduce((n, f) => n + f.length, 0);
    console.log(`${name}: ${frames.length} requests, ${total} bytes, longest frame ${longest} bytes`);
  }
}

// ---- decode --------------------------------------------------------------------------

function decodeSnapshot(name) {
  const text = readFileSync(resolve(testsDir, name, "keycode_events.snapshot"), "utf8");
  const replies = [];
  let current = null;
  for (const line of text.split("\n")) {
    let m;
    if ((m = line.match(/^emtest: reply, (\d+) bytes/))) {
      current = { length: Number(m[1]), bytes: [] };
      replies.push(current);
    } else if (current && (m = line.match(/^emtest: {3}([0-9a-f ]+)$/))) {
      current.bytes.push(...m[1].trim().split(" ").map((h) => parseInt(h, 16)));
    } else {
      current = null;
    }
  }

  const readback = [];
  for (const r of replies) {
    if (r.bytes.length !== r.length) throw new Error(`reply says ${r.length} bytes but ${r.bytes.length} were logged`);
    const payloads = unframe(r.bytes);
    if (payloads.length !== 1) throw new Error(`expected one frame per reply, got ${payloads.length}`);
    const msg = Response.decode(payloads[0]);
    if (msg.notification) {
      console.log(`notification: ${JSON.stringify(msg.notification)}`);
      continue;
    }
    const rr = msg.requestResponse;
    let shown;
    if (rr.meta) {
      shown = rr.meta.simpleError !== undefined ? `error ${errorConditionsToJSON(rr.meta.simpleError)}` : "no response";
    } else {
      const c = rr.core;
      const [field] = Object.entries(c).filter(([, v]) => v !== undefined);
      const [kind, value] = field;
      if (value && typeof value === "object" && "status" in value) {
        shown = `${kind}: ${macroStatusToJSON(value.status)}`;
      } else if (kind === "macroRead") {
        if (EXPECTED_READBACK[name]?.slot === value.slot) readback.push(value);
        shown = `${kind}: slot ${value.slot} offset ${value.offset}, ${value.steps.length} steps` +
          (value.steps.length ? ` [${value.steps.slice(0, 4).map((s) => "0x" + s.toString(16)).join(" ")}${value.steps.length > 4 ? " ..." : ""}]` : "");
      } else {
        shown = `${kind}: ${JSON.stringify(value)}`;
      }
    }
    console.log(`reply to request ${rr.requestId}: ${shown}`);
  }

  // Where a test reads a whole macro back, compare it with what was written.
  const expected = EXPECTED_READBACK[name];
  if (expected) {
    const got = [];
    for (const r of readback) for (let i = 0; i < r.steps.length; i++) got[r.offset + i] = r.steps[i];
    const same = got.length === expected.steps.length && got.every((s, i) => s === expected.steps[i]);
    console.log(`read back ${got.length} of ${expected.steps.length} steps from slot ${expected.slot}: ${same ? "IDENTICAL to what was written" : "DIFFERENT from what was written"}`);
    if (!same) process.exitCode = 1;
  }
}

// ---- main ----------------------------------------------------------------------------

const [command, arg] = process.argv.slice(2);
if (command === "gen") {
  generate();
} else if (command === "decode" && arg) {
  decodeSnapshot(arg);
} else {
  console.error("usage: rpc-vectors.mjs gen | decode <test-name>");
  process.exit(2);
}
