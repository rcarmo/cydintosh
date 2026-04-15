# Cydintosh web flasher artifacts

This directory contains the browser flasher page and stable firmware artifacts.

## Browser flasher

- `index.html`
- `manifest.json`

## Stable MQTT build artifacts

These files were copied after a successful local build of the shared MQTT home-client firmware:

- `bootloader-stable-mqtt-v1.bin`
- `partitions-stable-mqtt-v1.bin`
- `firmware-stable-mqtt-v1.bin`
- `merged-firmware-stable-mqtt-v1.bin`
- `littlefs-stable-mqtt-v1.bin`

The merged image includes:
- bootloader
- partitions
- app firmware
- patched ROM at `0x210000`

The separate filesystem image should be flashed at:
- `0x230000` → `littlefs-stable-mqtt-v1.bin`

## Notes

- The current `manifest.json` may still point at the generic local build outputs.
- If you want the web flasher to always use the stable snapshot, update `manifest.json` to reference `merged-firmware-stable-mqtt-v1.bin` instead.
