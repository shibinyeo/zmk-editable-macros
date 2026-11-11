# zmk-editable-macros

A ZMK module that adds macros whose key sequences are stored on the keyboard and can be changed without rebuilding or reflashing firmware. ZMK itself is not modified.

**Status: playback and storage work and are tested. There is no editor yet.** The Studio protocol and editor are the next phases; until then slots can only be filled by the bring-up seed described under Testing.

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

## How it works

- **Steps** are the 32-bit value `&kp` takes as its parameter: modifiers, HID usage page and usage id. Keyboard and consumer pages are accepted.
- **Playback** raises the same key events `&kp` raises, one at a time from a delayable work item. It does not use ZMK's shared 64-item behavior queue, which is what limits stock macros to about 32 taps.
- **Storage** is one Zephyr settings value per slot, `emacro/s/<slot>`: a 20-byte header (version, name length, step count, 16 name bytes) followed by little-endian steps. A save writes storage first and changes the slot only if that succeeds. Loading checks every field, and a record that does not validate leaves its slot empty.
- **Editing while playing** stops the playback; the key that is down is released.

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

Two devicetree-only test aids exist for these tests and for first hardware bring-up. A `zmk,editable-macro-test` node fills slots at start-up, and `&emtest` performs store operations from a key press. Neither is built unless a keymap declares it.

## Limits

- Taps only: no holding a key across other keys, no pauses.
- Macros are stored as plain data in the settings partition. ZMK's `settings_reset` firmware erases them along with everything else.
- Macros live on the device that runs the keymap. A dongle and a keyboard half used as central each have their own.
