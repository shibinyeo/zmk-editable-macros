# zmk-editable-macros

A ZMK module that adds macros whose key sequences are stored on the keyboard and can be changed without rebuilding or reflashing firmware. ZMK itself is not modified.

**Status: playback, storage and the Studio protocol work and are tested. There is no editor inside ZMK Studio yet.** Until there is, macros are read and written with a small terminal client that speaks the same protocol.

Built and tested against ZMK v0.3.0 (`edf5c0814f`).

## What it provides

`&emacro <slot>` plays the macro stored in that slot. Each step is one key tap, with or without modifiers, so a slot can hold text or shortcuts. Pressing the same key again while its macro is playing stops it.

| | Default | Kconfig |
|---|---|---|
| Slots | 16 | `CONFIG_ZMK_EDITABLE_MACRO_SLOTS` |
| Steps per macro | 512 | `CONFIG_ZMK_EDITABLE_MACRO_MAX_STEPS` |
| Steps stored in total | 2048 | `CONFIG_ZMK_EDITABLE_MACRO_TOTAL_STEPS` |
| Settings space left free | 4096 bytes | `CONFIG_ZMK_EDITABLE_MACRO_STORAGE_RESERVE` |
| Macros that can wait in line | 4 | `CONFIG_ZMK_EDITABLE_MACRO_QUEUE_SIZE` |

With the defaults the module uses about 36 KB of RAM and 3 KB of flash on an nRF52840.

## Using it

Make the module available to the build (as a west project, or with `-DZMK_EXTRA_MODULES=<path>`), then in the keymap:

```dts
#include <behaviors/editable_macro.dtsi>

&emacro {
    tap-ms = <5>;   /* how long each key is held */
    wait-ms = <5>;  /* pause between keys */
};

/ {
    keymap {
        compatible = "zmk,keymap";
        default_layer {
            bindings = <&emacro 0 &emacro 1 /* ... */>;
        };
    };
};
```

Including the `.dtsi` is what turns the feature on. Without it nothing from this module is compiled and the firmware is byte-identical to a build without the module. On a split keyboard only the central builds the driver.

`tap-ms` and `wait-ms` default to ZMK's stock macro timing (30 ms and 15 ms).

### Editing over the Studio connection

When the firmware is built with ZMK Studio, the module adds six requests to Studio's `core` subsystem for listing, reading, writing and clearing macros. Two things are needed for that:

- **The `zmk-studio-messages` fork** that defines the requests, in place of the stock one. In a west manifest, list a `zmk-studio-messages` project before `zmk` so it takes precedence. To build with the stock messages instead, set `CONFIG_ZMK_EDITABLE_MACRO_STUDIO_RPC=n`; playback still works.
- **`CONFIG_ZMK_STUDIO_RPC_RX_BUF_SIZE=256`** on the device that runs Studio. ZMK's default is 30 bytes, smaller than a macro write, and ZMK v0.3 drops bytes (USB) or hangs (Bluetooth) when a request does not fit. The build fails with a message if this is missing.

Reading or changing macros needs Studio unlocked, like changing the keymap. Only the request that reports what the keyboard supports is answered while locked.

## How it works

- **Steps** are the 32-bit value `&kp` takes as its parameter: modifiers, HID usage page and usage id. Keyboard and consumer pages are accepted.
- **Playback** raises the same key events `&kp` raises, one at a time from a delayable work item. It does not use ZMK's shared 64-item behavior queue, which is what limits stock macros to about 32 taps.
- **Storage** is one Zephyr settings value per slot, `emacro/s/<slot>`: a 20-byte header (version, name length, step count, 16 name bytes) followed by little-endian steps. A save writes storage first and changes the slot only if that succeeds. Loading checks every field, and a record that does not validate leaves its slot empty.
- **Editing while playing** stops the playback; the key that is down is released.

- **The protocol** is described in `core.proto` in the messages fork. A client writes steps to a staging buffer in requests of up to 16, then commits; nothing changes until the commit, and a commit either replaces and stores the whole macro or changes nothing. ZMK has no way for a module to add an RPC subsystem, but it collects handlers from a linker section, so the module registers its handlers on the existing `core` subsystem and ZMK is not patched. Requests contain only varint fields, which keeps them clear of a defect in ZMK v0.3's handling of string and bytes fields.

The module writes flash only through the settings API.

## Recovery build

`CONFIG_ZMK_EDITABLE_MACRO_SETTINGS_IGNORE=y` starts with every slot empty even though macro data is still stored. Stored records are left alone until a slot is saved or cleared.

Firmware without this module ignores the `emacro/` settings keys.

## Testing

The tests use ZMK's snapshot harness on `native_posix_64`, so they run on a PC. From inside the ZMK build container, anywhere in a west workspace with ZMK checked out:

```sh
/path/to/zmk-editable-macros/tests/run.sh              # all tests
/path/to/zmk-editable-macros/tests/run.sh play-512     # one test
```

| Test | What it shows |
|---|---|
| `play-basic`, `play-repeat-key` | Steps play in order as taps; modifiers apply to their own key; consumer keys work |
| `play-512` | A 512-step macro plays completely and in order |
| `cancel-repress` | Pressing the key again stops playback and releases the held key |
| `queue-two-slots`, `interleave-kp` | A second macro waits its turn; normal typing during playback stays balanced |
| `empty-invalid-slot` | Empty and non-existent slots do nothing |
| `commit-during-play` | Saving a playing slot stops it cleanly; the next press plays the new contents |
| `store-validation` | Every kind of invalid write or commit is refused and changes nothing |
| `persist-roundtrip` | What is saved is what loads back, through NVS on simulated flash |
| `persist-corrupt` | Damaged, oversized and foreign records are ignored; valid ones still load |
| `storage-full` | Saves are refused before the settings partition fills; a small save still fits |
| `settings-ignore` | The recovery build starts empty with data still stored |
| `rpc-roundtrip` | A macro written over the Studio protocol reads back identical and plays |
| `rpc-errors` | Every invalid request gets the right refusal and changes nothing; an undecodable request does not break the connection |
| `rpc-locked` | While Studio is locked only the probe is answered; after unlocking, the macro is as it was |
| `rpc-ringwrap` | 241 steps in 30 requests wrap the firmware's receive buffer many times and read back identical |

The `rpc-*` tests run ZMK's real RPC thread, framing, dispatch and locking; only the wire is replaced. Their request frames come from the TypeScript Studio client's own encoder (`tests/rpc/rpc-vectors.mjs gen`), and `tests/rpc/rpc-vectors.mjs decode <test>` decodes the firmware's replies with the same client.

Devicetree-only test aids exist for these tests and for first hardware bring-up. A `zmk,editable-macro-test` node fills slots at start-up and can feed request frames to the RPC thread, and `&emtest` performs store operations from a key press. None of it is built unless a keymap declares it.

## Limits

- Taps only: no holding a key across other keys, no pauses.
- Macros are stored as plain data in the settings partition. ZMK's `settings_reset` firmware erases them along with everything else.
- Macros live on the device that runs the keymap. A dongle and a keyboard half used as central each have their own.
